# Synchronous readback page window experiment

Default remains the original 512 KiB speculative window. Enable
`KYTY_SYNC_READBACK_NARROW=1` to enumerate only the full tracker pages covering
the request, clipped to the live buffer owner. This differs from the existing
`KYTY_READBACK_NARROW` switch, which narrows asynchronous guest requests only
when their wider window overlaps a GPU-written image.

## Evidence and limits

M19 (12b52f94) spends roughly 16–18 ms each frame in a synchronous SRT download.
A route trace shows eleven 600-byte ranges whose writers completed earlier,
and one 1216-byte range written in the current copy tick. All twelve share a
single widened window and a final graphics-queue wait. This proves a late writer
exists. It does NOT prove it lies outside the actual request's required pages.
The existing trace recorded copies, not the request/consumer, so new tracew
events 37–39 record request, SRT consumer and enumerated window.

Narrowing can help only if speculative neighbouring pages caused the wait.
It cannot bypass the actual request's unfinished writer. More small downloads
can also make it slower.

M20 be20169a built this path and tested three paired 12-second standing windows.
FPS pairs (wide/narrow): 20.17/20.17, 20.83/20.92, 21.50/21.75. This is too small
to establish a useful gain; the flag was restored to 0. Instrumented captures
show a one-byte fault request at 0x20ed7fda0, shader hash 7fd4cfcc774889a2. This
one byte is the fault-handler request, not proof of the shader's total read size.
The last 608-byte write overlaps that address in the current tick and completes
about 16 ms later. Narrowing from the clipped 16 KiB owner to its 4 KiB page
reduces copied bytes from 7,816 to 3,864 per sampled request without removing the
actual producer dependency. The original captures and consumer reports live in
Performance Experiments/M20-be20169a-correctness. No bypass is justified.

New regression --sync-readback-narrow-only covers the synchronous GPU-thread
path, a crossing request, dirty bytes outside that request on both required
pages, the excluded dirty page, and the actual page-aligned owner end. Buffer
owners are caching-page aligned; no partial-owner-page coverage is claimed.
The regression is source-reviewed but not yet built or run. It preserves and
restores the two live switches it changes.

## Correctness constraints

Every exact dirty byte on every required tracker page is copied before those
pages become CPU-owned. Do not narrow to just a dword while returning an entire
page to the CPU. Source ownership, pending-ticket retirement, download alignment,
write-back protection, transfer-queue producer checks and priority completion are
unchanged. With scoped draining enabled, the complete selected window retires
before enumeration. Default global retirement is retained otherwise.

## Demon's Souls-only acceptance

After the current game exits, build a clean stamped candidate. Same save/cache,
camera and quality: alternate flag 0/1 for three pairs, then reference queue mode
3 during attacks and the short route. Match events 37–39 to copied ranges and
writer ticks; refuse a gain claim unless the required byte/race checks pass.
Record download count, exclusive wait time, p95/p99/max, uploads and RAM/VRAM.
Repeat with normal warm gameplay and area streaming. Roll back by unsetting the
flag, writing the resolved switch to 0, or reverting this isolated commit.
