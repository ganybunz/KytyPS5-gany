# Portable graphical launcher

The Desktop package builds `launcher.exe` and `kyty_emulator.exe` from the same
source commit. Its `Kyty.ini` holds package-local game directories and GUI
settings. When this file exists beside the launcher, startup selects that
directory before reading settings, including when started from a shortcut.

The optional `launcher-environment.json` beside the emulator supplies runtime
settings for an individual title. The launcher inherits the normal system
environment on every launch and then applies only that title's profile. This
keeps the Demon's Souls performance switches from affecting other titles.

```json
{
  "version": 1,
  "titles": {
    "PPSA01342": {
      "KYTY_SHADER_WARMUP": "1",
      "KYTY_NATIVE_PIPELINE_WARMUP": "1"
    }
  }
}
```

Names must match `KYTY_[A-Z0-9_]+`; values must be strings without null bytes.
Malformed files or selected profiles produce an error before starting the game.
A missing file or a title without a profile uses the normal inherited settings.
Remove the optional profile file to reverse this package configuration.

The Desktop source copy includes this change, the local integration history, and
the dependency source snapshot. The original worktree and player directories
remain available. Packaging and launcher checks do not establish a new gameplay
performance result or resolve the existing texture ownership crash.
