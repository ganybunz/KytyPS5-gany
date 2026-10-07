# Optional native draw region-epoch sampling

`KYTY_NATIVE_REGION_EPOCH_CACHE=1` is a boot-only CPU cost experiment, off by
default. M20's render-thread profile puts native resource validation at about
2.8% of busy self samples; inclusive native draw work is much larger and must
not be attributed entirely to this check. Expected FPS benefit is unmeasured.

Each normal buffer dependency adds every covered tracker region's epoch to its
per-range sum, then adds that to the record's aggregate. The experiment samples
each distinct region once per validation invocation, preserving its multiplicity
in every dependency. It rechecks all sampled epochs before using the result.
Changed epochs, deferred protection, missing trackers, invalid ranges, a zero
per-range sum, or more than 32 distinct regions return zero and retain the exact
dirty-range fallback. No state persists between invocations. Live buffer-owner,
mapping, image, registration and content checks remain in place.

The original range limit permits 64 tracker regions and rejects 65 or more.
The bounded cache may conservatively fall back sooner. A valid dependency in
region zero queries address one because GuestRange rejects address zero.
All sums retain uint64_t modular arithmetic and the old zero sentinel.

## Acceptance and rollback

1. Build and run `native_region_epoch_tests` after the current game exits. Compare
   stable aggregates with the old calculation, including duplicate dependencies,
   wraps, region-zero addresses and invalid/unknown/deferred ranges. Changed
   epochs during recheck must reject the proof.
2. Run only Demon's Souls, first without the switch, then with it; use the same
   warm cache/save, scene, route and remaining switches. Verify native XPR
   resources separately with KYTY_NATIVE_XPR=2, including CPU writes, buffer
   replacement/rebind and traversal. Table draws may bypass this validator;
   quantify its actual usage before expecting a benefit.
3. Compare clean matched frame percentiles and separate render-thread profiles.
   Reject visual/resource mismatches, new crashes, more misses or slower frames.
   Do not count a reduced atomic-read count as a measured FPS improvement.

Unset the variable to restore the original algorithm, or revert the isolated
commit. Standalone clang-cl /O2 CPU regression passed on October 7, including
600 deterministic differential cases. This built only one small translation
unit, without the emulator, Vulkan, or LTO. The integrated emulator build and
in-game verification remain pending; no promotion to the player package.
