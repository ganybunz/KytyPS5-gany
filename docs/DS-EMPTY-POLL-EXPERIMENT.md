# USA 01.004 empty-poll backoff experiment

**Default off. Built in be20169a; enabled path unmeasured.** Set `KYTY_DS_EMPTY_POLL_BACKOFF=1`
before a new process to install it. A lower CPU percentage alone is not an FPS
win. No modification to the current game's memory or installed game assets.

## Evidence

M19 12b52f94, Ryzen5 5500/RX6650XT, USA PPSA01342 01.004: one guest fiber thread
uses approximately94% of one core. A5s/20,000-sample profile finds27.68% at
eboot+863a22 (RDTSC result assembly),27.32% at86eb3e immediately following PAUSE,
plus surrounding empty-work queue/deadline polling. Original worker idle patch
is installed at86e963; this is a distinct scheduler function86ea30..86ecc2.

Full function disassembly is in M19/scheduler-full-disasm.txt. Byte audit:
658bytes, FNV1a64 c85cde9513ef7e84, SHA256
aa5a51d9f2d948d6727629e0f093f3ab40d7a9aec38575001afb2314117abe1f.
Both entries to86eb39 have tested r15b bit0 clear; positive queue/ready paths
bypass the site. The next instructions read a deadline and call the clock.
This supports a short empty-poll backoff experiment, not a claim that the busy
thread is the whole frame bottleneck. GPU work is already roughly32ms/frame.

## Bounds and preservation

Install only on exact title/version, OS-enabled AVX, whole-function hash plus
74exact nearby bytes. Fail closed on differences. Preserve original flags,
volatile integer registers, x87/MXCSR and all16YMM registers around the explicit
SysV callback. FXSAVE alone would lose upper YMM halves if a host callback used
VZEROUPPER. Original nonvolatile R13 assignment is reproduced after restoration.
Stack alignment follows the audited prologue; spills stay below the guest red
zone. The new CALL uses the same return slot as the immediately following
existing timestamp CALL; intervening original instructions do not read it.
Audited function has no branch into the remaining four overwritten bytes.
`empty-poll-guard-audit.json` independently compares the source's74bytes and
five replaced bytes with the captured owned function and recomputes its658byte
FNV/SHA256. All match. This static byte check does not verify emitted thunk
instructions, compilation, shutdown behavior or gameplay timing.

The callback requests5microseconds of nonspinning sleep at the existing PAUSE.
OS scheduling can overshoot that duration and delay newly arriving work.
Ready/exit tests, job execution, time values, thresholds, locks and fences are
retained. This is an explicit timing change, not a proven equivalent rewrite.
It does not alter global RDTSC, skip jobs or simulate completed work.

Uses a separate16KiB code lease at eboot+8004000, after the existing idle lease
and before the GPU-pages lease8080000. Install before guest initializers/threads.
After installation this is a process-lifetime lease: do not restore/free it
while a guest thread could execute it. The existing normal-exit path
`KytyClose -> RuntimeLinker::Clear` has no guest quiescence protocol. Both guest
exit wrappers therefore use the existing window-close `quick_exit` path only
when this patch is installed, preserving their requested exit status and running
the registered emergency flush. This changes normal-exit cleanup only for this
explicit experiment. `Clear` rejects an installed process-lifetime patch before
any bytes, mappings or other patches change. The OS releases the16KiB lease at
process exit. Future in-process unload/reset support requires guest quiescence;
this candidate does not permit live removal/reset. Rollback is a new process
with the flag unset, not instruction restoration in a running guest. The module
unload handler also refuses the exact owning program with `KERNEL_ERROR_EBUSY`
before calling its stop function; other modules retain their existing path.
`UnloadProgram` has a defensive owner guard for internal callers. Luna caught
this separate unload route during the lifecycle review; full `Clear` alone
would not have protected it. These changes need compiled and gameplay tests.

## Demon's Souls acceptance and rollback

After the current game exits: clean single-worker build, same warm cache/save,
camera and quality. Separate boots flag unset vs1; first verify installed guard,
gameplay progression, attacks and route twice. Measure busiest guest core, GPU
feeding gaps, frame times, synchronous download counts and game pacing. Repeat
area streaming and10minutes per run. Reject on FPS/tail regression, oversleep,
job/fiber crash, stalls or animation/audio timing changes. Test at least two runs
each; lowering CPU while FPS worsens fails acceptance. Roll back by unsetting
the flag on the next launch or reverting this isolated change. Do not promote
before these checks; no memory, CPU or FPS saving has been established yet.
