# Linux build

HWAoI Recomp builds on x86-64 Linux from the same sources as the Windows build: `tools/build_game.py` compiles the
runtime's SDL3 files (`*_sdl.c`) and `runtime/platform/linux.c` in place of the Win32 ones and links against the
system libraries. There is no Linux release or installer; this is a build from your own extracted game files.

## Requirements

- Linux 6.7 or later (userfaultfd write protection with `PAGEMAP_SCAN`).
- A Vulkan 1.3 driver with `VK_EXT_external_memory_host` (RADV, NVIDIA).
- Packages, on Arch:

  ```sh
  pacman -S --needed clang lld ninja make sdl3 opus libvpx zstd zlib vulkan-headers vulkan-icd-loader \
      shaderc spirv-tools dotnet-sdk python git
  ```

- Extracted game files of the supported version (ExeFS and RomFS), by default in `work/game/exefs` and
  `work/game/romfs`. `tools/hwaoi.py` checks the module hashes before it generates anything.

## Build and run

```sh
python -m venv .venv && .venv/bin/pip install -r tools/requirements-hwaoi.txt
tools/linux/build.sh            # or: tools/linux/build.sh <exefs dir> <romfs dir>
tools/linux/play.sh
```

`build.sh` runs three steps and keeps everything under the ignored `work/` folder:

1. `tools/hwaoi.py generate --display-settings` writes the game's C to `work/generated/hwaoi196608-full`.
2. `tools/build_game.py` compiles and links `work/build/hwaoi196608/hwaoi`.
3. `tools/build_xlate_hwaoi.py` publishes the managed shader translator for `linux-x64` to
   `work/build/hwaoi196608/xlate-managed-irbanks`. The first run restores its .NET packages from nuget.org into
   `work/toolchain/NuGet` (`--online`); the comparison of `tools/vendor/ryujinx` with a pinned DKB clone is skipped
   (`--no-vendor-reference`, recorded in `build-report.json`).

`play.sh` starts the game with the settings `tools/hwaoi.py launch` uses on Windows. Saves go to
`work/saves/hwaoi196608`, each run's log to `work/logs/`. Variables set before the call win, for example
`HWAOI_FPS_LIMIT=60 SDL_VIDEODRIVER=wayland tools/linux/play.sh`.

## Notes

- The script defaults to XWayland (`SDL_VIDEODRIVER=x11`). On NVIDIA with native Wayland, changing the window size or
  resolution from the menu ended the game with `wp_linux_drm_syncobj_surface_v1: error 3: Missing buffer`.
- libopus 1.5 and later (what distributions ship) has a much larger decoder state than the classic one. The hwopus
  service reserves at most the classic size for the game (`runtime/hwopus.c`); without that the first sound bank fails
  with "can't create resource".
- Audio buffers go back to the game while SDL still holds two device periods (`runtime/audout.c`); releasing them only
  as the device consumed them left less queued than one PipeWire period, which crackled.
- `HWAOI_AUDIO=none` runs without an audio device; `HWAOI_TRACE=audio` logs the queue depth once a second.
