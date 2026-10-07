# Required readback producer attribution

M20's synchronous fault at `0x20ed7fda0` requests one byte and overlaps the latest
608-byte GPU write declaration at `0x20ed7fce0`, in the current submission tick.
The one-byte size is the fault handler's request, not the shader's complete read
footprint. Required-page narrowing reduced copied bytes but did not remove the
roughly 16 ms completion wait or produce a persuasive FPS gain.

The next diagnostic adds trace event 40 after each event 26 on the same thread.
It records the enclosing draw/dispatch shader address, command kind and origin:
writable binding declaration, CPU upload, buffer join copy, or image download.
Nested command scopes restore the previous context. Existing ticks, ownership,
copies and barriers are unchanged. No trace records are emitted unless `tracew`
is active. The scope maintains two thread-local context values even when tracing
is off; this is diagnostic bookkeeping, not an FPS optimization.

An enclosing shader address is not a stable shader-code hash. A writable binding
only declares potential writes. Uploads/joins during a dispatch are explicitly
labelled as copies rather than attributed to the shader's stores. Absent context
in older captures is unknown. GPU tick completion still does not prove every
byte has a matching writer or that transfer-queue copies have finished.

After a clean build, capture a short fixed scene with `tracew`, filter with the
updated `filter_gpu_trace.py producers`, and run `sync_consumer_report.py` using
the capture's actual printed TSC frequency. Resolve the command's live shader
bytes/hash before considering an exact equivalent copy or early readback. Never
remove a required wait based only on the small byte count or a neighbour's tick.

Status: source inspection and whitespace check only; unbuilt while M20 remains
open. No runtime gain or producer identity is claimed yet. Rollback is this
isolated source commit plus the helper's event-40 decoding; old captures remain
readable without context.

## Consumed linear-copy capture operands

The live M20 one-frame capture omitted operands for all 221 consumed linear
copies. A fresh trace still identifies the same consumer byte and current-tick
608-byte overlapping declaration; there is no evidence that the address moved.
The missing capture match is unknown, not absence of a GPU producer.

`TryLinearCopy` now appends the validated source and destination byte intervals
and the eight-byte control read to the active frame capture, after the actual
copy and feedback scheduling. Only the destination is marked written. Rejected
kernels append nothing. This does not read additional guest data or change copy
eligibility, barriers, ownership, or feedback. The capture remains a diagnostic
that may perturb timings; do not count its frame as an optimization benchmark.

After the next clean build, capture one frame in the matching scene and run
`frame_readback_producer.py` with the address from a fresh `tracew` capture. Match
ordering and ticks before proposing any early mirror/readback. Source inspected
and whitespace checked only; build and in-game verification pending.
