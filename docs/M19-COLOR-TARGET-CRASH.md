# M19 color-target acquisition crash (2026-10-07)

Run M19 used12b52f94 and ended at10:08:35 local, approximately64minutes after
launch. Last physical free RAM543.8MiB and commit headroom11026MiB; watchdog
reported process exit, not an emergency memory termination. No new source code
had been applied to that process. The uncontrolled later play reached new
views/effects and synchronous compiles, including a2.56second frame, plus large
streaming uploads and a510.8ms frame with482.2ms GPU wait. It invalidates any
claim of hitch-free long gameplay based on the frozen short route alone.

Fatal: `TextureCache: color target requires rediscovery before final acquisition`.
Exact archived exe/PDB backtrace resolves to `FindRenderTarget ->
AcquireRenderTargets -> ExecutePreparedDraw -> DrawAuto -> CpOpDrawIndexAuto ->
CommandProcessor::ProcessPm4`. This is normal draw acquisition, not evidence that
the table-compute correction or new scheduler candidate failed: neither was
built or running. Draw table2 was active, so a contribution from its prior
resource preparation has not been ruled out.

Source audit found the color-target rediscovery precondition omitted `depth_id`,
which `FindRenderTarget` rejects along with unregistered owners and
`needs_rebind`. The candidate includes that missing case in existing rediscovery
before final acquisition. It retains `FindImage` ownership resolution and the
final consistency check; no draw or binding is silently omitted and no invalid
view is accepted. This mismatch is concrete, but the old fatal did not record
which condition was true. It is not yet proof that the run's crash is fixed.
If discovery still returns an unavailable owner, the retained fatal now records
ID/generation, registration, depth association, rebind flag and requested/owner
ranges/format. That evidence is required before any larger alias-lifetime change.

Not built or game-tested. Next candidate: same warm cache, table compute0,
all new timing/transfer experiments off, repeated Boletaria attacks/traversal
and a longer streaming transition. Then isolate table draws0/2 if this crash
returns. Preserve guards, every draw and GPU-written contents. Do not replace
failed ownership with a null image, remove the assertion, or label a single
successful load a permanent crash fix. Rollback is this isolated change.
