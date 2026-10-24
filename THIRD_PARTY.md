# Third-party source and licensing

Selfmate is distributed under **GNU GPL version 3 or later**. The full license is in [COPYING](COPYING).

## Stockfish 17.1

- Upstream: https://github.com/official-stockfish/Stockfish
- Release tag: `sf_17.1`
- Pinned commit: `03e27488f3d21d8ff4dbf3065603afa21dbd0ef3`
- License: GPL-3.0-or-later; upstream notices, license and author credits remain in `vendor/stockfish`.

The unmodified upstream source is preserved in `vendor/stockfish`. The engine compiles upstream `src/bitboard.cpp` and `src/movegen.cpp`, and uses the upstream chess rule headers. It does **not** compile Stockfish's search, evaluation, NNUE, normal-chess transposition table, UCI engine, or Syzygy tablebase implementation. Standard-chess tablebase results cannot be used as results for this variant.

`src/stockfish_position.cpp` is derived from upstream `src/position.cpp`. Its copyright and license header is retained. The exact adaptations are:

1. Remove the includes for `syzygy/tbprobe.h`, `tt.h`, and `uci.h`.
2. Replace the two `UCIEngine::square` calls with a local algebraic square formatter (`a1` through `h8`).
3. Remove normal-chess Syzygy WDL and DTZ probes from the optional ASCII position printer.
4. Remove normal-chess transposition-table prefetches from `Position::do_move` and `Position::do_null_move`, retaining their parameter types for header/API compatibility and leaving the unused parameters unnamed.
5. Update the adjacent transposition-table prefetch comment to explain the adaptation.

Legal move generation, check detection, castling, en passant, promotion, move/undo, repetition bookkeeping, and board hashing are inherited from Stockfish. Selfmate's search and objective are implemented separately in `src/engine.cpp`.

The 0.2 search in `src/search.cpp` was developed after inspecting Stockfish's
search, move picker, history, and transposition-table implementations at commit
`effdc3fd3f6de6e75fe1374a51a7a7eed66f880b`. Techniques adapted include principal
variation search, aspiration re-searches, bounded history updates, clustered
cache replacement, and mate-score normalization. The objective, draw-history
cache identity, and fixed-horizon validation are specific to Selfmate. See
`docs/SEARCH.md` for pinned upstream links and design details. This does not
change the pinned 17.1 rules source compiled under `vendor/stockfish`.

## Optional local build compiler

The Windows build script first uses a local Zig compiler if present at `tools/zig-x86_64-windows-0.14.1/zig.exe`. The compiler is a development tool, not part of the engine's runtime requirements or linked Stockfish source. Zig's included license files remain in its extracted directory. A system C++17 compiler can be selected with `build.ps1 -Compiler <path>` or CMake.

The downloaded Zig 0.14.1 Windows x86-64 ZIP used for this workspace had SHA-256 `554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c`.
