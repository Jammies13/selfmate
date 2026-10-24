// Selfmate UCI and local console frontend. GPL-3.0-or-later.
#include "engine.h"
#include "movegen.h"

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
using namespace Selfmate;
using namespace Stockfish;
std::mutex outputMutex;

void output(const std::string& line) {
    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << line << std::endl;
}

std::string infoLine(const SearchInfo& info) {
    std::ostringstream out;
    out << "info depth " << info.depth << " score ";
    if (info.score >= MateThreshold) out << "mate " << (Mate - info.score + 1) / 2;
    else if (info.score <= -MateThreshold) out << "mate " << -(Mate + info.score + 1) / 2;
    else out << "cp " << info.score;
    out << " nodes " << info.nodes << " time " << info.timeMs
        << " nps " << (info.nodes * 1000 / std::max<std::int64_t>(1, info.timeMs));
    if (!info.pv.empty()) {
        out << " pv";
        for (Move move : info.pv) out << ' ' << moveUci(move);
    }
    return out.str();
}

bool number(const std::string& text, std::int64_t& value) {
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size()
        && value >= 0 && value <= 1000000000000LL;
}

class Uci {
 public:
    Uci() : board_(std::make_unique<Board>()) {}
    ~Uci() { finish(); }
    void run() {
        std::string line;
        while (std::getline(std::cin, line)) {
            try {
                std::istringstream input(line);
                std::string command;
                input >> command;
                if (command == "uci") {
                    output("id name Selfmate 0.2");
                    output("id author Selfmate contributors; Stockfish developers (rules core)");
                    output("option name Hash type spin default 16 min 1 max 256");
                    output("option name Move Overhead type spin default 10 min 0 max 5000");
                    output("option name Ponder type check default false");
                    output("option name MultiPV type spin default 1 min 1 max 1");
                    output("option name Clear Hash type button");
                    output("option name Use TT type check default true");
                    output("option name Use PVS type check default true");
                    output("option name Use Aspiration type check default true");
                    output("info string Objective: force your own checkmate against an opponent with the same objective");
                    output("info string Positive mate scores mean forced SELF-checkmate; orthodox GUI result labels are reversed");
                    output("uciok");
                } else if (command == "isready") output("readyok");
                else if (command == "quit") { finish(); return; }
                else if (command == "stop" || command == "ponderhit") finish();
                else if (command == "ucinewgame") {
                    finish(); searcher_.clear(); board_ = std::make_unique<Board>();
                } else if (command == "setoption") setOption(input);
                else if (command == "position") position(input);
                else if (command == "go") go(input);
                else if (command == "d") {
                    finish(); output(board_->ascii()); output("info string FEN " + board_->pos.fen());
                } else if (command == "legal") {
                    finish(); std::string text = "info string legal";
                    for (Move move : board_->legalMoves()) text += " " + moveUci(move);
                    output(text);
                } else if (command == "eval") {
                    finish(); output("info string selfmate evaluation " + std::to_string(evaluate(board_->pos)));
                } // Unknown UCI commands are ignored as required by the protocol.
            } catch (const std::exception& error) { output(std::string("info string error: ") + error.what()); }
        }
        finish();
    }
 private:
    std::unique_ptr<Board> board_;
    Searcher searcher_;
    std::thread worker_;
    std::atomic_bool stop_{false};
    std::mutex waitMutex_;
    std::condition_variable wake_;
    bool holding_ = false;
    int overhead_ = 10;

    void finish() {
        stop_.store(true, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lock(waitMutex_); holding_ = false; }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void setOption(std::istringstream& input) {
        std::string token, name, value;
        input >> token;
        if (token != "name") { output("info string error: expected setoption name"); return; }
        while (input >> token && token != "value") { if (!name.empty()) name += ' '; name += token; }
        std::getline(input >> std::ws, value);
        std::int64_t parsed = 0;
        if (name == "Hash") {
            if (!number(value, parsed) || parsed < 1 || parsed > 256) {
                output("info string error: Hash must be between 1 and 256"); return;
            }
            finish(); searcher_.setHash(int(parsed));
        } else if (name == "Clear Hash") { finish(); searcher_.clear(); }
        else if (name == "Move Overhead") {
            if (!number(value, parsed) || parsed > 5000) {
                output("info string error: Move Overhead must be between 0 and 5000"); return;
            }
            overhead_ = int(parsed);
        } else if (name == "Ponder") {
            if (value != "true" && value != "false") output("info string error: Ponder expects true or false");
        } else if (name == "MultiPV") {
            if (value != "1") output("info string error: this engine supports one principal variation");
        } else if (name == "Use TT" || name == "Use PVS" || name == "Use Aspiration") {
            if (value != "true" && value != "false") {
                output("info string error: " + name + " expects true or false"); return;
            }
            finish();
            SearchOptions options = searcher_.options();
            if (name == "Use TT") options.useTT = value == "true";
            if (name == "Use PVS") options.usePVS = value == "true";
            if (name == "Use Aspiration") options.useAspiration = value == "true";
            searcher_.setOptions(options);
        } else output("info string error: unsupported option " + name);
    }

    void position(std::istringstream& input) {
        finish();
        std::string token, fen, error;
        input >> token;
        if (token == "startpos") {
            fen = StartFen;
            if (!(input >> token)) token.clear();
        }
        else if (token == "fen") {
            for (int field = 0; field < 6; ++field) {
                if (!(input >> token)) { output("info string error: incomplete FEN"); return; }
                if (field) fen += ' '; fen += token;
            }
            if (!(input >> token)) token.clear();
        } else { output("info string error: position expects startpos or fen"); return; }
        auto candidate = std::make_unique<Board>();
        if (!candidate->setFen(fen, error)) { output("info string error: " + error); return; }
        if (!token.empty()) {
            if (token != "moves") { output("info string error: expected moves after position"); return; }
            while (input >> token) if (!candidate->push(token, error)) {
                output("info string error: " + error); return;
            }
        }
        board_ = std::move(candidate);
    }

    void go(std::istringstream& input) {
        finish();
        std::vector<std::string> tokens;
        std::string token;
        while (input >> token) tokens.push_back(token);
        Limits limits;
        bool explicitDepth = false, hasTime = false, hold = false;
        std::int64_t remaining[2] = {-1, -1}, increment[2] = {0, 0}, movesToGo = 30;
        auto recognized = [](const std::string& text) {
            return text == "depth" || text == "movetime" || text == "nodes" || text == "wtime"
                || text == "btime" || text == "winc" || text == "binc" || text == "movestogo"
                || text == "infinite" || text == "ponder" || text == "mate" || text == "searchmoves";
        };
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            token = tokens[i];
            if (token == "infinite" || token == "ponder") { limits.infinite = true; hold = true; continue; }
            if (token == "searchmoves") {
                limits.restrictMoves = true;
                while (i + 1 < tokens.size() && !recognized(tokens[i + 1])) {
                    Move move = board_->parseMove(tokens[++i]);
                    if (!move.is_ok()) { output("info string error: illegal searchmove " + tokens[i]); return; }
                    limits.searchMoves.push_back(move);
                }
                continue;
            }
            if (!recognized(token)) continue;
            std::int64_t value;
            if (i + 1 >= tokens.size() || !number(tokens[++i], value)) {
                output("info string error: invalid numeric go parameter " + token); return;
            }
            if (token == "depth") { limits.depth = int(std::clamp<std::int64_t>(value, 1, MaxDepth)); explicitDepth = true; }
            else if (token == "mate") { limits.depth = int(std::clamp<std::int64_t>(value * 2, 1, MaxDepth)); explicitDepth = true; }
            else if (token == "movetime") { limits.moveTimeMs = std::max<std::int64_t>(1, value); hasTime = true; }
            else if (token == "nodes") limits.nodes = std::max<std::int64_t>(1, value);
            else if (token == "wtime") remaining[WHITE] = value;
            else if (token == "btime") remaining[BLACK] = value;
            else if (token == "winc") increment[WHITE] = value;
            else if (token == "binc") increment[BLACK] = value;
            else if (token == "movestogo") movesToGo = std::max<std::int64_t>(1, value);
        }
        Color us = board_->pos.side_to_move();
        if (!hasTime && remaining[us] >= 0 && !limits.infinite) {
            std::int64_t available = std::max<std::int64_t>(1, remaining[us] - overhead_);
            limits.moveTimeMs = std::clamp<std::int64_t>(remaining[us] / movesToGo + increment[us] * 3 / 4, 1, available);
            hasTime = true;
        }
        if (!explicitDepth && (hasTime || limits.nodes || limits.infinite)) limits.depth = MaxDepth;
        if (limits.infinite) limits.moveTimeMs = 0;
        stop_.store(false, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lock(waitMutex_); holding_ = hold; }
        worker_ = std::thread([this, limits] {
            try {
                SearchResult result = searcher_.search(*board_, limits, stop_, [](const SearchInfo& info) { output(infoLine(info)); });
                { std::unique_lock<std::mutex> lock(waitMutex_); wake_.wait(lock, [this] { return !holding_; }); }
                if (result.claimDraw) output("info string draw claim chosen: claim through the GUI before playing bestmove; UCI has no draw-claim command");
                if (result.terminal) output("info string terminal position under selfmate rules");
                const auto& stats = searcher_.stats();
                output("info string search ttHits " + std::to_string(stats.ttHits)
                       + " ttCutoffs " + std::to_string(stats.ttCutoffs)
                       + " betaCutoffs " + std::to_string(stats.betaCutoffs)
                       + " pvsResearches " + std::to_string(stats.pvsResearches)
                       + " aspirationResearches " + std::to_string(stats.aspirationResearches));
                output("bestmove " + moveUci(result.bestMove));
            } catch (const std::exception& error) {
                output(std::string("info string search error: ") + error.what());
                output("bestmove 0000");
            }
        });
    }
};

int play() {
    Board board;
    Searcher searcher;
    std::atomic_bool stop{false};
    std::cout << "Selfmate: both sides try to get themselves checkmated.\n"
              << "You play White; engine plays Black. Enter moves such as e2e4.\n"
              << "Commands: board, legal, hint, fen <FEN>, claim [move], quit.\n";
    while (true) {
        std::cout << '\n' << board.ascii();
        auto moves = board.legalMoves();
        if (moves.empty()) {
            if (board.pos.checkers()) std::cout << "Checkmate: the checkmated side WINS the selfmate objective.\n";
            else std::cout << "Draw by stalemate.\n";
            return 0;
        }
        if (automaticDraw(board.pos)) { std::cout << "Automatic draw.\n"; return 0; }
        Limits limits; limits.depth = 6; limits.moveTimeMs = 1000;
        if (board.pos.side_to_move() == BLACK) {
            auto result = searcher.search(board, limits, stop);
            if (result.claimDraw) { std::cout << "Engine claims a draw.\n"; return 0; }
            std::string error;
            std::cout << "Engine: " << moveUci(result.bestMove) << '\n' << infoLine(result) << '\n';
            if (!board.push(moveUci(result.bestMove), error)) { std::cerr << error << '\n'; return 1; }
            continue;
        }
        if (drawClaimAvailable(board.pos)) std::cout << "You may claim a draw.\n";
        std::cout << "> " << std::flush;
        std::string line, error;
        if (!std::getline(std::cin, line) || line == "quit") return 0;
        if (line == "board" || line.empty()) continue;
        if (line == "legal") {
            for (Move move : moves) std::cout << moveUci(move) << ' ';
            std::cout << '\n'; continue;
        }
        if (line == "hint") { auto result = searcher.search(board, limits, stop); std::cout << infoLine(result) << '\n'; continue; }
        if (line.rfind("fen ", 0) == 0) {
            if (!board.setFen(line.substr(4), error)) std::cout << error << '\n';
            else searcher.clear();
            continue;
        }
        if (line == "claim" || line.rfind("claim ", 0) == 0) {
            bool claim = drawClaimAvailable(board.pos);
            if (!claim && line.size() > 6) {
                Move move = board.parseMove(line.substr(6));
                if (move.is_ok()) {
                    StateInfo state; board.pos.do_move(move, state, nullptr);
                    claim = drawClaimAvailable(board.pos); board.pos.undo_move(move);
                }
            }
            if (claim) { std::cout << "Draw claimed.\n"; return 0; }
            std::cout << "No valid draw claim.\n"; continue;
        }
        if (!board.push(line, error)) std::cout << error << '\n';
    }
}
} // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    if (argc > 1 && std::string(argv[1]) == "--play") return play();
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::cout << "Selfmate 0.2 (GPL-3.0-or-later), using the Stockfish 17.1 rules core.\n"
                  << "No arguments: UCI protocol on stdin/stdout.\n"
                  << "--play: local interactive console.\n"
                  << "Goal: force your opponent to checkmate you while it pursues its own selfmate.\n";
        return 0;
    }
    Uci engine; engine.run(); return 0;
}
