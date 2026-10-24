# Search design and Stockfish source study

Selfmate uses ordinary chess legality with a different zero-sum objective: each side tries to force its own king to be checkmated. A side with no legal moves scores `+30000 - ply` if checked, or zero if stalemated. Negation makes checkmating the other side a loss. Getting checkmated therefore outranks a draw by a large margin; heuristic evaluations remain below the mate-score range.

## Source versions

The rules core remains pinned to Stockfish 17.1, commit `03e27488f3d21d8ff4dbf3065603afa21dbd0ef3`. For the second search implementation, we also read the current upstream source pinned to **`effdc3fd3f6de6e75fe1374a51a7a7eed66f880b`**. This is a source reference, not an automatically updated dependency. The examined checkout is under `tools/stockfish-current`.

The source study covered these primary implementations:

| Upstream implementation | What Selfmate adopts | Adaptation for this objective |
|---|---|---|
| [Iterative deepening and aspiration windows](https://github.com/official-stockfish/Stockfish/blob/effdc3fd3f6de6e75fe1374a51a7a7eed66f880b/src/search.cpp#L385) | Start later iterations with a window around the previous score; widen and repeat on failure. | Every retry retains the full requested depth. No fail-high depth reduction or optimism correction is used. |
| [Principal variation search](https://github.com/official-stockfish/Stockfish/blob/effdc3fd3f6de6e75fe1374a51a7a7eed66f880b/src/search.cpp#L1426) | Search the first move with the full window; test later moves with a zero-width scout, then re-search an improvement inside the window. | Both scout and re-search use the same full depth. No late move reduction or extension is applied. |
| [Clustered transposition table](https://github.com/official-stockfish/Stockfish/blob/effdc3fd3f6de6e75fe1374a51a7a7eed66f880b/src/tt.cpp#L63) | Store score, depth, move, bound and generation; prefer useful deep/recent entries for replacement. | Four entries occupy one 64-byte cluster. Every entry retains a full 64-bit context key, claim metadata, and an exact horizon. |
| [Mate score conversion](https://github.com/official-stockfish/Stockfish/blob/effdc3fd3f6de6e75fe1374a51a7a7eed66f880b/src/search.cpp#L1937) | Normalize mate distance from root-relative to node-relative on storage and reverse that on retrieval. | The sign describes winning the selfmate objective. Normal-chess tablebase values and 50-move mate downgrades are not imported. |
| [Bounded history updates](https://github.com/official-stockfish/Stockfish/blob/effdc3fd3f6de6e75fe1374a51a7a7eed66f880b/src/history.h#L50) | History gravity: `h += bonus - h * abs(bonus) / limit`. | Quiet, capture and one-move continuation histories learn cutoff moves under the selfmate search. |
| [Move picking and scoring](https://github.com/official-stockfish/Stockfish/blob/effdc3fd3f6de6e75fe1374a51a7a7eed66f880b/src/movepick.cpp#L216) | Prefer a cached move, learned history and useful previous-move context. | Incremental maximum selection avoids sorting moves that a cutoff never searches. Killer moves and countermoves supplement the learned scores; ordinary MVV and SEE do not remove moves. |
| [Statistics rewards and penalties](https://github.com/official-stockfish/Stockfish/blob/effdc3fd3f6de6e75fe1374a51a7a7eed66f880b/src/search.cpp#L1987) | Reward a successful cutoff move and penalize earlier unsuccessful moves. | Bonuses saturate smoothly and affect ordering only. |

The constants and implementation are intentionally smaller than Stockfish's current search. In particular, its tuned ordinary-chess pruning logic is not suitable for direct reuse just because the move generator is the same.

## Exact search at the selected horizon

Alpha-beta, principal variation search, aspiration windows and move ordering change how much work is done, while preserving the minimax score at the completed depth. A scout that improves alpha inside the open window is always re-searched at full depth. Failed aspiration searches widen their window until the result lies strictly inside it. Interrupted iterations never replace the last completed result.

The engine searches all legal move types, including quiet moves, checking moves, king moves and underpromotions. It uses neither capture-only quiescence nor selective depth changes. Terminal and draw detection occur even at depth zero. `evaluateLeaf` also recognizes optional claims available before an intended move, so a horizon position with an available draw action cannot receive a negative leaf score.

Scores from a transposition entry are reusable only at **the same remaining depth**. A deeper heuristic result is not automatically a bound on the minimax result of a shallower fixed horizon. Entries from other depths still provide an ordering move.

One eighth of the selected hash budget is reserved for a separate board-only move hint table; the remaining seven eighths holds the clustered context-safe score table. A move hint contains no reusable score and is accepted only if its full board key matches and the hinted move occurs in the current legal move list. It can therefore improve ordering across histories without confusing their draw rights. A context-matched move takes priority over this fallback. Only completed nodes save a valid best-move hint, and disabling `useTT` disables both tables.

Decisive cached scores do not cut off a principal variation node. Its mate line is searched to the terminal position rather than depending on the continued presence of every child entry in the cache. Other cached principal variation nodes can return a legal prefix, which may be shorter than the reported search depth. The search does not extend that prefix past a chosen draw claim.

## Repetition-safe cache keys

Caching by current board alone is unsafe because the result can depend on earlier positions. Adding only the number of times the **current** board has appeared is also insufficient: another board that may recur later can already have appeared twice in one history and only once in another.

Each node carries an order-independent sum of mixed raw board keys for **every available position since the last pawn move or capture, including the current position**. Repeated keys contribute repeatedly, so this fingerprints a multiset with multiplicities. A cache key combines that context, the current raw board key, and the exact halfmove count. Root context is built from the supplied `StateInfo` history once; each child adds one mixed key, or resets the context after a zeroing move. Updating the context therefore takes constant time per move.

Given the same board and this complete history multiset, the available future repetition counts and draw-claim actions are the same. Order before the last irreversible move is irrelevant. Retaining positions before a castling-rights change or raw en passant rights that never allowed a legal capture can distinguish equivalent histories, reducing cache hits without making results unsound. Exact draw adjudication still uses canonical legal en passant rights in `repetitionCount`; it does not use the cache fingerprint.

These remain 64-bit hash fingerprints, with the usual probabilistic collision limitation of chess transposition tables. The cache stores full signatures rather than Stockfish's shorter entry signatures. A FEN alone has no earlier position history; callers must provide actual setup moves to make those past repetitions available.

At the root, `go searchmoves` changes the available action set. Root score entries are neither used nor written for such a restricted search. Descendant nodes still have their full legal action sets and can use the regular cache safely.

## Draw claims are actions

After checking mate and stalemate, an automatic draw returns zero. A currently available optional claim contributes an additional zero-valued action; a positive continuation can still replace it. Before evaluating each ordinary move, the search also checks whether declaring that intended move would permit a claim. That claim belongs to the mover and contributes zero separately from actually playing the move, even when playing it would checkmate the opponent.

Current and intended claims, automatic draws, and the exact supplied repetition history are included in the search semantics. A cached chosen claim retains its flag so the UCI interface can tell the user to claim through the GUI. UCI itself has no draw-claim action, so it still emits a legal fallback `bestmove`.

## Controls and measurement

`SearchOptions` exposes independent `useTT`, `usePVS`, and `useAspiration` toggles. These permit comparisons against the same evaluator at the same fixed depth. `SearchStats` reports `ttHits`, `ttCutoffs`, `betaCutoffs`, `pvsResearches`, and `aspirationResearches`; counters reset for each root search. A hit can contribute ordering without supplying a score cutoff.

Move lists are reserved once per ply; scores and principal variations use fixed arrays. The large continuation history and per-ply storage live on the heap, keeping the Windows thread stack small. The search does not allocate a new move list, score vector, or principal variation vector at each node.

Tests compare these options with an independent exhaustive negamax reference, including histories and draw claims. Fixed-depth node counts and timings measure search efficiency; they do not establish ordinary-chess Elo or prove that the heuristic is globally optimal for selfmate. Mate proofs remain bounded by the completed horizon, and general blocked dead positions remain a rules limitation documented in the README.

## Techniques deliberately excluded

Null moves, futility pruning, razoring, ProbCut, late move reductions, singular extensions, ordinary NNUE, normal-chess mate-distance pruning, and standard Syzygy WDL/DTZ scores are not part of this implementation. Their ordinary-chess assumptions require separate derivation and testing for a game where the player wants to be mated. The new search retains standard chess legality and the adversarial selfmate objective throughout.
