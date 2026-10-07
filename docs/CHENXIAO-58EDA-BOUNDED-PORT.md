# Bounded CPU port from chenxiao 58eda700

Source: https://github.com/chenxiao07/KytyPS5/commit/58eda700ee0382d7fde1b8d82f1e88777b2734d8

Fetched Oct7. The latest remote tip is 0a1f06d92514ac5bb57f1c58d4c1ed653c445a6a.
This is a selected port, not a merge of every new fork change.

## Implemented, not built or measured yet

- Reuse bounded per-thread BDA-plan range storage. EvaluateBdaReadPlan is pure
  and non-recursive. Clear scratch for each evaluation; invalid plans preserve
  the caller's output; valid plans still sort and merge identical intervals.
- Reuse the caller's two range vectors in PrepareBdaBindings. All existing
  bounded/unbounded fallback and synchronization checks remain.
- TableBlocks uses subtract/compare indexing instead of remainder in its two
  buffer passes. Empty buffer lists still execute zero iterations.
- Cache all 128 seven-bit buffer format classes once. RawFormat is masked to
  seven bits by ShaderBufferResource, and unknown classes retain the same zero.
- Compare stored dynamic-state hashes before the existing full byte comparison.
  The hash is computed after storage; collisions still require byte equality.

These reduce repeated allocation/CPU work and are equivalent operations. The
fork author's small Latria result on another setup is not a measured gain here.
They do not remove the genuine current-tick readback dependency or prove 30 FPS.

## Deferred portions and rejection

The flat V# operand-load portion requires a different TablePair flat-form and
TableEvaluate implementation absent from our source. Do not copy its field
accesses without those prerequisites and descriptor equivalence coverage.
The newer null-texture recovery 0a1f06d9 can hide an invalid nonzero descriptor
or omit rendering; it is not imported as a generic crash workaround.

## Verification and rollback

Build only after the owned game exits. Run BDA-plan/resource tests, check
failure output and repeated calls, then same-view draw/table A/B on Demon's
Souls with the learned warm cache. Keep compile/streaming hitches in the
gameplay report. Inspect dynamic-state changes and camera transitions. Revert
this isolated port if CPU time worsens or any output differs. No player package
has been replaced for this unmeasured candidate.
