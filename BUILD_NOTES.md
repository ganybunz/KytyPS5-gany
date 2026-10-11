# KytyPS5 exFAT / ZArchive Port — Handoff Notes

## Goal

Add support for loading raw exFAT volume images and ZArchive-compressed game data, including launcher integration and filesystem access. The next development goal is FPKG support.

## Current branch

- Branch: `exfat-zarchive-port`
- Base branch: `ds-chenxiao-sync`
- Remote: `origin` (existing GitHub repository)

## Implementation overview

- `src/common/exfat.cpp` and `exfat.h`: exFAT volume handling.
- `src/common/archive.cpp` and `archive.h`: archive access.
- `src/common/file.cpp` and platform file I/O implementations: file access changes.
- `src/kernel/fileSystem.cpp`: filesystem integration.
- `src/launcher/`: game-content detection and launcher integration.
- `3rdparty/CMakeLists.txt`: pinned ZArchive and zstd dependencies; builds the ZArchive library without its CLI.
- `3rdparty/patches/zarchive-reader.patch`: patch applied to the ZArchive source.
- `tests/`: archive, exFAT, AV-player and filesystem tests.

Review the code and tests before assuming every intended runtime path is complete.

## Build environment used

- Windows 11 IoT Enterprise LTSC
- Visual Studio Build Tools / MSVC toolchain
- Qt 6.12.0, `msvc2022_64`
- AMD ROCm Clang 7.2 `clang-cl.exe`
- CMake and Ninja from Visual Studio Build Tools

Build directory: `_Build/windows-exfat`

Built targets included `launcher` and `kyty_emulator.exe`. The install staging directory was `_Build/windows-exfat/shareable`.

## Compatibility changes required for the successful local build

1. SDL2: checked out submodule commit `4b69833bc54abf3dd3288d4aa7afbba527775e5b`, where the expected SDL2 header `include/SDL.h` exists.
2. `src/graphics/shader/recompiler/ir/Value.cpp`: replaced a `std::ranges::find` use-validation expression with an explicit loop checking `use.user == this && use.operand == operand`, working around an AMD Clang / Visual Studio STL compatibility issue.
3. LibAtrac9: the checked-out library exposes decode APIs requiring an input-size argument. `src/libs/ajm/atrac9_decoder.h` was updated to pass `m_superframe_bytes_remain` as the input size and `0` for the final `nointerleave` argument. The LibAtrac9 submodule pointer was moved to the checked-out commit `946e05a9212976626a9f5e52f29c0a7202871f29`, because the original commit recorded by the base branch was not present locally.

These are compatibility changes, not exFAT features. Verify the dependency/API relationship before changing or reverting them. Successful compilation does not by itself prove ATRAC9 decoding correctness.

## Current local build artifacts

- Build log: `exfat-build.log` (kept outside the commit)
- Emulator: `_Build/windows-exfat/kyty_emulator.exe`
- Launcher: `_Build/windows-exfat/launcher/launcher.exe`
- Install staging: `_Build/windows-exfat/shareable`

The build directory is ignored by Git. Do not commit build artifacts or the local build log by default.

## Launcher configuration note

`C:\ProgramData\Kyty\Kyty.ini` had a broad `game_dirs` entry pointing to `D:/`, which caused an excessive scan and made the launcher appear to hang. The configuration was narrowed to specific Desktop paths. Backup: `C:\ProgramData\Kyty\Kyty.ini.backup-20261011`.

When testing, add only the specific folder containing the image. Avoid scanning an entire drive root.

## Verification status and remaining work

- A local Release build completed successfully.
- The launcher was made responsive again after narrowing its scan paths.
- End-to-end booting of a game directly from a raw exFAT image has not yet been confirmed.
- Test targets exist, but do not assume all tests have been executed or passed unless the logs confirm this.
- The current next feature goal is FPKG support. Investigate the existing package-loading paths first: detection, package parsing, decryption/key handling, mounting or extraction, virtual filesystem integration, and executable resolution. Do not assume FPKG support is only a launcher change.
- Preserve the current branch and its working changes while investigating. Avoid scripts that recursively update all submodules or reapply old patches without reviewing their effects.

## Original patch files

The files under `docs/original-patches/` are retained as historical references to the changes that led to this port. The current implementation already includes applied changes and subsequent local modifications. Do not reapply these patches to the current branch without first checking which hunks are already present.
