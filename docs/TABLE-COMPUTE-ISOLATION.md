# Compute table path: isolate the failing shader

## Current evidence

M20 ran clean source be20169a on USA PPSA01342 01.004 with RX6650XT.
Enabling TABLE_DISPATCH for a 14-second probe still severely overexposed the
same wall/character scene. Disabling it restored the lighting. The conservative
dependency changes did not fix the visual defect. Keep TABLE_DISPATCH=0 for play.
The earlier compute LOD-statistics explanation was incorrect: only the pixel
stage allocates that feedback binding. No FPS gain is accepted from this probe.

## Diagnostic selection (not yet built or tested)

The opt-in table path now supports selection by stable shader hash. Rejected
dispatches execute the full normal DispatchDirect path; none is skipped.
Pipeline/compiler caching and all normal special-path checks stay in place.

- KYTY_TABLE_DISPATCH_BUCKETS: decimal 32-bit mask, default 4294967295 (all).
  Bucket = (shader_hash XOR (shader_hash >> 32)) AND 31. Zero selects no shaders.
- KYTY_TABLE_DISPATCH_HASH_LO / HASH_HI: decimal low/high hash words, default
  zero/zero (no exact restriction). A nonzero combined hash selects only that
  shader, still subject to the bucket mask.
- KYTY_TABLE_DISPATCH remains default off and controls the entire experiment.

For live changes, disable TABLE_DISPATCH and wait for its live command to
complete before changing the two hash words, then enable it. Resolve every
symbol from the exact running executable's PDB. Live poke32 values are hex;
environment values are decimal. Do not reuse addresses from another build.

## Bounded next test

1. Preserve baseline scene screenshots and a cache backup; start compute off.
2. Confirm the all-zero bucket mask executes normal work and matches baseline.
3. Split the 32 buckets, inspect the same advancing gameplay scene briefly,
   restore compute off after every probe. Keep every hitch in the report.
4. Split failing buckets further, then select one exact shader at a time using
   its logged stable hash. Multiple shaders or interactions can be responsible.
5. Compare that shader's descriptors and output against the normal path, with
   all producer/consumer dependencies retained. Only a demonstrated semantic
   correction can be promoted. Do not accept a visually broken faster result.

No speedup, visual fix, or 30 FPS qualification is established by this selector.
