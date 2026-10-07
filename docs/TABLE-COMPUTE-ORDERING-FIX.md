# Table compute ordering correction (2026-10-07)

M19, executable 12b52f94, Demon's Souls PPSA01342 01.004 on Ryzen 5 5500 /
RX 6650 XT: enabling `KYTY_TABLE_DISPATCH=1` in already qualified tunnel
gameplay caused severe overexposure. Disabling it restored the prior lighting
in the same process. The 23 FPS sample with corrupt rendering is rejected.
Screenshots and switches are in Performance Experiments/M19-12b52-scoped-readback.

Source inspection found the table path used `ChainHandle()` and
`ContinueComputeChain()` solely because the title was Demon's Souls. It neither
honored the normal path's `KYTY_DS_COMPUTE_CHAIN` setting nor checked or recorded
`ChainAccess` ranges. The normal path explicitly checks dependent buffers,
images and indirect arguments. Interleaving table/native dispatches could also
leave the normal path's proof unaware of a table dispatch's accesses.

Correction: a table dispatch calls `Handle()` to retire a pending normal chain,
retains storage-write and indirect-argument dependencies, and records the full
post-dispatch shader barrier. Storage image writes now participate in the
write-hazard decision as in normal dispatches. Until a complete access proof is
implemented, table dispatches do not elide barriers. Draw table mode is separate.

Also copy compute LOD-statistics words prepared by `TableEvaluate` to push data;
previously only table pixel draws populated those fields. PushData is already
zero-initialized, so this is a missing value, not uninitialized storage.

This fixes concrete source omissions but **does not yet prove the visual
regression is fixed**. Runtime buffer stride on AMD remains a separate suspect.
The current game continues to run the old executable with table compute OFF.
This correction is not built or game-tested yet. No speedup is claimed.

Next verification: after the owned game exits, build cleanly with one linker
worker. On Demon's Souls only, compare native compute with corrected table
compute using the same warmed cache/save/camera and table draw setting. Confirm
lighting, shadows, attacks and traversal first; compare full frame tails and
GPU busy time afterward. Include normal/table interleaving, indirect dispatches,
storage-image writes, pointer access and chain mode 0. Keep table compute off
on any discrepancy. The feature remains default-off; reverting this source
commit also rolls back the correction without changing normal dispatches.
