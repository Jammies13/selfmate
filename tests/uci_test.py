#!/usr/bin/env python3
"""Exercise the real UCI process; accepts the compiled executable as argv[1]."""
# SPDX-License-Identifier: GPL-3.0-or-later
import os
import queue
import re
import subprocess
import sys
import threading
import time
from pathlib import Path


class Engine:
    def __init__(self, executable):
        self.process = subprocess.Popen(
            [str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.lines = queue.Queue()
        self.history = []
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\r\n"))
        self.lines.put(None)

    def send(self, command):
        self.history.append("> " + command)
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def until(self, predicate, timeout=8):
        deadline = time.monotonic() + timeout
        seen = []
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.001, deadline-time.monotonic()))
            except queue.Empty:
                break
            if line is None:
                raise AssertionError("engine exited before expected response")
            self.history.append("< " + line)
            seen.append(line)
            if predicate(line):
                return line, seen
        raise AssertionError("timed out waiting for engine response")

    def ready(self):
        self.send("isready")
        return self.until(lambda line: line == "readyok")[1]

    def best(self, command, timeout=8):
        self.send(command)
        line, seen = self.until(lambda line: line.startswith("bestmove "), timeout)
        fields = line.split()
        assert len(fields) >= 2, "malformed bestmove"
        return fields[1], seen

    def close(self):
        if self.process.poll() is None:
            try:
                self.send("quit")
                self.process.wait(timeout=4)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait(timeout=4)


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else
                      Path(__file__).resolve().parents[1] / "bin" /
                      ("selfmate.exe" if os.name == "nt" else "selfmate")).resolve()
    engine = Engine(executable)
    start_moves = {f"{file}2{file}{rank}" for file in "abcdefgh" for rank in "34"}
    start_moves.update({"b1a3", "b1c3", "g1f3", "g1h3"})
    checks = 0

    def check(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            raise AssertionError(message)

    try:
        engine.send("uci")
        _, lines = engine.until(lambda line: line == "uciok")
        check(any(line.startswith("id name ") for line in lines), "UCI identifies engine")
        engine.ready()
        checks += 1
        engine.send("ucinewgame")
        engine.send("position startpos")
        move, lines = engine.best("go depth 2")
        check(move in start_moves, "startpos bestmove is legal")
        check(any(line.startswith("info depth ") for line in lines), "search emits UCI progress")

        engine.send("position startpos")
        move, _ = engine.best("go depth 2 searchmoves e2e4")
        check(move == "e2e4", "UCI searchmoves is honored")
        engine.send("position startpos moves e2e4 e7e5 g1f3")
        move, _ = engine.best("go depth 1 searchmoves b8c6")
        check(move == "b8c6", "UCI position applies move history")

        engine.send("position startpos")
        move, _ = engine.best("go depth 1 searchmoves g1f3")
        check(move == "g1f3", "bare position startpos resets a changed position")

        engine.send("position fen 8/8/1Q6/2n5/k1N5/2b5/NR6/KB6 w - - 0 1")
        move, lines = engine.best("go depth 2")
        check(move == "b6b3", "UCI finds adversarial forced selfmate")
        check(any(re.search(r"score mate [1-9]", line) for line in lines),
              "forced own mate has a positive UCI mate score")

        for option in ("Use TT", "Use PVS", "Use Aspiration"):
            engine.send(f"setoption name {option} value false")
        engine.send("position fen 8/8/1Q6/2n5/k1N5/2b5/NR6/KB6 w - - 0 1")
        move, lines = engine.best("go depth 2")
        check(move == "b6b3" and any("score mate 1" in line for line in lines),
              "disabling search optimizations preserves forced selfmate")
        for option in ("Use TT", "Use PVS", "Use Aspiration"):
            engine.send(f"setoption name {option} value true")
        engine.send("position fen 8/8/1Q2N3/8/k1N1n3/2b5/NR6/KB6 w - - 0 1")
        move, lines = engine.best("go depth 4")
        check(move == "e6c5" and any("score mate 2" in line for line in lines),
              "finds a selfmate requiring four plies against resistance")
        check(any(line.startswith("info string search ttHits ") for line in lines),
              "search reports optimization counters")

        engine.send("position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1")
        move, _ = engine.best("go depth 2")
        check(move == "0000", "terminal own checkmate reports no legal move")

        engine.send("position startpos")
        engine.send("position fen this is not a FEN")
        lines = engine.ready()
        check(any(line.startswith("info string ") for line in lines), "invalid FEN produces a diagnostic")
        move, _ = engine.best("go depth 1 searchmoves e2e4")
        check(move == "e2e4", "invalid FEN preserves previous usable position")

        engine.send("position startpos moves e2e5")
        lines = engine.ready()
        check(any(line.startswith("info string ") for line in lines), "illegal move produces a diagnostic")
        move, _ = engine.best("go depth 1 searchmoves d2d4")
        check(move == "d2d4", "illegal position move preserves previous usable position")

        engine.send("position startpos")
        engine.send("go infinite")
        engine.until(lambda line: line.startswith("info depth "))
        # isready must remain responsive while a search is running.
        engine.ready()
        engine.send("stop")
        line, _ = engine.until(lambda line: line.startswith("bestmove "), timeout=4)
        check(line.split()[1] in start_moves, "stop returns a legal move from infinite search")

        engine.send("position startpos")
        move, _ = engine.best("go movetime 25", timeout=4)
        check(move in start_moves, "movetime search completes with a legal move")

        engine.send("position startpos")
        engine.send("go ponder depth 2")
        engine.until(lambda line: line.startswith("info depth 2 "))
        ponder_ready = engine.ready()
        # readyok fences earlier output: a completed ponder result must still
        # be held until ponderhit or stop, rather than emitted immediately.
        check(not any(line.startswith("bestmove ") for line in ponder_ready),
              "ponder holds a completed result")
        engine.send("ponderhit")
        line, _ = engine.until(lambda line: line.startswith("bestmove "), timeout=4)
        check(line.split()[1] in start_moves, "ponderhit returns the completed legal result")

        engine.send("position startpos")
        move, _ = engine.best("go wtime 100 btime 100 winc 0 binc 0", timeout=4)
        check(move in start_moves, "clock-managed search completes with a legal move")
        engine.send("quit")
        engine.process.wait(timeout=4)
        check(engine.process.returncode == 0, "quit exits cleanly")
    except Exception:
        print("\n".join(engine.history[-80:]), file=sys.stderr)
        raise
    finally:
        engine.close()

    # GUIs and pipelines may close stdin instead of sending quit.
    eof = subprocess.Popen([str(executable)], stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        output, _ = eof.communicate("position startpos\ngo infinite\n", timeout=4)
        check(eof.returncode == 0, "EOF stops background search and exits cleanly")
    except subprocess.TimeoutExpired:
        eof.kill()
        eof.communicate()
        raise AssertionError("EOF did not stop background search")
    print(f"{checks} UCI checks passed")


if __name__ == "__main__":
    main()
