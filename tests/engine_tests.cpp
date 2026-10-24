// Regression coverage for standard chess legality and adversarial selfmate.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace Selfmate;
int checks = 0;
int failures = 0;

void expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void load(Board& board, const std::string& fen) {
    std::string error;
    if (!board.setFen(fen, error))
        throw std::runtime_error("Test FEN rejected: " + fen + " (" + error + ")");
}

void play(Board& board, const std::string& move) {
    std::string error;
    if (!board.push(move, error))
        throw std::runtime_error("Test move rejected: " + move + " (" + error + ")");
}

SearchResult search(Board& board, int depth = 2) {
    Searcher searcher;
    Limits limits;
    limits.depth = depth;
    std::atomic_bool stop{false};
    return searcher.search(board, limits, stop);
}

bool legal(const Board& board, Move move) {
    const auto moves = board.legalMoves();
    return std::find(moves.begin(), moves.end(), move) != moves.end();
}

void moveGeneration() {
    Board board;
    load(board, StartFen);
    const auto original = board.pos.fen();
    const std::uint64_t counts[] = {1, 20, 400, 8902, 197281};
    for (int depth = 0; depth <= 4; ++depth)
        expect(perft(board.pos, depth) == counts[depth], "start-position perft " + std::to_string(depth));
    expect(board.pos.fen() == original, "perft restores the board");

    load(board, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    expect(perft(board.pos, 1) == 48, "Kiwipete perft 1");
    expect(perft(board.pos, 2) == 2039, "Kiwipete perft 2");
    expect(perft(board.pos, 3) == 97862, "Kiwipete perft 3");

    load(board, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    expect(board.parseMove("e1g1") != Move::none(), "kingside castling uses UCI king destination");
    expect(board.parseMove("e1c1") != Move::none(), "queenside castling uses UCI king destination");
    expect(moveUci(board.parseMove("e1g1")) == "e1g1", "castling formats as standard UCI");
    play(board, "e1g1");
    expect(board.pos.piece_on(Stockfish::SQ_G1) == Stockfish::W_KING, "castle places king on g1");
    expect(board.pos.piece_on(Stockfish::SQ_F1) == Stockfish::W_ROOK, "castle places rook on f1");
    load(board, "4kr2/8/8/8/8/8/8/R3K2R w KQ - 0 1");
    expect(board.parseMove("e1g1") == Move::none(), "castling through check is illegal");

    load(board, "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2");
    expect(board.parseMove("e5d6") != Move::none(), "legal en passant is generated");
    play(board, "e5d6");
    expect(board.pos.piece_on(Stockfish::SQ_D6) == Stockfish::W_PAWN, "en passant pawn arrives on d6");
    expect(board.pos.empty(Stockfish::SQ_D5), "en passant removes the captured pawn");
    load(board, "k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 2");
    expect(board.parseMove("e5d6") == Move::none(), "en passant exposing own king is illegal");

    load(board, "7k/P7/8/8/8/8/8/7K w - - 0 1");
    for (const auto* suffix : {"q", "r", "b", "n"})
        expect(board.parseMove(std::string("a7a8") + suffix) != Move::none(), "all four promotions are legal");
    expect(board.parseMove("a7a8") == Move::none(), "promotion piece is required");

    load(board, StartFen);
    const auto beforeIllegal = board.pos.fen();
    std::string error;
    expect(!board.push("e2e5", error), "illegal move is rejected");
    expect(!error.empty(), "illegal move has a diagnostic");
    expect(board.pos.fen() == beforeIllegal, "illegal move does not mutate position");
    load(board, "\trnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR\tw\tKQkq\t-\t0\t1  \r\n");
    expect(board.pos.fen() == StartFen, "FEN whitespace is normalized before Stockfish parsing");
    expect(!board.setFen("not a FEN", error), "malformed FEN is rejected");
    expect(!board.setFen("8/8/8/8/8/8/8/8 w - - 0 1", error), "FEN without kings is rejected");
    expect(!board.setFen("8/8/8/8/8/8/4k3/4K3 w - - 0 1", error), "adjacent kings are rejected");
}

void terminalScores() {
    Board board;
    load(board, "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1");
    auto result = search(board);
    expect(result.terminal, "own checkmate is terminal");
    expect(result.bestMove == Move::none(), "checkmated side has no best move");
    expect(result.score == Mate, "being checkmated has the positive terminal score");

    load(board, "7k/6Q1/5K2/8/8/8/8/8 b - - 150 1");
    result = search(board);
    expect(result.score == Mate, "checkmate takes priority over the 75-move draw");

    load(board, "7k/5Q2/5K2/8/8/8/8/8 b - - 0 1");
    result = search(board);
    expect(result.terminal && result.score == 0, "stalemate is a draw");

    load(board, "7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    Searcher searcher;
    Limits forced;
    forced.depth = 1;
    forced.restrictMoves = true;
    forced.searchMoves = {board.parseMove("g6g7")};
    std::atomic_bool stop{false};
    result = searcher.search(board, forced, stop);
    expect(result.score == -Mate + 1, "checkmating the opponent is a loss");
    result = search(board, 2);
    expect(result.bestMove != board.parseMove("g6g7"), "engine avoids an available opponent checkmate");
    expect(result.score > -MateThreshold, "engine prefers continued play or draw to giving mate");
}

constexpr const char* SelfmateFen = "8/8/1Q6/2n5/k1N5/2b5/NR6/KB6 w - - 0 1";

void adversarialSelfmate() {
    Board board;
    load(board, SelfmateFen);
    const auto original = board.pos.fen();
    const auto result = search(board, 2);
    expect(moveUci(result.bestMove) == "b6b3", "finds Qb3+ forcing own mate");
    expect(result.score == Mate - 2, "forced own mate receives a positive distance score");
    expect(board.pos.fen() == original, "search restores the board");
    expect(repetitionCount(board.pos) == 1, "search restores repetition history");

    play(board, "b6b3");
    auto replies = board.legalMoves();
    expect(replies.size() == 1, "all opponent choices are covered in forced selfmate");
    expect(replies.size() == 1 && moveUci(replies.front()) == "c5b3", "only reply is Nxb3#");
    play(board, "c5b3");
    expect(board.legalMoves().empty() && board.pos.checkers(), "forced reply actually checkmates our king");

    // Qh6 lets Black deliver Nxb3#, but also lets Black decline it. A search
    // which treats the opponent as helpful would incorrectly call this a win.
    load(board, SelfmateFen);
    const auto quietMove = board.parseMove("b6h6");
    expect(quietMove != Move::none(), "nonforcing comparison move is legal");
    play(board, "b6h6");
    bool canMateUs = false;
    bool canAvoidMatingUs = false;
    for (const auto reply : board.legalMoves()) {
        Stockfish::StateInfo state;
        board.pos.do_move(reply, state, nullptr);
        const bool mate = board.pos.checkers() && board.legalMoves().empty();
        canMateUs |= mate;
        canAvoidMatingUs |= !mate;
        board.pos.undo_move(reply);
    }
    expect(canMateUs && canAvoidMatingUs, "comparison line offers both mating and nonmating opponent replies");
    load(board, SelfmateFen);
    Searcher searcher;
    Limits limits;
    limits.depth = 2;
    limits.restrictMoves = true;
    limits.searchMoves = {board.parseMove("b6h6")};
    std::atomic_bool stop{false};
    const auto adversarial = searcher.search(board, limits, stop);
    expect(adversarial.score < MateThreshold, "opponent refuses a voluntary mate of our king");
}

void draws() {
    Board board;
    const std::vector<std::pair<std::string, bool>> material = {
      {"7k/8/8/8/8/8/8/K7 w - - 0 1", true},
      {"7k/8/8/8/8/8/8/KB6 w - - 0 1", true},
      {"7k/8/8/8/8/8/8/KN6 w - - 0 1", true},
      {"5b1k/8/8/8/8/8/8/K1B5 w - - 0 1", true},
      {"6bk/8/8/8/8/8/8/K1B5 w - - 0 1", false},
      {"7k/8/8/8/8/8/6NN/K7 w - - 0 1", false},
      {"7k/8/8/8/8/8/P7/K7 w - - 0 1", false}};
    for (const auto& entry : material) {
        load(board, entry.first);
        expect(deadPosition(board.pos) == entry.second, "dead-material classification: " + entry.first);
    }
    load(board, "7k/8/8/8/8/8/8/K7 w - - 0 1");
    auto result = search(board);
    expect(result.terminal && result.score == 0, "dead position terminates as draw");

    load(board, StartFen);
    expect(repetitionCount(board.pos) == 1, "initial occurrence count is one");
    const std::vector<std::string> cycle = {"g1f3", "g8f6", "f3g1", "f6g8"};
    for (int occurrence = 2; occurrence <= 5; ++occurrence) {
        for (const auto& move : cycle)
            play(board, move);
        expect(repetitionCount(board.pos) == occurrence, "exact repetition count " + std::to_string(occurrence));
        expect(drawClaimAvailable(board.pos) == (occurrence >= 3), "threefold claim starts on third occurrence");
        expect(automaticDraw(board.pos) == (occurrence >= 5), "fivefold automatic draw starts on fifth occurrence");
    }
    result = search(board);
    expect(result.terminal && result.score == 0, "fivefold repetition terminates search");

    // An en-passant marker distinguishes repetitions only if the capture is
    // legal. Here e5xd6 would expose Ke1 to Re8, so the initial marker must not
    // prevent counting the same position after a king shuffle.
    load(board, "k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 2");
    for (const auto* move : {"e1f1", "a8b8", "f1e1", "b8a8"})
        play(board, move);
    expect(repetitionCount(board.pos) == 2, "illegal pinned en passant does not distinguish repetition");
    load(board, "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2");
    for (const auto* move : {"e1f1", "e8f8", "f1e1", "f8e8"})
        play(board, move);
    expect(repetitionCount(board.pos) == 1, "legal en passant availability distinguishes repetition");

    load(board, "7k/8/5KQ1/8/8/8/8/8 w - - 99 1");
    expect(!drawClaimAvailable(board.pos), "99 halfmoves do not allow a current-position claim");
    Searcher searcher;
    Limits limits;
    limits.depth = 1;
    limits.restrictMoves = true;
    limits.searchMoves = {board.parseMove("g6g7")};
    std::atomic_bool stop{false};
    result = searcher.search(board, limits, stop);
    expect(result.score == 0 && result.claimDraw, "may claim fifty moves by announcing a quiet intended move");

    // Constrain the root to Ka3-b4, which discovers Ra8-a1#. With 99 halfmoves
    // Black can instead announce that quiet king move and claim a draw.
    load(board, "r7/8/8/8/8/kPb5/1BP5/KR6 b - - 0 1");
    limits.searchMoves = {board.parseMove("a3b4")};
    expect(limits.searchMoves.front() != Move::none(), "restricted discovered-mate king move is legal");
    result = searcher.search(board, limits, stop);
    expect(moveUci(result.bestMove) == "a3b4" && result.score == -Mate + 1,
           "restricted discovered-mate move loses under selfmate objective");
    load(board, "r7/8/8/8/8/kPb5/1BP5/KR6 b - - 99 1");
    expect(!drawClaimAvailable(board.pos), "restricted-move position has no current fifty-move claim");
    limits.searchMoves = {board.parseMove("a3b4")};
    result = searcher.search(board, limits, stop);
    expect(result.claimDraw && result.score == 0,
           "announcing restricted quiet mate permits an intended fifty-move claim");

    load(board, "7k/8/5KQ1/8/8/8/8/8 w - - 100 1");
    expect(drawClaimAvailable(board.pos), "100 halfmoves allow a draw claim");
    expect(!automaticDraw(board.pos), "50 moves is a claim, not automatic");
    load(board, "7k/8/5KQ1/8/8/8/8/8 w - - 150 1");
    expect(automaticDraw(board.pos), "150 halfmoves is an automatic draw");
    result = search(board);
    expect(result.terminal && result.score == 0, "75-move automatic draw terminates search");

    // Capturing b3 resets the clock, so White can decline its own optional
    // claim and prevent Black claiming before the compulsory Nxb3#.
    load(board, "8/8/1Q6/2n5/k1N5/1pb5/NR6/KB6 w - - 100 1");
    result = search(board, 2);
    expect(!result.claimDraw && result.score == Mate - 2, "declines optional draw when forced own mate is available");
    expect(moveUci(result.bestMove) == "b6b3", "capture resets fifty-move clock on winning selfmate line");
}

void limitsAndRestoration() {
    Board board;
    load(board, StartFen);
    const auto original = board.pos.fen();
    Searcher searcher;
    Limits limits;
    limits.depth = 20;
    limits.nodes = 256;
    std::atomic_bool stop{false};
    auto result = searcher.search(board, limits, stop);
    expect(legal(board, result.bestMove), "node-limited search returns a legal move");
    expect(result.nodes <= 1024, "node limit stops search promptly");
    expect(board.pos.fen() == original, "node abort restores board");

    limits.nodes = 0;
    limits.moveTimeMs = 20;
    const auto start = std::chrono::steady_clock::now();
    result = searcher.search(board, limits, stop);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
    expect(legal(board, result.bestMove), "time-limited search returns a legal move");
    expect(elapsed < 2000, "time limit stops search promptly");
    expect(board.pos.fen() == original, "time abort restores board");

    stop = true;
    limits.moveTimeMs = 0;
    result = searcher.search(board, limits, stop);
    expect(legal(board, result.bestMove), "pre-stopped search has a legal fallback move");
    expect(board.pos.fen() == original, "stop abort restores board");

    stop = false;
    limits.depth = 2;
    limits.restrictMoves = true;
    limits.searchMoves = {board.parseMove("e2e4")};
    result = searcher.search(board, limits, stop);
    expect(moveUci(result.bestMove) == "e2e4", "searchmoves restricts root moves");
}
} // namespace

int main() {
    Selfmate::initialize();
    for (const auto& test : std::vector<std::pair<const char*, void(*)()>>{
           {"move generation", moveGeneration}, {"terminal scores", terminalScores},
           {"adversarial selfmate", adversarialSelfmate}, {"draws", draws},
           {"search limits", limitsAndRestoration}}) {
        try {
            test.second();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
