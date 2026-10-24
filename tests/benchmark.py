#!/usr/bin/env python3
"""Reproducible UCI search workload comparison, not a playing-strength test.

Run baseline and candidate serially with cold hash per position. Fixed-depth
scores may change when evaluation changes; reduced nodes alone prove no Elo.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import csv
import hashlib
import json
import math
import platform
import re
import statistics
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uci_test import Engine

ROOT = Path(__file__).resolve().parents[1]
POSITIONS = [
    ("start", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", ""),
    ("open_game", "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3", ""),
    ("kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", ""),
    ("closed_center", "r2q1rk1/pp1nbppp/2pbpn2/3p4/2PP4/2N1PN2/PPQ1BPPP/R1B2RK1 w - - 0 9", ""),
    ("pawn_endgame", "8/5pk1/6p1/4P2p/4K2P/6P1/8/8 w - - 0 40", ""),
    ("rook_endgame", "8/5pk1/6p1/3r3p/4K2P/6P1/5P2/3R4 w - - 0 40", ""),
    ("forced_selfmate", "8/8/1Q6/2n5/k1N5/2b5/NR6/KB6 w - - 0 1", ""),
    ("forced_selfmate_4ply", "8/8/1Q2N3/8/k1N1n3/2b5/NR6/KB6 w - - 0 1", ""),
    ("resisting_opponent", "8/8/1Q6/2n5/k1N5/2b5/NR6/KB6 w - - 0 1", "b6h6"),
]
VARIANTS = {
    "all": {"Use TT": True, "Use PVS": True, "Use Aspiration": True},
    "plain": {"Use TT": False, "Use PVS": False, "Use Aspiration": False},
    "tt_only": {"Use TT": True, "Use PVS": False, "Use Aspiration": False},
    "pvs_only": {"Use TT": False, "Use PVS": True, "Use Aspiration": False},
    "aspiration_only": {"Use TT": False, "Use PVS": False, "Use Aspiration": True},
}


def parse_info(lines):
    result = {}
    for line in lines:
        if not line.startswith("info depth "):
            continue
        fields = line.split()
        for name in ("depth", "nodes", "time", "nps"):
            if name in fields:
                try:
                    result[name] = int(fields[fields.index(name) + 1])
                except (ValueError, IndexError):
                    pass
        match = re.search(r"\bscore (cp|mate) (-?\d+)", line)
        if match:
            result["score_kind"], result["score"] = match[1], int(match[2])
        if " pv " in line:
            result["pv"] = line.split(" pv ", 1)[1]
    return result


def run_case(executable, label, options, case, mode, args, repeat):
    name, fen, searchmoves = case
    engine = Engine(executable)
    row = {"engine": label, "position": name, "mode": mode, "repeat": repeat,
           "fen": fen, "searchmoves": searchmoves, "status": "complete"}
    try:
        engine.send("uci")
        _, handshake = engine.until(lambda line: line == "uciok")
        row["engine_id"] = next((line[8:] for line in handshake if line.startswith("id name ")), "unknown")
        for name, value in options.items():
            engine.send(f"setoption name {name} value {str(value).lower()}")
        engine.send(f"setoption name Hash value {args.hash}")
        engine.send("ucinewgame")
        engine.ready()
        engine.send("position fen " + fen)
        command = f"go depth {args.depth}" if mode == "fixed_depth" else f"go movetime {args.movetime}"
        if searchmoves:
            command += " searchmoves " + searchmoves
        started = time.perf_counter()
        try:
            bestmove, lines = engine.best(command, timeout=args.timeout)
        except AssertionError:
            row["status"] = "timeout"
            engine.send("stop")
            line, _ = engine.until(lambda line: line.startswith("bestmove "), timeout=5)
            bestmove = line.split()[1]
            lines = [line[2:] for line in engine.history if line.startswith("< ")]
        row["elapsed_ms"] = round((time.perf_counter() - started) * 1000, 3)
        row["bestmove"] = bestmove
        row.update(parse_info(lines))
        if mode == "fixed_depth":
            row["requested_depth"] = args.depth
            row["completed_requested_depth"] = row.get("depth", 0) >= args.depth
            row["terminal_proof"] = row.get("score_kind") == "mate"
        else:
            row["requested_ms"] = args.movetime
        errors = [line for line in lines if "info string error" in line or "search error" in line]
        if errors:
            row["status"] = "error"
            row["errors"] = errors
    finally:
        engine.close()
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=ROOT / "tools/baseline/selfmate-0.1.exe")
    parser.add_argument("--candidate", type=Path, default=ROOT / "bin/selfmate.exe")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--baseline-only", action="store_true")
    group.add_argument("--candidate-only", action="store_true")
    parser.add_argument("--depth", type=int, default=6)
    parser.add_argument("--movetime", type=int, default=1000)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--hash", type=int, default=16)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--variant", action="append", choices=list(VARIANTS),
                        help="candidate optimization configuration; repeat for ablation runs")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--fixed-only", action="store_true")
    modes.add_argument("--timed-only", action="store_true")
    parser.add_argument("--case", action="append", choices=[case[0] for case in POSITIONS])
    parser.add_argument("--out", type=Path, default=ROOT / "benchmarks/search-comparison")
    args = parser.parse_args()
    if min(args.depth, args.movetime, args.hash, args.repeat) <= 0:
        parser.error("depth, movetime, hash and repeat must be positive")
    engines = []
    if not args.candidate_only:
        engines.append(("baseline", args.baseline.resolve(), {}))
    if not args.baseline_only:
        for variant in args.variant or ["all"]:
            label = "candidate" if variant == "all" else "candidate_" + variant
            engines.append((label, args.candidate.resolve(), VARIANTS[variant]))
    for _, path, _ in engines:
        if not path.is_file():
            parser.error(f"engine does not exist: {path}")
    cases = [case for case in POSITIONS if not args.case or case[0] in args.case]
    search_modes = [mode for mode in ("fixed_depth", "fixed_time")
                    if not (args.fixed_only and mode == "fixed_time")
                    and not (args.timed_only and mode == "fixed_depth")]
    report = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "purpose": "Search workload and completed depth; no playing-strength or Elo claim.",
        "platform": platform.platform(), "processor": platform.processor(),
        "python": sys.version, "argv": sys.argv[1:], "hash_mb": args.hash,
        "threads": 1, "ponder": False, "cold_hash_per_case": True,
        "binaries": {label: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "options": options}
                     for label, path, options in engines},
        "candidate_source_sha256": {
            str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted((ROOT / "src").glob("*")) if path.is_file()},
        "rows": [],
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    output_base = args.out.with_suffix("") if args.out.suffix.lower() in (".json", ".csv") else args.out
    output_json = Path(str(output_base) + ".json")
    for repeat in range(1, args.repeat + 1):
        for case in cases:
            for mode in search_modes:
                for label, executable, options in engines:
                    row = run_case(executable, label, options, case, mode, args, repeat)
                    report["rows"].append(row)
                    output_json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
                    print(f"{label:25} {case[0]:20} {mode:11} depth={row.get('depth', '?'):2} "
                          f"nodes={row.get('nodes', '?'):9} ms={row['elapsed_ms']:9.3f} "
                          f"{row.get('score_kind', '?')}={row.get('score', '?')} {row['status']}", flush=True)
    report["comparisons"] = []
    for label, _, _ in engines:
        if label == "baseline":
            continue
        for case in cases:
            for mode in search_modes:
                baseline = [row for row in report["rows"] if row["engine"] == "baseline"
                            and row["position"] == case[0] and row["mode"] == mode and row["status"] == "complete"]
                candidate = [row for row in report["rows"] if row["engine"] == label
                             and row["position"] == case[0] and row["mode"] == mode and row["status"] == "complete"]
                if not baseline or not candidate:
                    continue
                base_ms = statistics.median(row["elapsed_ms"] for row in baseline)
                cand_ms = statistics.median(row["elapsed_ms"] for row in candidate)
                comparison = {"candidate": label, "position": case[0], "mode": mode,
                              "baseline_median_ms": base_ms, "candidate_median_ms": cand_ms,
                              "baseline_median_depth": statistics.median(row.get("depth", 0) for row in baseline),
                              "candidate_median_depth": statistics.median(row.get("depth", 0) for row in candidate),
                              "baseline_median_nodes": statistics.median(row.get("nodes", 0) for row in baseline),
                              "candidate_median_nodes": statistics.median(row.get("nodes", 0) for row in candidate)}
                if mode == "fixed_depth":
                    comparison["elapsed_speedup"] = base_ms / cand_ms if cand_ms else None
                    comparison["same_score"] = {(row.get("score_kind"), row.get("score")) for row in baseline} == {
                        (row.get("score_kind"), row.get("score")) for row in candidate}
                report["comparisons"].append(comparison)
    proof_positions = {row["position"] for row in report["rows"] if row.get("score_kind") == "mate"}
    ordinary = [row for row in report["comparisons"] if row["mode"] == "fixed_depth"
                and row["position"] not in proof_positions]
    report["aggregate_excluding_early_mate_proofs"] = []
    for label, _, _ in engines:
        values = [row for row in ordinary if row["candidate"] == label]
        if not values:
            continue
        node_ratio = math.exp(statistics.mean(math.log(row["candidate_median_nodes"] / row["baseline_median_nodes"])
                                               for row in values))
        elapsed_ratio = math.exp(statistics.mean(math.log(row["candidate_median_ms"] / row["baseline_median_ms"])
                                                  for row in values))
        report["aggregate_excluding_early_mate_proofs"].append({
            "candidate": label, "positions": len(values), "geomean_candidate_baseline_node_ratio": node_ratio,
            "geomean_candidate_baseline_elapsed_ratio": elapsed_ratio,
            "fixed_depth_scores_agree": all(row["same_score"] for row in values)})
        print(f"{label}: geometric mean node ratio={node_ratio:.4f}, elapsed ratio={elapsed_ratio:.4f} "
              f"over {len(values)} non-proof positions; lower is less work/time, not Elo.")
    output_json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    columns = ["engine", "position", "mode", "repeat", "status", "depth", "nodes", "elapsed_ms",
               "time", "nps", "score_kind", "score", "bestmove", "pv", "requested_depth", "requested_ms",
               "completed_requested_depth", "terminal_proof", "engine_id", "fen", "searchmoves", "errors"]
    with Path(str(output_base) + ".csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(report["rows"])
    print(f"Saved {output_json}")
    return int(any(row["status"] != "complete" for row in report["rows"]))


if __name__ == "__main__":
    raise SystemExit(main())
