# Selfmate development

This is the source project for the Selfmate UCI engine. The parent directory is
a separate chess application collection; publish this project alone.

Both sides try to get themselves checkmated using standard legal chess moves.
Being checkmated is an objective win; delivering mate is an objective loss;
draws rank between them. Keep optional draw claims optional and checkmate ahead
of automatic draw adjudication. Normal-chess tablebases and evaluation scores
cannot establish results for this objective.

Build and test on Windows with `./build.ps1 -Test`. CMake/CTest is supported for
other platforms. Run `tests/uci_test.py` against the executable. The local chess
collection's Python is `../../runtime/python/python.exe`; CI uses standard
Python. Run the independent exhaustive search comparisons before changing
search bounds, transposition caching, or pruning. Benchmarks measure search
work and depth, not Elo or a proof of playing strength.

Preserve the vendored Stockfish source, GPL license and provenance. Exclude
local tools, compiled binaries, credentials, and the parent collection from Git.
