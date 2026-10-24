// Selfmate: adversarial self-checkmate chess. GPL-3.0-or-later.
#include "engine.h"
#include "movegen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <mutex>
#include <sstream>

namespace Stockfish::Zobrist { extern Key enpassant[FILE_NB]; }

namespace Selfmate {
using namespace Stockfish;
namespace {
const std::string PieceChars = " PNBRQK  pnbrqk";

bool legalEp(const Position& pos, Square target) {
    if (target == SQ_NONE || !pos.empty(target)) return false;
    Color us = pos.side_to_move();
    Square captured = target + pawn_push(~us);
    if (!is_ok(captured) || pos.piece_on(captured) != make_piece(~us, PAWN)) return false;
    Bitboard froms = pawn_attacks_bb(~us, target) & pos.pieces(us, PAWN);
    while (froms) {
        Square from = pop_lsb(froms);
        Bitboard occupancy = (pos.pieces() & ~square_bb(from) & ~square_bb(captured))
                           | square_bb(target);
        if (!(pos.attackers_to(pos.square<KING>(us), occupancy)
              & pos.pieces(~us) & ~square_bb(captured))) return true;
    }
    return false;
}

bool mayClaimByMove(const Position& pos) {
    if (pos.rule50_count() >= 99) return true;
    const StateInfo* state = pos.state();
    int remaining = std::min(state->rule50, state->pliesFromNull);
    if (remaining < 7) return false;
    // A third occurrence needs an earlier repeat. An EP marker can hide a
    // canonical repeat from upstream's raw-key bookkeeping, so include it as
    // a conservative trigger for the exact move-by-move check below.
    for (; state && remaining >= 0; state = state->previous, --remaining)
        if (state->repetition || state->epSquare != SQ_NONE) return true;
    return false;
}

bool validFenSyntax(const std::string& fen, std::string& error) {
    std::istringstream input(fen);
    std::string layout, side, castle, ep, half, full, extra;
    if (!(input >> layout >> side >> castle >> ep >> half >> full) || (input >> extra)) {
        error = "FEN must have exactly six fields"; return false;
    }
    std::array<char, 64> board{};
    int rank = 7, file = 0, kings[2]{}, pieces[2]{}, pawns[2]{};
    for (char ch : layout) {
        if (ch == '/') {
            if (file != 8 || rank == 0) { error = "invalid FEN ranks"; return false; }
            --rank; file = 0;
        } else if (ch >= '1' && ch <= '8') file += ch - '0';
        else {
            if (PieceChars.find(ch) == std::string::npos || ch == ' ' || file >= 8) {
                error = "invalid FEN piece placement"; return false;
            }
            int c = std::islower(static_cast<unsigned char>(ch)) ? 1 : 0;
            board[rank * 8 + file++] = ch;
            ++pieces[c];
            if (ch == 'K' || ch == 'k') ++kings[c];
            if (ch == 'P' || ch == 'p') {
                ++pawns[c];
                if (rank == 0 || rank == 7) { error = "pawn on promotion rank"; return false; }
            }
        }
        if (file > 8) { error = "FEN rank exceeds eight squares"; return false; }
    }
    if (rank != 0 || file != 8 || kings[0] != 1 || kings[1] != 1
        || pieces[0] > 16 || pieces[1] > 16 || pawns[0] > 8 || pawns[1] > 8) {
        error = "FEN requires eight ranks, one king per side, and valid piece counts"; return false;
    }
    if (side != "w" && side != "b") { error = "invalid side to move"; return false; }
    if (castle != "-") {
        std::string seen;
        for (char ch : castle) {
            if (std::string("KQkq").find(ch) == std::string::npos
                || seen.find(ch) != std::string::npos) {
                error = "invalid standard-chess castling rights"; return false;
            }
            seen += ch;
            bool white = std::isupper(static_cast<unsigned char>(ch)) != 0;
            int base = white ? 0 : 56;
            int rook = ch == 'K' || ch == 'k' ? 7 : 0;
            if (board[base + 4] != (white ? 'K' : 'k')
                || board[base + rook] != (white ? 'R' : 'r')) {
                error = "castling right requires king and rook on their starting squares"; return false;
            }
        }
    }
    if (ep != "-") {
        if (ep.size() != 2 || ep[0] < 'a' || ep[0] > 'h'
            || ep[1] != (side == "w" ? '6' : '3')) {
            error = "invalid en passant target"; return false;
        }
        int sq = (ep[1] - '1') * 8 + ep[0] - 'a';
        if (board[sq] || board[sq + (side == "w" ? -8 : 8)] != (side == "w" ? 'p' : 'P')
            || board[sq + (side == "w" ? 8 : -8)]) {
            error = "en passant target does not match a double pawn move"; return false;
        }
    }
    auto number = [](const std::string& text, int minimum) {
        if (text.empty() || text.size() > 7) return false;
        int value = 0;
        for (char ch : text) {
            if (ch < '0' || ch > '9') return false;
            value = value * 10 + ch - '0';
        }
        return value >= minimum && value <= 1000000;
    };
    if (!number(half, 0) || !number(full, 1)) { error = "invalid FEN move counters"; return false; }
    return true;
}

int vulnerability(const Position& pos, Color us) {
    Color them = ~us;
    Square king = pos.square<KING>(us);
    Bitboard enemy = pos.pieces(them);
    if (!(enemy & ~pos.pieces(KING))) return -1800; // The other king cannot mate us.
    Bitboard ring = attacks_bb<KING>(king);
    Bitboard attacks = 0;
    int proximity = 0;
    for (Bitboard remaining = enemy; remaining;) {
        Square sq = pop_lsb(remaining);
        PieceType type = type_of(pos.piece_on(sq));
        attacks |= type == PAWN ? pawn_attacks_bb(them, sq) : attacks_bb(type, sq, pos.pieces());
        if (type != KING) proximity += std::max(0, 5 - distance(sq, king)) * (type == QUEEN ? 9 : 4);
    }
    int escapes = 0;
    for (Bitboard targets = ring & ~pos.pieces(us); targets;) {
        Square to = pop_lsb(targets);
        Bitboard occupancy = (pos.pieces() & ~square_bb(king)) | square_bb(to);
        if (!(pos.attackers_to(to, occupancy) & enemy & ~square_bb(to))) ++escapes;
    }
    int resources = pos.count<QUEEN>(them) * 70 + pos.count<ROOK>(them) * 40
                  + pos.count<BISHOP>(them) * 22 + pos.count<KNIGHT>(them) * 22
                  + pos.count<PAWN>(them) * 8;
    int edge = 3 - std::min(edge_distance(file_of(king)), edge_distance(File(rank_of(king))));
    return (8 - escapes) * 22 + popcount(attacks & ring) * 24
         + popcount(ring & pos.pieces(us)) * 8 + ((attacks & king) ? 75 : 0)
         + edge * 18 + proximity + resources;
}
} // namespace

void initialize() {
    static std::once_flag once;
    std::call_once(once, [] { Bitboards::init(); Position::init(); });
}

std::string moveUci(Move move) {
    if (!move.is_ok()) return "0000";
    Square from = move.from_sq(), to = move.to_sq();
    if (move.type_of() == CASTLING) to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));
    std::string text;
    text += char('a' + file_of(from)); text += char('1' + rank_of(from));
    text += char('a' + file_of(to)); text += char('1' + rank_of(to));
    if (move.type_of() == PROMOTION) text += "  nbrq"[move.promotion_type()];
    return text;
}

Board::Board() { initialize(); std::string error; setFen(StartFen, error); }

bool Board::setFen(const std::string& fen, std::string& error) {
    // Stockfish's low-level parser expects single-space delimiters. Normalize
    // before passing validated user input to it, including in console mode.
    std::istringstream fields(fen);
    std::string canonical, field;
    while (fields >> field) { if (!canonical.empty()) canonical += ' '; canonical += field; }
    if (!validFenSyntax(canonical, error)) return false;
    StateInfo temporaryState{};
    Position candidate;
    candidate.set(canonical, false, &temporaryState);
    if (candidate.attackers_to(candidate.square<KING>(~candidate.side_to_move()))
        & candidate.pieces(candidate.side_to_move())) {
        error = "side that just moved cannot leave its king in check"; return false;
    }
    if (popcount(candidate.checkers()) > 2) { error = "impossible triple check"; return false; }
    states_.clear(); states_.emplace_back();
    pos.set(canonical, false, &states_.back());
    error.clear(); return true;
}

std::vector<Move> Board::legalMoves() const {
    MoveList<LEGAL> list(pos);
    return std::vector<Move>(list.begin(), list.end());
}

Move Board::parseMove(const std::string& text) const {
    for (Move move : legalMoves()) if (moveUci(move) == text) return move;
    return Move::none();
}

bool Board::push(const std::string& text, std::string& error) {
    Move move = parseMove(text);
    if (!move.is_ok()) { error = "illegal move: " + text; return false; }
    states_.emplace_back();
    pos.do_move(move, states_.back(), nullptr);
    error.clear(); return true;
}

std::string Board::ascii() const {
    std::ostringstream out;
    for (int rank = 7; rank >= 0; --rank) {
        out << rank + 1 << "  ";
        for (int file = 0; file < 8; ++file) {
            Piece piece = pos.piece_on(make_square(File(file), Rank(rank)));
            out << (piece == NO_PIECE ? '.' : PieceChars[piece]) << ' ';
        }
        out << '\n';
    }
    out << "   a b c d e f g h\n" << (pos.side_to_move() == WHITE ? "White" : "Black")
        << " to move" << (pos.checkers() ? " (in check)" : "") << '\n';
    return out.str();
}

int repetitionCount(const Position& pos) {
    // Match raw board keys, excluding EP first. If the boards match, the current
    // board can also establish the historical EP right's legality. A pinned EP
    // pawn must not distinguish otherwise identical positions (FIDE 9.2.3).
    const StateInfo* current = pos.state();
    Key base = current->key;
    if (current->epSquare != SQ_NONE) base ^= Zobrist::enpassant[file_of(current->epSquare)];
    Square effectiveEp = legalEp(pos, current->epSquare) ? current->epSquare : SQ_NONE;
    int count = 1;
    int remaining = std::min(current->rule50, current->pliesFromNull);
    for (const StateInfo* state = current->previous; state && remaining > 0;
         state = state->previous, --remaining) {
        Key previousBase = state->key;
        if (state->epSquare != SQ_NONE) previousBase ^= Zobrist::enpassant[file_of(state->epSquare)];
        if (base == previousBase) {
            Square previousEp = legalEp(pos, state->epSquare) ? state->epSquare : SQ_NONE;
            if (effectiveEp == previousEp) ++count;
        }
    }
    return count;
}

bool deadPosition(const Position& pos) {
    if (pos.pieces(PAWN, ROOK, QUEEN)) return false;
    int minorCount = popcount(pos.pieces(KNIGHT, BISHOP));
    if (minorCount <= 1) return true;
    if (pos.pieces(KNIGHT)) return false;
    Bitboard bishops = pos.pieces(BISHOP);
    constexpr Bitboard darkSquares = 0xAA55AA55AA55AA55ULL;
    return !(bishops & darkSquares) || !(bishops & ~darkSquares);
}

bool drawClaimAvailable(const Position& pos) {
    return pos.rule50_count() >= 100 || repetitionCount(pos) >= 3;
}

bool automaticDraw(const Position& pos) {
    return pos.rule50_count() >= 150 || deadPosition(pos) || repetitionCount(pos) >= 5;
}

int evaluate(const Position& pos) {
    if (deadPosition(pos)) return 0;
    int score = vulnerability(pos, pos.side_to_move()) - vulnerability(pos, ~pos.side_to_move());
    return std::clamp(score, -8000, 8000);
}

int evaluateLeaf(Position& pos, const std::vector<Move>& moves) {
    int score = evaluate(pos);
    if (drawClaimAvailable(pos)) return std::max(0, score);
    if (score < 0 && mayClaimByMove(pos)) {
        for (Move move : moves) {
            StateInfo state;
            pos.do_move(move, state, nullptr);
            bool intendedClaim = drawClaimAvailable(pos);
            pos.undo_move(move);
            if (intendedClaim) return 0;
        }
    }
    return score;
}

std::uint64_t perft(Position& pos, int depth) {
    if (depth <= 0) return 1;
    MoveList<LEGAL> moves(pos);
    if (depth == 1) return moves.size();
    std::uint64_t nodes = 0;
    for (Move move : moves) {
        StateInfo state;
        pos.do_move(move, state, nullptr);
        nodes += perft(pos, depth - 1);
        pos.undo_move(move);
    }
    return nodes;
}

} // namespace Selfmate
