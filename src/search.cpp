/*
  Selfmate: adversarial self-checkmate chess. GPL-3.0-or-later.

  Search techniques adapted after studying Stockfish's search.cpp, tt.cpp,
  history.h and movepick.cpp at effdc3fd3f6de6e75fe1374a51a7a7eed66f880b.
  Stockfish copyright (C) 2004-2026 The Stockfish developers.
  See docs/SEARCH.md, COPYING and vendor/stockfish/AUTHORS.
*/
#include "engine.h"
#include "movegen.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <limits>

namespace Selfmate {
using namespace Stockfish;
namespace {
constexpr int Infinity = 31000;
constexpr int HistoryLimit = 16384;
constexpr int PieceSquareCount = PIECE_NB * SQUARE_NB;
enum Bound : std::uint8_t { Empty, Upper, Lower, Exact };
constexpr std::uint8_t ClaimFlag = 1;

// A full key is retained, rather than Stockfish's compact partial signature.
struct Entry {
    std::uint64_t key = 0;
    std::int16_t value = 0;
    std::uint16_t move = 0;
    std::uint8_t depth = 0;
    std::uint8_t bound = Empty;
    std::uint8_t generation = 0;
    std::uint8_t flags = 0;
};
struct alignas(64) Cluster { Entry entry[4]; };
static_assert(sizeof(Entry) == 16 && sizeof(Cluster) == 64);
struct HintEntry {
    std::uint64_t boardKey = 0;
    Move move = Move::none();
};

std::uint64_t mix(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::uint64_t historyContext(const Position& pos) {
    std::uint64_t context = 0;
    int remaining = std::min(pos.state()->rule50, pos.state()->pliesFromNull);
    const StateInfo* state = pos.state();
    while (state) {
        context += mix(state->key);
        if (remaining-- <= 0) break;
        state = state->previous;
    }
    return context;
}

std::uint64_t childContext(const Position& child, std::uint64_t parentContext) {
    return (child.rule50_count() == 0 ? 0 : parentContext) + mix(child.state()->key);
}

std::uint64_t tableKey(const Position& pos, std::uint64_t context) {
    // A multiset of ALL available past positions, not only the repeat count of
    // the current board, determines which future repetition claims are legal.
    return mix(pos.state()->key ^ mix(context)
               ^ mix(std::uint64_t(pos.rule50_count()) + 0x9e3779b97f4a7c15ULL));
}

int toTable(int score, int ply) {
    return score >= MateThreshold ? score + ply : score <= -MateThreshold ? score - ply : score;
}
int fromTable(int score, int ply) {
    return score >= MateThreshold ? score - ply : score <= -MateThreshold ? score + ply : score;
}

void gravity(std::int16_t& value, int bonus) {
    bonus = std::clamp(bonus, -HistoryLimit, HistoryLimit);
    value = std::int16_t(int(value) + bonus - int(value) * std::abs(bonus) / HistoryLimit);
}
int pieceSquare(Piece piece, Square square) { return int(piece) * SQUARE_NB + int(square); }
} // namespace

struct Searcher::Data {
    struct Frame {
        std::vector<Move> moves;
        std::array<int, MAX_MOVES> scores{};
        std::array<Move, MaxDepth + 1> pv{};
        int pvLength = 0;
        Move bestMove = Move::none();
        bool claimChoice = false;
        Piece previousPiece = NO_PIECE;
        Move previousMove = Move::none();
        Frame() { moves.reserve(MAX_MOVES); }
    };
    std::vector<Cluster> table;
    std::vector<HintEntry> hints;
    std::unique_ptr<Frame[]> frames = std::make_unique<Frame[]>(MaxDepth + 2);
    std::array<std::array<Move, 2>, MaxDepth + 2> killers{};
    std::int16_t history[COLOR_NB][64][64]{};
    std::int16_t captureHistory[PIECE_NB][64][PIECE_TYPE_NB]{};
    std::vector<std::int16_t> continuation = std::vector<std::int16_t>(PieceSquareCount * PieceSquareCount);
    std::array<Move, PieceSquareCount> counterMoves{};
    SearchOptions options;
    SearchStats stats;
    Limits limits;
    std::atomic_bool* stop = nullptr;
    std::chrono::steady_clock::time_point start;
    std::uint64_t nodes = 0;
    std::uint8_t generation = 0;
    bool aborted = false;

    std::int64_t elapsed() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start).count();
    }
    bool shouldStop(bool forceTime = false) {
        if (stop->load(std::memory_order_relaxed) || (limits.nodes && nodes >= limits.nodes)
            || (limits.moveTimeMs > 0 && (forceTime || !(nodes & 255))
                && elapsed() >= limits.moveTimeMs)) {
            aborted = true; return true;
        }
        return false;
    }
    void setHash(int megabytes) {
        megabytes = std::clamp(megabytes, 1, 256);
        std::size_t bytes = std::size_t(megabytes) * 1024 * 1024;
        hints.assign((bytes / 8) / sizeof(HintEntry), HintEntry{});
        table.assign((bytes - bytes / 8) / sizeof(Cluster), Cluster{});
    }
    void clear() {
        std::fill(table.begin(), table.end(), Cluster{});
        std::fill(hints.begin(), hints.end(), HintEntry{});
        for (auto& row : killers) row.fill(Move::none());
        for (auto& color : history) for (auto& from : color) std::fill(std::begin(from), std::end(from), 0);
        for (auto& piece : captureHistory) for (auto& to : piece) std::fill(std::begin(to), std::end(to), 0);
        std::fill(continuation.begin(), continuation.end(), 0);
        counterMoves.fill(Move::none());
        stats = {};
    }

    Move boardHint(const Position& pos, const std::vector<Move>& legal) const {
        if (!options.useTT) return Move::none();
        const HintEntry& hint = hints[pos.state()->key % hints.size()];
        if (hint.boardKey == pos.state()->key
            && std::find(legal.begin(), legal.end(), hint.move) != legal.end()) return hint.move;
        return Move::none();
    }
    void saveHint(const Position& pos, Move move) {
        if (options.useTT && move.is_ok())
            hints[pos.state()->key % hints.size()] = {pos.state()->key, move};
    }

    Entry* probe(std::uint64_t key, int depth, Move& orderMove) {
        if (!options.useTT) return nullptr;
        Entry* matchingDepth = nullptr;
        int orderDepth = -1;
        bool found = false;
        for (Entry& entry : table[key % table.size()].entry) {
            if (entry.bound == Empty || entry.key != key) continue;
            found = true;
            if (entry.move && int(entry.depth) > orderDepth) {
                orderMove = Move(entry.move); orderDepth = entry.depth;
            }
            if (int(entry.depth) == depth) matchingDepth = &entry;
            entry.generation = generation;
        }
        if (found) ++stats.ttHits;
        return matchingDepth;
    }
    void save(std::uint64_t key, int depth, int ply, int score, Bound bound,
              Move move, bool claim) {
        if (!options.useTT) return;
        Entry* replace = nullptr;
        int least = std::numeric_limits<int>::max();
        for (Entry& entry : table[key % table.size()].entry) {
            if (entry.bound == Empty) { replace = &entry; break; }
            if (entry.key == key && int(entry.depth) == depth) {
                // An exact result is more informative than a later bound at
                // the same horizon. Keep its score and claim metadata intact.
                if (entry.bound == Exact && bound != Exact) return;
                replace = &entry; break;
            }
            int age = std::uint8_t(generation - entry.generation);
            int quality = int(entry.depth) + (entry.bound == Exact ? 4 : 0) - 8 * age;
            if (quality < least) { least = quality; replace = &entry; }
        }
        assert(replace);
        *replace = Entry{key, std::int16_t(toTable(score, ply)), move.raw(),
                         std::uint8_t(depth), bound, generation, std::uint8_t(claim ? ClaimFlag : 0)};
    }

    void scoreMoves(const Position& pos, int ply, Move hashMove) {
        Frame& frame = frames[ply];
        int previous = frame.previousPiece != NO_PIECE && frame.previousMove.is_ok()
                     ? pieceSquare(frame.previousPiece, frame.previousMove.to_sq()) : -1;
        for (std::size_t index = 0; index < frame.moves.size(); ++index) {
            Move move = frame.moves[index];
            Piece piece = pos.moved_piece(move);
            int score;
            if (pos.capture(move)) {
                PieceType captured = move.type_of() == EN_PASSANT ? PAWN : type_of(pos.piece_on(move.to_sq()));
                score = captureHistory[piece][move.to_sq()][captured]
                      - PieceValue[make_piece(~pos.side_to_move(), captured)] / 8;
            } else {
                score = 2 * history[pos.side_to_move()][move.from_sq()][move.to_sq()];
                if (previous >= 0)
                    score += continuation[previous * PieceSquareCount + pieceSquare(piece, move.to_sq())];
            }
            if (type_of(piece) == KING) score += 300;
            if (pos.gives_check(move)) score += 400;
            if (move == killers[ply][0]) score += 500000;
            if (move == killers[ply][1]) score += 400000;
            if (previous >= 0 && move == counterMoves[previous]) score += 300000;
            if (move == hashMove) score += 10000000;
            frame.scores[index] = score;
        }
    }
    Move pickMove(Frame& frame, std::size_t index) {
        std::size_t best = index;
        for (std::size_t next = index + 1; next < frame.moves.size(); ++next)
            if (frame.scores[next] > frame.scores[best]) best = next;
        std::swap(frame.moves[index], frame.moves[best]);
        std::swap(frame.scores[index], frame.scores[best]);
        return frame.moves[index];
    }
    void updateMove(const Position& pos, int ply, Move move, int bonus) {
        Piece piece = pos.moved_piece(move);
        if (pos.capture(move)) {
            PieceType captured = move.type_of() == EN_PASSANT ? PAWN : type_of(pos.piece_on(move.to_sq()));
            gravity(captureHistory[piece][move.to_sq()][captured], bonus);
        } else {
            gravity(history[pos.side_to_move()][move.from_sq()][move.to_sq()], bonus);
            const Frame& frame = frames[ply];
            if (frame.previousPiece != NO_PIECE && frame.previousMove.is_ok()) {
                int previous = pieceSquare(frame.previousPiece, frame.previousMove.to_sq());
                gravity(continuation[previous * PieceSquareCount + pieceSquare(piece, move.to_sq())], bonus);
            }
        }
    }
    void updateCutoff(const Position& pos, int depth, int ply, std::size_t index) {
        Frame& frame = frames[ply];
        Move move = frame.moves[index];
        int bonus = std::min(2048, 32 * depth * depth + 32);
        updateMove(pos, ply, move, bonus);
        for (std::size_t prior = 0; prior < index; ++prior) updateMove(pos, ply, frame.moves[prior], -bonus / 2);
        if (!pos.capture(move)) {
            if (move != killers[ply][0]) { killers[ply][1] = killers[ply][0]; killers[ply][0] = move; }
            if (frame.previousPiece != NO_PIECE && frame.previousMove.is_ok())
                counterMoves[pieceSquare(frame.previousPiece, frame.previousMove.to_sq())] = move;
        }
    }
    void setPV(int ply, Move move, bool claim) {
        Frame& frame = frames[ply];
        frame.pv[0] = move;
        frame.pvLength = 1;
        if (!claim) {
            const Frame& child = frames[ply + 1];
            int count = std::min(child.pvLength, MaxDepth - ply);
            std::copy_n(child.pv.begin(), count, frame.pv.begin() + 1);
            frame.pvLength += count;
        }
    }

    int negamax(Position& pos, int depth, int ply, int alpha, int beta, std::uint64_t context) {
        assert(ply <= MaxDepth && alpha < beta);
        Frame& frame = frames[ply];
        frame.pvLength = 0; frame.bestMove = Move::none(); frame.claimChoice = false;
        if (shouldStop()) return 0;
        ++nodes;
        MoveList<LEGAL> legal(pos);
        frame.moves.assign(legal.begin(), legal.end());
        // A checked side with no legal move has WON its selfmate objective.
        // This is tested before automatic draws, including at halfmove 150.
        if (frame.moves.empty()) return pos.checkers() ? Mate - ply : 0;
        if (automaticDraw(pos)) return 0;
        if (ply == 0 && limits.restrictMoves) {
            frame.moves.erase(std::remove_if(frame.moves.begin(), frame.moves.end(), [&](Move move) {
                return std::find(limits.searchMoves.begin(), limits.searchMoves.end(), move) == limits.searchMoves.end();
            }), frame.moves.end());
            if (frame.moves.empty()) return 0;
        }
        int originalAlpha = alpha;
        bool allowTable = !(ply == 0 && limits.restrictMoves);
        std::uint64_t key = tableKey(pos, context);
        Move hashMove = Move::none();
        Entry* cached = allowTable ? probe(key, depth, hashMove) : nullptr;
        // Board-only transpositions may safely suggest a legal move even when
        // their different repetition histories make cached scores unusable.
        // The stronger context-matched hint retains priority when available.
        if (!hashMove.is_ok() && depth > 0) hashMove = boardHint(pos, frame.moves);
        if (cached) {
            int value = fromTable(cached->value, ply);
            bool decisivePV = beta - alpha > 1 && std::abs(value) >= MateThreshold;
            if (!decisivePV && (cached->bound == Exact || (cached->bound == Lower && value >= beta)
                || (cached->bound == Upper && value <= alpha))) {
                ++stats.ttCutoffs;
                Move move(cached->move);
                if (std::find(frame.moves.begin(), frame.moves.end(), move) != frame.moves.end()) {
                    frame.bestMove = move; frame.pv[0] = move; frame.pvLength = 1;
                }
                frame.claimChoice = bool(cached->flags & ClaimFlag);
                return value;
            }
        }
        if (depth <= 0 || ply == MaxDepth) {
            int value = evaluateLeaf(pos, frame.moves);
            if (allowTable) save(key, depth, ply, value, Exact, Move::none(), false);
            return value;
        }
        bool currentClaim = drawClaimAvailable(pos);
        int best = currentClaim ? 0 : -Infinity;
        frame.claimChoice = currentClaim;
        if (currentClaim) alpha = std::max(alpha, 0);
        if (alpha >= beta) {
            if (allowTable) save(key, depth, ply, best, Lower, Move::none(), true);
            return best;
        }
        scoreMoves(pos, ply, hashMove);
        for (std::size_t index = 0; index < frame.moves.size(); ++index) {
            Move move = pickMove(frame, index);
            Piece piece = pos.moved_piece(move);
            StateInfo state;
            pos.do_move(move, state, nullptr);
            bool intendedClaim = drawClaimAvailable(pos);
            std::uint64_t nextContext = childContext(pos, context);
            frames[ply + 1].previousPiece = piece;
            frames[ply + 1].previousMove = move;
            int value;
            if (options.usePVS && index > 0 && beta - alpha > 1) {
                value = -negamax(pos, depth - 1, ply + 1, -alpha - 1, -alpha, nextContext);
                if (intendedClaim) value = std::max(value, 0);
                // Scouts never reduce depth. An improvement inside the open
                // window MUST be re-searched with the original full window.
                if (!aborted && value > alpha && value < beta) {
                    ++stats.pvsResearches;
                    value = -negamax(pos, depth - 1, ply + 1, -beta, -alpha, nextContext);
                }
            } else value = -negamax(pos, depth - 1, ply + 1, -beta, -alpha, nextContext);
            bool useClaim = intendedClaim && value <= 0;
            if (intendedClaim) value = std::max(value, 0);
            pos.undo_move(move);
            if (aborted) return 0;
            if (value > best) {
                best = value; frame.bestMove = move; frame.claimChoice = useClaim;
                setPV(ply, move, useClaim);
            }
            alpha = std::max(alpha, value);
            if (alpha >= beta) {
                ++stats.betaCutoffs;
                updateCutoff(pos, depth, ply, index);
                break;
            }
        }
        Bound bound = best >= beta ? Lower : best <= originalAlpha ? Upper : Exact;
        saveHint(pos, frame.bestMove);
        if (allowTable) save(key, depth, ply, best, bound, frame.bestMove, frame.claimChoice);
        return best;
    }

    SearchResult search(Board& board, const Limits& requested, std::atomic_bool& stopping,
                        const std::function<void(const SearchInfo&)>& report) {
        limits = requested; stop = &stopping; nodes = 0; aborted = false; stats = {};
        ++generation;
        start = std::chrono::steady_clock::now();
        Position& pos = board.pos;
        SearchResult result;
        auto legal = board.legalMoves();
        if (legal.empty() || automaticDraw(pos)) {
            result.terminal = true;
            result.score = legal.empty() && pos.checkers() ? Mate : 0;
            if (report) report(result);
            return result;
        }
        if (limits.restrictMoves) {
            legal.erase(std::remove_if(legal.begin(), legal.end(), [&](Move move) {
                return std::find(limits.searchMoves.begin(), limits.searchMoves.end(), move) == limits.searchMoves.end();
            }), legal.end());
        }
        if (legal.empty()) return result;
        result.bestMove = legal.front();
        result.score = evaluateLeaf(pos, legal);
        if (drawClaimAvailable(pos) && result.score == 0) result.claimDraw = true;
        std::uint64_t context = historyContext(pos);
        frames[0].previousPiece = NO_PIECE; frames[0].previousMove = Move::none();
        int maxDepth = std::clamp(limits.depth, 1, MaxDepth);
        for (int depth = 1; depth <= maxDepth && !shouldStop(true); ++depth) {
            int delta = 64;
            int alpha = -Infinity, beta = Infinity;
            if (options.useAspiration && depth >= 3 && std::abs(result.score) < MateThreshold) {
                alpha = std::max(-Infinity, result.score - delta);
                beta = std::min(Infinity, result.score + delta);
            }
            int value;
            while (true) {
                value = negamax(pos, depth, 0, alpha, beta, context);
                if (aborted) break;
                if (value <= alpha && alpha > -Infinity) {
                    ++stats.aspirationResearches;
                    alpha = std::max(-Infinity, value - delta);
                } else if (value >= beta && beta < Infinity) {
                    ++stats.aspirationResearches;
                    beta = std::min(Infinity, value + delta);
                } else break;
                delta = std::min(Infinity, delta * 2);
            }
            if (aborted) break;
            Frame& root = frames[0];
            result.score = value; result.depth = depth; result.claimDraw = root.claimChoice;
            if (root.bestMove.is_ok()) result.bestMove = root.bestMove;
            result.pv.assign(root.pv.begin(), root.pv.begin() + root.pvLength);
            result.nodes = nodes; result.timeMs = elapsed();
            if (report) report(result);
            if (std::abs(value) >= MateThreshold) break;
        }
        result.nodes = nodes; result.timeMs = elapsed();
        return result;
    }
};

Searcher::Searcher(int hashMb) : data_(std::make_unique<Data>()) { data_->setHash(hashMb); }
Searcher::~Searcher() = default;
void Searcher::setHash(int megabytes) { data_->setHash(megabytes); }
void Searcher::clear() { data_->clear(); }
void Searcher::setOptions(const SearchOptions& options) { data_->options = options; }
const SearchOptions& Searcher::options() const { return data_->options; }
const SearchStats& Searcher::stats() const { return data_->stats; }
SearchResult Searcher::search(Board& board, const Limits& limits, std::atomic_bool& stop,
                              const std::function<void(const SearchInfo&)>& report) {
    return data_->search(board, limits, stop, report);
}
} // namespace Selfmate
