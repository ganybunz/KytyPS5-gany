# Native graphics pipeline warmup

The native-XPR variant uses the same normal SPIR-V but a different descriptor
layout: flattened SRT/shader-data bindings become dynamic storage buffers.
`GraphicsPipelineKey` already includes this variant bit. Existing startup recipes
omit it, and native pipelines return before recording, so startup cannot replay
those exact variants. This is a demonstrated coverage gap, not proof that it
caused every observed long freeze. Table shaders use a separate asynchronous path
and remain outside this patch.

`KYTY_NATIVE_PIPELINE_WARMUP=1` enables recording and replay of successfully
published native graphics variants. The complete rendering, vertex-input and
static key plus recorded shader indices are retained. Asynchronous jobs record
on adoption on the render thread; workers do not mutate the recording cache.
Hot cache hits do not serialize a recipe. The recorded bit is also used for the
startup cache key and descriptor-layout construction. Disabled mode retains
native recipes but does not replay or add them. Every draw still follows its
normal/native fallback and compilation rules.

The file schema becomes `KytyShaderWarmup4`; shader records are unchanged.
Strictly valid same-signature v3 files migrate in memory with a false native bit,
preserving shader indices and recipe order. Checksum, semantic, bound and exact
identity validation remain mandatory. New seeds use `KytyShaderSeeds3`, old
`KytyShaderSeeds2` recipes migrate as ordinary bindings. Back up caches before
the first new build: an old executable cannot read the new file schema. Player
and test caches are separate and no running-process file is rewritten manually.

Acceptance after the current owned game exits:

1. Clean single-worker build; run migration, malformed-file and native-bit
   roundtrip cases plus existing shader warmup tests. Verify old learned shader
   words/indices and pipeline state survive migration exactly.
2. On Demon's Souls, enable only this option above the accepted configuration;
   warm cache unchanged otherwise. Play the observed stand/attack/traversal route
   with new effects. Match slow-create records to `native=1` and shader hashes.
3. Preserve the learned cache after exit, boot again with the option enabled,
   and verify those exact native keys are prepared during startup. Compare both
   route frame tails and startup cost. Include paging and cache saves in results.
4. Reject rendering, ownership or load regressions. No skipped draw/pipeline is
   an acceptable hitch fix. Unset the option for replay rollback; use the backed
   up v3 cache when reverting the executable.

Status: unbuilt and game-unverified. This adds learned coverage only; unseen
effects and table permutations can still miss. No hitch-elimination/FPS claim.

Independent Luna review found one missed secondary call site before any build:
static seed precompile must pass the native bit to descriptor-layout creation as
well. Corrected separately; otherwise a native-labelled recipe would populate
the ordinary pipeline variant. Both startup replay and explicit seed compilation
now use the saved variant. Build/Vulkan verification remains pending.
