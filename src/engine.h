// Selfmate: adversarial self-checkmate chess. GPL-3.0-or-later.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "position.h"

namespace Selfmate {
using Stockfish::Move;
using Stockfish::Position;
constexpr int Mate = 30000;
constexpr int MateThreshold = 29000;
constexpr int MaxDepth = 96;
inline constexpr const char* StartFen =
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

void initialize();
std::string moveUci(Move move);

class Board {
 public:
    Board();
    Position pos;
    bool setFen(const std::string& fen, std::string& error);
    bool push(const std::string& text, std::string& error);
    Move parseMove(const std::string& text) const;
    std::vector<Move> legalMoves() const;
    std::string ascii() const;
 private:
    std::deque<Stockfish::StateInfo> states_;
};

int repetitionCount(const Position& pos);
bool deadPosition(const Position& pos);
bool drawClaimAvailable(const Position& pos);
bool automaticDraw(const Position& pos);
int evaluate(const Position& pos);
int evaluateLeaf(Position& pos, const std::vector<Move>& legalMoves);
std::uint64_t perft(Position& pos, int depth);

struct Limits {
    int depth = 8;
    std::uint64_t nodes = 0;
    std::int64_t moveTimeMs = 0;
    bool infinite = false;
    bool restrictMoves = false;
    std::vector<Move> searchMoves;
};
struct SearchInfo {
    int depth = 0;
    int score = 0;
    std::uint64_t nodes = 0;
    std::int64_t timeMs = 0;
    std::vector<Move> pv;
};
struct SearchResult : SearchInfo {
    Move bestMove = Move::none();
    bool claimDraw = false;
    bool terminal = false;
};

struct SearchOptions {
    bool useTT = true;
    bool usePVS = true;
    bool useAspiration = true;
};

struct SearchStats {
    std::uint64_t ttHits = 0;
    std::uint64_t ttCutoffs = 0;
    std::uint64_t betaCutoffs = 0;
    std::uint64_t pvsResearches = 0;
    std::uint64_t aspirationResearches = 0;
};

// Score entries include the complete available reversible-history context.
// Their depth must match the requested horizon, including for exact bounds.
class Searcher {
 public:
    explicit Searcher(int hashMb = 16);
    ~Searcher();
    Searcher(const Searcher&) = delete;
    Searcher& operator=(const Searcher&) = delete;
    void setHash(int megabytes);
    void clear();
    void setOptions(const SearchOptions& options);
    const SearchOptions& options() const;
    const SearchStats& stats() const;
    SearchResult search(Board& board, const Limits& limits,
                        std::atomic_bool& stop,
                        const std::function<void(const SearchInfo&)>& report = {});
 private:
    struct Data;
    std::unique_ptr<Data> data_;
};
} // namespace Selfmate
