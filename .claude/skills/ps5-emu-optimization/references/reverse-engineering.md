# Reverse-engineering the guest game

## Address model

- In Kyty the main module (eboot) is mapped at `0x900000000`; its first PT_LOAD has vaddr 0, so a
  guest address is `0x900000000 + va`. For a decrypted eboot file, `va = file offset - 0x4000` (check
  the program headers of your dump). Guest code is identity-mapped inside the emulator process, so the
  emulator's own debugger/memory reader sees it at the same addresses.
- Host code: link the emulator at a fixed base (Windows: `/DYNAMICBASE:NO`, base `0x140000000`) so
  runtime address = map RVA + base. Classify sampled addresses as guest (`0x800000000..0x1000000000`)
  or host by range.
- Decrypted modules may exist only in memory: read them from the running process (Linux
  `/proc/PID/mem`, Windows ReadProcessMemory + VirtualQueryEx) rather than from disk.

## Disassembly workflow

- Dump bytes from the live process and disassemble with objdump/llvm-objdump using `--adjust-vma` so
  addresses match: e.g. `guest-disasm.py 0x9002dd700 --size 0x100 --mark 0x9002dd73b`.
- Useful modes: `--find <hexbytes>` in a range (optionally data regions), `--dump` 64-bit words,
  `--regions` (executable regions), `--xref <addr>` (scan for rip-relative disp32 that resolve to the
  target; try instruction tails of +4/+5/+8).
- Sample guest threads with pc + callers (the word at rsp is the likely caller: guest code has no
  unwind info) and bucket by 64-byte granule to find hot guest functions.
- Time-HLE census: record the guest return address of every sleep/wait/vblank/equeue/TSC call (the
  call goes through the PLT, so the return address is the real call site). This finds spin loops and
  frame-pacing logic quickly.
- Engine conventions found this way in one title: convars registered with a lea-object/lea-name
  pattern (disable layers such as particles or XPR rendering via a command-line args file the game
  reads), a boot-flow state machine with a `table + state*0x10` handler table and state at `[obj+0xc]`,
  and an FNV-1a-style uppercase-folded name hash. Recover them with small scripts (string xrefs,
  pattern scans) and keep the scripts in the repo.

## Shaders

- Bundles (`*.csdr` in the source title): footer `RDSC` + payload length; header with a count;
  20-byte entries (stage, flags, code bytes, header bytes, relative); 0xff padding; an AGC header
  (starts `'1234'`) after each code block. Some shaders are embedded in the executable's data
  segment as an AGC header array with code 256-byte aligned, anchored by relocations.
- AGC header (96 bytes): self-relative 64-bit pointers, version, header size at 64, shader size at 68,
  type at 90. Port the console's static-input-info logic (resource usage from register lists) to
  compute the same compile keys the emulator computes at run time.
- Materials (`.cmat`, footer `STAM`) name the bundle and the techniques (VS/PS pairs, CS), which gives
  the VS/PS pairing; render-pass states (targets, blend, depth) are learned from a recorded run.
- The resulting seed file drives offline compilation (shard across processes on NVIDIA). Validate by
  comparing the modules a real run compiles against the precompiled set (coverage by stage).

## Missing or floating geometry

An object that is missing or floats while the rest of the frame is right is usually per-draw data
read at the wrong index, not a broken shader. Before bisecting draws, check what the CP writes by
itself for each draw packet. In Demon's Souls (a brazier at Boletaria 1-1 floating without its
stone pillar) the cause was in the original emulator:
- PM4 indirect draws (DRAW_INDIRECT, DRAW_INDEX_INDIRECT, the _MULTI forms) name SH registers the CP
  fills from the draw's arguments: START_INST_LOC (here SPI_SHADER_USER_DATA_GS_2) gets the start
  instance, BASE_VTX_LOC the base vertex. Both fields were ignored (the CPU path logged "partial
  args"), so shaders indexing per-object data with that SGPR read a stale user-data value.
- The instance ID VGPR counts from 0 on the GPU; Vulkan's gl_InstanceIndex includes firstInstance.
  Shaders adding the SGPR to the ID worked by accident; shaders using the SGPR alone broke.
- Fix: instance ID = gl_InstanceIndex - gl_BaseInstance; the named SGPR = gl_BaseInstance (shaders
  are translated per start-instance SGPR); firstInstance = the arguments' start instance, which also
  covers arguments that stay on the GPU. 95% of the game's indexed indirect draws start above 0.
- Proof before the fix: an experiment writing the SGPR on the CPU path (firstInstance 0) restored
  the pillar exactly as on the console; skipping draws, occlusion queries and culling switches had
  only ruled things out.

## Game patches

- Format used: etaHEN-style JSON (`id`, `version`, `process`, `mods[].memory[] {offset, off, on}`).
  Validate title id, app version and process name; find the module base by searching the first
  non-zero `off` pattern in loadable segments; verify every `off` sequence before writing; allocate a
  code cave for writes outside the module; flush the instruction cache; apply when the module loads.
- Emulator-side guest hooks (instead of patches): verify each audited function by RVA, size and hash
  before redirecting call sites; fall back to the original behaviour if any differs.
- Examples that paid: skipping boot logos/character creation for benchmarking, removing
  over-conservative compute barriers, a faster memmove, sleeping instead of spinning. Always keep
  them title+version-exact.

## Crash triage

1. Crash report must include: faulting thread name, guest registers, code bytes around pc, stack
   words, the host frames (Windows: RtlLookupFunctionEntry/RtlVirtualUnwind from the fault context;
   the walk stops at guest code) and the fault page's state/protection (reserved vs committed tells
   "unmapped since" bugs apart).
2. Symbolize host frames with the linker map of that exact build.
3. Classify: host access violation on guest memory (stale range after unmap, missing protection),
   guest fault on a red-zone access (Windows exception dispatch), lock/queue deadlock (dump all thread
   stacks of the hung process), driver crash (reduce to the shader/pipeline).
4. Reproduce with the save + route that triggered it before claiming a fix.
