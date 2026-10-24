// Independent full-width minimax oracle for the optimized search.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/engine.h"
#include "movegen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace Selfmate;
int checks = 0, failures = 0;
std::uint64_t referenceNodes = 0;
constexpr int Infinity = 31000;
constexpr const char* MateInTwo = "8/8/1Q2N3/8/k1N1n3/2b5/NR6/KB6 w - - 0 1";
constexpr const char* MateInTwoChild = "8/8/1Q6/2N5/k1N1n3/2b5/NR6/KB6 b - - 0 1";

void expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void load(Board& board, const std::string& fen) {
    std::string error;
    if (!board.setFen(fen, error)) throw std::runtime_error("FEN rejected: " + fen + ": " + error);
}

void push(Board& board, const std::string& move) {
    std::string error;
    if (!board.push(move, error)) throw std::runtime_error("move rejected: " + move + ": " + error);
}

std::vector<Move> legalMoves(const Position& pos) {
    Stockfish::MoveList<Stockfish::LEGAL> list(pos);
    return {list.begin(), list.end()};
}

// No alpha-beta windows, TT, aspiration, move ordering, scout search, selective
// pruning, or early mate exit. Only the leaf evaluator and rules primitives are
// shared with production. Every legal continuation is explored at full depth.
int reference(Position& pos, int depth, int ply = 0) {
    ++referenceNodes;
    auto moves = legalMoves(pos);
    if (moves.empty()) return pos.checkers() ? Mate - ply : 0;
    if (automaticDraw(pos)) return 0;
    if (depth == 0) return evaluateLeaf(pos, moves);
    int best = drawClaimAvailable(pos) ? 0 : -Infinity;
    for (Move move : moves) {
        Stockfish::StateInfo state;
        pos.do_move(move, state, nullptr);
        bool intendedClaim = drawClaimAvailable(pos);
        int value = -reference(pos, depth - 1, ply + 1);
        if (intendedClaim) value = std::max(0, value);
        pos.undo_move(move);
        best = std::max(best, value);
    }
    return best;
}

struct Fixture {
    std::string name;
    std::string fen;
    std::vector<std::string> moves;
};

void setup(Board& board, const Fixture& fixture) {
    load(board, fixture.fen);
    for (const auto& move : fixture.moves) push(board, move);
}

std::vector<Fixture> makeFixtures() {
    const std::vector<std::string> seeds = {
      StartFen,
      "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
      "r2q1rk1/pp1nbppp/2pbpn2/3p4/2PP4/2N1PN2/PPQ1BPPP/R1B2RK1 w - - 0 9",
      "8/5pk1/6p1/4P2p/4K2P/6P1/8/8 w - - 0 40",
      "8/5pk1/6p1/3r3p/4K2P/6P1/5P2/3R4 w - - 0 40",
      "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2",
      "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
      MateInTwo,
      "8/8/1Q6/2n5/k1N5/1pb5/NR6/KB6 w - - 100 1"};
    std::vector<Fixture> fixtures;
    std::uint32_t random = 0x8b791e5d;
    for (std::size_t seed = 0; seed < seeds.size(); ++seed) {
        Board board;
        load(board, seeds[seed]);
        std::vector<std::string> played;
        for (int ply = 0; ply < 24; ++ply) {
            auto legal = board.legalMoves();
            if (legal.empty() || automaticDraw(board.pos)) break;
            if (ply % 3 == 0)
                fixtures.push_back({"walk" + std::to_string(seed) + "/" + std::to_string(ply), seeds[seed], played});
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            std::string move = moveUci(legal[random % legal.size()]);
            played.push_back(move);
            push(board, move);
        }
    }
    // Exact draw history is retained by replaying moves, not by serializing FEN.
    Fixture repetition{"threefold", StartFen, {}};
    for (int cycle = 0; cycle < 2; ++cycle)
        for (const auto* move : {"g1f3", "g8f6", "f3g1", "f6g8"}) repetition.moves.push_back(move);
    fixtures.push_back(repetition);
    fixtures.push_back({"clock99", "7k/8/5KQ1/8/8/8/8/8 w - - 99 1", {}});
    fixtures.push_back({"clock149", "7k/8/5KQ1/8/8/8/8/8 w - - 149 1", {}});
    return fixtures;
}

void verifyPV(Board& board, const SearchResult& result, const std::string& label) {
    const auto before = board.pos.fen();
    const auto beforeKey = board.pos.state()->key;
    const auto beforeRepetitions = repetitionCount(board.pos);
    std::vector<Move> played;
    std::vector<Stockfish::StateInfo> states(result.pv.size());
    bool valid = true;
    for (std::size_t i = 0; i < result.pv.size(); ++i) {
        auto legal = board.legalMoves();
        if (std::find(legal.begin(), legal.end(), result.pv[i]) == legal.end()) {
            valid = false;
            break;
        }
        board.pos.do_move(result.pv[i], states[i], nullptr);
        played.push_back(result.pv[i]);
    }
    expect(valid, label + ": PV is legal");
    if (std::abs(result.score) >= MateThreshold && !result.terminal) {
        expect(valid && board.legalMoves().empty() && board.pos.checkers(), label + ": mate PV reaches actual checkmate");
        expect(int(result.pv.size()) == Mate - std::abs(result.score), label + ": mate distance matches full PV");
    }
    for (auto it = played.rbegin(); it != played.rend(); ++it) board.pos.undo_move(*it);
    expect(board.pos.fen() == before && board.pos.state()->key == beforeKey
           && repetitionCount(board.pos) == beforeRepetitions, label + ": PV replay restores history");
}

void verifySearch(Board& board, Searcher& searcher, int depth, int expected, const std::string& label) {
    const auto before = board.pos.fen();
    const auto beforeKey = board.pos.state()->key;
    const auto repetitions = repetitionCount(board.pos);
    Limits limits; limits.depth = depth;
    std::atomic_bool stop{false};
    auto result = searcher.search(board, limits, stop);
    expect(result.score == expected, label + ": expected " + std::to_string(expected) + ", got " + std::to_string(result.score));
    expect(board.pos.fen() == before && board.pos.state()->key == beforeKey
           && repetitionCount(board.pos) == repetitions, label + ": search restores board/history");
    auto legal = board.legalMoves();
    expect(result.terminal || std::find(legal.begin(), legal.end(), result.bestMove) != legal.end(), label + ": best move is legal");
    if (!result.terminal && !result.claimDraw && result.bestMove.is_ok()) {
        Stockfish::StateInfo state;
        board.pos.do_move(result.bestMove, state, nullptr);
        bool claim = drawClaimAvailable(board.pos);
        int moveValue = -reference(board.pos, depth - 1, 1);
        if (claim) moveValue = std::max(0, moveValue);
        board.pos.undo_move(result.bestMove);
        expect(moveValue == expected, label + ": chosen move attains the minimax value");
    }
    verifyPV(board, result, label);
}

void differential() {
    const auto fixtures = makeFixtures();
    expect(fixtures.size() >= 64, "at least 64 deterministic varied positions");
    struct Case { Fixture fixture; int depth; int score; };
    std::vector<Case> cases;
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
        Board board;
        setup(board, fixtures[index]);
        cases.push_back({fixtures[index], 2, reference(board.pos, 2)});
        if (index % 10 == 0) cases.push_back({fixtures[index], 3, reference(board.pos, 3)});
    }
    struct Variant { const char* name; SearchOptions options; };
    const std::vector<Variant> variants = {
      {"all", {true, true, true}}, {"noTT", {false, true, true}},
      {"noPVS", {true, false, true}}, {"noAspiration", {true, true, false}},
      {"plainAlphaBeta", {false, false, false}}};
    for (const auto& variant : variants) {
        // Reuse a deliberately small table across different positions and
        // histories. Neither stale bounds nor index collisions may alter a score.
        Searcher searcher(1);
        searcher.setOptions(variant.options);
        for (const auto& item : cases) {
            Board board;
            setup(board, item.fixture);
            verifySearch(board, searcher, item.depth, item.score,
                         std::string(variant.name) + "/" + item.fixture.name + "/d" + std::to_string(item.depth));
        }
    }
    std::cout << fixtures.size() << " varied positions, " << cases.size() << " fixed-depth cases, 5 search configurations\n";
}

void warmTablesAndMateDistance() {
    Searcher searcher(1);
    Board board;
    load(board, MateInTwo);
    const int expected = reference(board.pos, 4);
    expect(expected == Mate - 4, "independent oracle confirms selfmate in four plies");
    verifySearch(board, searcher, 4, expected, "mate4 cold");
    expect(searcher.stats().aspirationResearches > 0, "mate swing exercises aspiration widening");
    verifySearch(board, searcher, 4, expected, "mate4 warm");
    expect(searcher.stats().ttHits > 0 && searcher.stats().ttCutoffs > 0, "warm test exercises actual cached score cutoffs");
    push(board, "e6c5");
    expect(board.legalMoves().size() == 1, "mate4 child has one legal resisting reply");
    verifySearch(board, searcher, 3, -Mate + 3, "mate3 child at new root");
    push(board, "e4c5");
    verifySearch(board, searcher, 2, Mate - 2, "mate2 grandchild at new root");

    // Same piece layout, different fifty-move and repetition contexts.
    const std::vector<Fixture> histories = {
      {"no clock claim", "8/8/1Q6/2n5/k1N5/2b5/NR6/KB6 w - - 0 1", {}},
      {"clock claim", "8/8/1Q6/2n5/k1N5/2b5/NR6/KB6 w - - 100 1", {}},
      {"clock reset capture", "8/8/1Q6/2n5/k1N5/1pb5/NR6/KB6 w - - 100 1", {}},
      {"fresh clock8", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 8 5", {}},
      {"threefold clock8", StartFen, {"g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1", "f6g8"}}};
    for (int pass = 0; pass < 3; ++pass) {
        for (const auto& history : histories) {
            setup(board, history);
            const int value = reference(board.pos, 3);
            verifySearch(board, searcher, 3, value, "history reuse/" + history.name);
        }
    }
    load(board, MateInTwoChild);
    verifySearch(board, searcher, 3, -Mate + 3, "mate3 fresh history");

    load(board, "8/8/1Q6/2n5/k1N5/2b5/NR6/KB6 w - - 0 1");
    verifySearch(board, searcher, 2, Mate - 2, "full root before restricted search");
    Limits restricted; restricted.depth = 2; restricted.restrictMoves = true;
    restricted.searchMoves = {board.parseMove("b6h6")};
    Stockfish::StateInfo state;
    board.pos.do_move(restricted.searchMoves.front(), state, nullptr);
    int restrictedValue = -reference(board.pos, 1, 1);
    board.pos.undo_move(restricted.searchMoves.front());
    std::atomic_bool stop{false};
    auto restrictedResult = searcher.search(board, restricted, stop);
    expect(restrictedResult.score == restrictedValue && restrictedResult.score < MateThreshold,
           "cached unrestricted mate does not contaminate a restricted root");
    expect(moveUci(restrictedResult.bestMove) == "b6h6", "warm restricted root honors its only permitted move");
    verifyPV(board, restrictedResult, "warm restricted root");

    load(board, "7k/8/8/8/8/1q6/N6r/KB6 w - - 98 1");
    restricted.depth = 4;
    restricted.searchMoves = {board.parseMove("a2c3")};
    for (int warm = 0; warm < 2; ++warm) {
        auto claimLine = searcher.search(board, restricted, stop);
        expect(claimLine.score == 0, "deep intended-claim fixture draws");
        expect(claimLine.pv.size() <= 2, "PV stops at the opponent's intended draw claim");
        verifyPV(board, claimLine, "deep intended-claim PV");
    }

    searcher.clear();
    load(board, MateInTwo);
    verifySearch(board, searcher, 4, expected, "mate4 after clear");
}

void interruptedCache() {
    Board board;
    load(board, StartFen);
    Searcher searcher(1);
    Limits limits;
    limits.depth = 20; limits.nodes = 1000;
    std::atomic_bool stop{false};
    const auto before = board.pos.fen();
    const auto result = searcher.search(board, limits, stop);
    expect(board.pos.fen() == before && repetitionCount(board.pos) == 1, "interrupted search restores history");
    auto legal = board.legalMoves();
    expect(std::find(legal.begin(), legal.end(), result.bestMove) != legal.end(), "interrupted search returns legal completed result");
    verifySearch(board, searcher, 3, reference(board.pos, 3), "after node interruption");
    limits.nodes = 0;
    stop = true;
    searcher.search(board, limits, stop);
    expect(board.pos.fen() == before && repetitionCount(board.pos) == 1, "pre-stopped search restores history");
    stop = false;
    verifySearch(board, searcher, 3, reference(board.pos, 3), "after immediate interruption");
}
} // namespace

int main() {
    initialize();
    for (const auto& test : std::vector<std::pair<const char*, void(*)()>>{
           {"differential", differential}, {"warm tables and mates", warmTablesAndMateDistance},
           {"interrupted cache", interruptedCache}}) {
        try { test.second(); }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << checks << " search checks, " << referenceNodes << " exhaustive reference nodes, "
              << failures << " failures\n";
    return failures ? 1 : 0;
}
