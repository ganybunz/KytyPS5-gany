# Pipeline hitch attribution (2026-10-07)

M19 continued beyond the frozen short Boletaria route and entered new views.
It recorded a378.3ms frame with306.5ms synchronous graphics-pipeline creation,
then a2563.2ms frame with2516.6ms recorded compile time, including a938.7ms
synchronous compute-pipeline creation. These are uncontrolled play observations,
not a same-route A/B or proof that the warmed cache prevents new-area hitches.

Luna source review: table shader variants return from ProgramCache::Get before
normal warmup recording. Native-binding graphics pipelines return before the
normal recipe is recorded. Table workers use a separate `.table.bin` cache.
However, the old `SLOW CreateGraphicsPipeline` line is emitted by the synchronous
path, not the table worker. `async_pipelines=4` in the frame does not identify
which key caused306.5ms. Ordinary new shader/state combinations remain possible.

The diagnostic change appends mode, binding path, driver-cache choice, stable
shader hashes and a process-local graphics key fingerprint to slow synchronous
creation logs. Separate `AsyncCreateGraphicsPipeline` / `AsyncCreateComputePipeline`
lines measure worker creation. Existing synchronous prefixes and VS/PS/CS IDs
are retained. The fingerprint includes process-local shader IDs; it is not a
portable cache key. Worker durations must not be added to frame compile time or
assumed to stall the GPU feed. This introduces no pipeline or shader shortcuts,
does not alter cache formats, and is not a performance fix.

Not built or game-tested. After owned exit, clean build and capture a repeated
new-view/effect route on Demon's Souls. Match stable hashes/mode/state to normal
warmup records and startup prewarm. Verify that the same route's second pass and
second boot reuse compiled pipelines. Only then extend recording/replay for a
demonstrated missing permutation, preserving cache identity, all draws, resource
specialization and AMD's full-build path. Do not enable the Nvidia fast-first
hint or skip a missing pipeline to hide the hitch. Rollback is the isolated
diagnostic commit; existing safe rendering paths remain in use.
