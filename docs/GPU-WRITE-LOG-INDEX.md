# GPU write-log index

`KYTY_GPU_WRITE_LOG_INDEX` is read once on first use of the write log:

- Unset or `0` (and any other value): use the existing full scan.
- `1`: enable the conservative 64 KiB granule index.
- `2`: enable the index and compare every indexed answer with the full scan. A mismatch is reported, the reference answer is returned, and indexing is disabled for that log generation.

The index is a lazily allocated fixed array of 65,536 64-bit stamps (512 KiB maximum), hashed by 64 KiB guest granule. Collisions can move the backward scan start later, which is conservative. A query examines at most 256 granules, then walks backward from the newest candidate for at most 4096 entries. If that walk cannot prove the answer within the bound, it uses the full scan. Writes spanning more than 64 granules keep the full-scan path active until they retire. Queries spanning more than 256 granules also use the full scan.

Writes keep their absolute sequence through prefix retirement and compaction, so compaction does not rebuild the table. The table is zeroed when the log generation resets. Sequence numbers never wrap: on exhaustion, the index is released and disabled, while the retained log continues to use full scans. These paths preserve the existing overlap and tick rules, including undated writes returning `UINT64_MAX`.

This is a scan-cost optimization, not a measured frame-rate improvement. No emulator or GPU performance claim is made here.

Adapted from chenxiao f0f5db7bfb42f0644bab41a5c25d86520935d358.
The local log lacks upstream's counted-granule prerequisite, so this implementation
uses absolute stamps, a bounded hash table and conservative full-scan fallbacks.
Mode 2 prints a verified comparison count initially and every 65,536 comparisons.
An offline property model passed 20,740 comparisons (agent-reported); integrated
C++ compilation and gameplay verification are still pending.
