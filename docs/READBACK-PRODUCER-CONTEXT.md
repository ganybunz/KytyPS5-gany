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
