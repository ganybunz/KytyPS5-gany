# Bounded depth fallback batches (2026-10-07)

## Scope and rollback

Opt-in `KYTY_DEPTH_FALLBACK_BATCH=1`, together with the existing
`KYTY_DEPTH_FALLBACK_SCRATCH_REUSE=1`. Default is off. Unset the new flag to
keep the single-layer scratch path; unset both to use the original full-image
detile. No player package is changed by this experiment.

The observed Demon's Souls 01.004 pool has 80 layers of 4 MiB. The single-layer
path removes the old 320 MiB temporary but records 80 image uploads, each with
buffer and image barriers. A batch uses up to eight contiguous layers and up
to 32 MiB (also bounded by staging capacity), reducing that case to ten uploads.
There are still 80 tile dispatches and every layer is copied. Scratch may grow
from 4 to 32 MiB; this is a tradeoff, not a total-memory savings claim.

## Correctness constraints

- Preserve original whole-source acquisition and synchronization, including
  image-owned GPU aliases. Never substitute CPU backing for that source.
- Keep single-mip, non-stencil, non-promoted depth eligibility and the existing
  copy/tiling checks. Batch only equal contiguous input and output slices.
- Rebase offsets within the batch; preserve each tile's original `surface_z`
  and each copy's depth aspect, pitch, image extent and destination array layer.
- Keep detile clearing, read-to-write reuse ordering and `Image::Upload`
  barriers. Retain ring-backed input after each consumer batch.
- Retire the scratch lease after the final consumer, including any intermediate
  scheduler submissions caused by parameter-ring wrap.
- Last short batch uses its actual byte count. Stack metadata is bounded to
  eight tiles/copies; there is no per-layer metadata heap allocation.

## Verification required before promotion

At preparation, this source change has **not been built or game-tested**.
The running M19 executable is 12b52f94 and does not contain it.

After the owned game exits, commit and build with the bounded linker helper.
Compare flags off/on on Demon's Souls only, same warmed cache/save/view and
quality settings. Require the log to show the expected batch and scratch size.
Check every depth layer via a captured DS upload replay if available, including
first/last layers, shadows, partial updates and transitions. Exercise attack and
traversal twice plus a long gameplay session. Record frame tails, load/refresh
time, resident RAM, Vulkan allocation attribution and crashes. Reject or disable
on corruption, worse frame tails or memory pressure. No FPS improvement is
established by the source-level reduction in upload calls.
