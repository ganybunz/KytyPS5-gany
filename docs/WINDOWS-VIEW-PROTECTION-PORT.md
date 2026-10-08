# Windows mapping protection port

Adapted upstream c30ac7c904ca9cd678a064b73c0b66b0b333ba99 (2026-10-08 local review).
MapViewOfFile3 already applies the requested protection for readable/writable/
executable views. Avoid the immediately redundant VirtualProtect. NoAccess still
maps readable then explicitly protects, as before. Tracker protection calls and
all fences, mapping publication, error handling, and view-remap notifications
remain intact. This affects mapping work, not every page-fault protection call.

The upstream test maps the same placeholder in four supported modes, queries
actual Windows protection and mapping type, then unmaps before the next mode.
Run virtual_memory_allocation_tests --backing-view-modes-only after a clean build.
Status: source port; build/test/game verification pending. No measured FPS gain.
Rollback: revert this commit. Primary source:
https://github.com/KytyPS5/KytyPS5/commit/c30ac7c904ca9cd678a064b73c0b66b0b333ba99
