# Selfmate 0.2

[Source on GitHub](https://github.com/Jammies13/selfmate) · GNU GPL v3 or later

A UCI chess engine where **each player tries to force the opponent to checkmate it**. Both players pursue that objective and resist helping the other player. All moves obey ordinary chess rules, including the requirement to escape check.

For the side being evaluated, the results rank:

1. Get checkmated: win.
2. Draw or stalemate: draw.
3. Checkmate the opponent: loss.

This is an adversarial selfmate search built on Stockfish 17.1's board representation and legal move generation. Its search and evaluation are specific to this objective.

## Run on Windows

From this directory in PowerShell:

```powershell
.\build.ps1 -Test
.\bin\selfmate.exe --play
```

The build script uses the local compiler in `tools/zig-x86_64-windows-0.14.1` when present, or a system `clang++`/`g++`. To choose a C++17 compiler explicitly, use `build.ps1 -Compiler <path>`. It downloads nothing. MSVC builds can use CMake:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Linux or macOS with CMake and a C++17 compiler, run `sh ./build.sh --test`.

In `--play` mode you play White and the engine plays Black. Enter moves as `e2e4`, `g1f3`, or `a7a8n` for promotion. The local console also accepts `board`, `legal`, `hint`, `fen <six-field FEN>`, `claim`, `claim <intended move>`, and `quit`. For example, `claim g1f3` claims a valid draw before that intended move. The default response search uses depth 6 with a 1-second time budget.

## Analyze a position

The PowerShell helper performs the UCI handshake and waits for `bestmove` before closing the engine:

```powershell
.\analyze.ps1
.\analyze.ps1 -Moves e2e4,e7e5 -Depth 6 -MoveTimeMs 2000
.\analyze.ps1 -Fen '8/8/1Q6/2n5/k1N5/2b5/NR6/KB6 w - - 0 1' -Depth 4
```

The last example has a forced selfmate: **1. Qb3+ Nxb3#**. Black's only legal reply checkmates White, so White achieves its objective even though Black resists. The white rook on b2 is pinned by the bishop on c3 and cannot capture the mating knight. At depth 2 or greater the engine reports a positive mate score and selects `b6b3`.

An available checkmate is not necessarily forced. In that same position, **1. Qh6** lets Black play `Nxb3#`, but Black has other legal replies and can refuse. The search treats those alternatives as choices for an adversary.

`-MoveTimeMs 0` removes the time limit; the helper still has a five-minute timeout. Depth and time limits both apply when specified, so a timed search can finish before reaching its requested depth.

## Use with a chess GUI

Add `bin/selfmate.exe` as a UCI engine. Launching it without arguments starts the UCI interface. A sample command sequence is in `examples/selfmate.uci`; send `quit` after the engine has returned `bestmove`. Piping `quit` immediately after `go` can stop the search early.

Common commands include `uci`, `isready`, `ucinewgame`, `position startpos moves ...`, `position fen ...`, `go depth ...`, `go movetime ...`, `stop`, and `quit`.

Scores describe **Selfmate's objective**, from the side to move at the search root. Positive scores favor getting that side checkmated; negative scores favor the opponent's objective. A positive `score mate` means forced selfmate, while a negative mate score means being forced to checkmate the opponent. A conventional GUI may describe those scores or the final winner using ordinary chess terminology.

Threefold repetition and the 50-move rule are optional draw claims. The search can decline a claim when it has a better continuation, and can claim before an intended move that meets the rule. Fivefold repetition and 75 moves without a pawn move or capture are automatic draws, with checkmate taking precedence. These follow [FIDE's draw rules](https://handbook.fide.com/chapter/e012023). Repetition history must be supplied through `position ... moves ...`; a FEN alone does not contain earlier positions.

UCI has no standard draw-claim command. When a draw claim is the engine's choice it emits an explanatory `info string` and a legal fallback `bestmove`; claim the draw through the GUI. The GUI's own adjudication settings may end games earlier.

## Search and limitations

Iterative deepening and alpha-beta negamax search model both players as trying to get themselves mated. Being checkmated receives a terminal score near +30,000, compared with 0 for a draw and at most ±8,000 for a heuristic position score. This gives a proven selfmate overwhelming priority over drawing. Terminal scoring also penalizes mating the opponent and prefers quicker wins. A separate position evaluation estimates how exposed or confined each king is and what mating resources the opponent retains.

Version 0.2 adds principal variation search, aspiration windows with full-depth re-searches, bounded history and continuation move ordering, and a clustered transposition table. Cached scores include the complete available reversible-position history and halfmove clock, so positions with different repetition rights cannot share a score. The search keeps the same fixed-depth objective as full-width minimax; an independent exhaustive reference checks those results.

The engine searches legal moves at every ply. It does not use Stockfish's ordinary NNUE evaluation, null moves, late move reductions, capture-only quiescence, or normal-chess tablebases. Stockfish's original source is retained under `vendor/stockfish`; a small adapted position implementation removes dependencies on its ordinary search infrastructure. The [search design and source audit](docs/SEARCH.md) explain the changes and link to the exact upstream revision inspected.

The UCI check options `Use TT`, `Use PVS`, and `Use Aspiration` allow each optimization to be disabled for comparisons. Search statistics are printed before `bestmove`. `MultiPV` is limited to one line.

This is a finite-depth engine. It can prove selfmates within its completed search, but does not solve arbitrary positions or guarantee the globally worst ordinary chess move. Quiet selfmate plans beyond the horizon can remain invisible. It detects familiar material dead positions conservatively: bare kings, a single minor piece, or bishops confined to the same square color. Other dead positions, such as some permanently blocked pawn structures, are not recognized automatically.

## License

GNU GPL version 3 or later; see `COPYING` and `THIRD_PARTY.md`. Stockfish is pinned to tag `sf_17.1`, commit `03e27488f3d21d8ff4dbf3065603afa21dbd0ef3`. Upstream copyrights, license and author credits are preserved. If you distribute the compiled engine, comply with the GPL's corresponding-source requirements.
