# Search comparison: 0.2 versus 0.1

The [raw JSON](../benchmarks/search-comparison.json) and
[CSV](../benchmarks/search-comparison.csv) preserve all 108 measurements,
positions, binary and source hashes, settings, and failures. All cases completed.

Both binaries ran serially on the same Windows machine, using one search thread,
16 MB hash, cold caches per position, and three repetitions. Nine positions were
tested at depth 6 and with a one-second time budget. The evaluator and objective
were unchanged, making the fixed-depth comparison a test of search efficiency.

Across the seven positions that did not finish early with a mate proof, the
geometric mean candidate/baseline ratio was **0.8939 for nodes** and **0.9242 for
elapsed time**: 10.6% fewer nodes and 7.6% less elapsed time. Every fixed-depth
score matched the baseline. Elapsed time includes UCI communication overhead;
small millisecond measurements should not be treated as precise calibration.

Median completed depths with the same one-second budget:

| Position | 0.1 | 0.2 |
|---|---:|---:|
| Initial position | 8 | 8 |
| Open game | 7 | 7 |
| Kiwipete | 6 | 6 |
| Closed center | 7 | 7 |
| Pawn ending | 11 | 12 |
| Rook ending | 8 | 9 |
| Opponent can refuse to mate | 8 | 9 |
| Forced selfmate in two plies | 2, proved | 2, proved |
| Forced selfmate in four plies | 4, proved | 4, proved |

The improvement is not uniform. The closed-center depth-6 case regressed from
108,279 to 179,114 nodes, with median elapsed time approximately 34.1 to 53.4 ms.
That result is retained in the report rather than omitted.

Validation also passed 94 chess/objective checks, 2,674 search checks, and 24 UCI
process checks. The search checks use an independent exhaustive reference over
76 varied positions and five optimization configurations, exercise cached mate
distances at new roots, and replay mate principal variations to actual checkmate.

These results establish correctness checks and a modest measured search gain,
including one additional ply on three test positions. They are not an Elo
estimate or a statistical playing-strength test. Further tuning is deferred.

To repeat with local baseline and candidate binaries:

```powershell
python tests/benchmark.py --baseline PATH_TO_0.1.exe --candidate bin/selfmate.exe --depth 6 --movetime 1000 --repeat 3 --out benchmarks/repeated-comparison
```
