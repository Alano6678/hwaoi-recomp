# Linux build

The game builds on x86-64 Linux from the same sources as the Windows build: `tools/build_game.py` compiles the
runtime's SDL3 files (`*_sdl.c`) and `runtime/platform/linux.c` in place of the Win32 ones.

## The release (players)

Age of Decomp's Linux AppImage (and the `.tar.xz` of the same folder) carries the whole compiler, as the Windows zip
does, so the player's PC needs nothing installed: LLVM's own clang and lld, the files of the Steam Runtime 3 "sniper"
SDK the build reads (glibc 2.31, GCC 14's libstdc++) in `llvm/sysroot`, Opus, libvpx, zstd and the Vulkan loader's link
library in `deps/`, SDL 3.4 built in the SDK's image with every optional part loaded at run time (`libSDL3.so.0` goes
beside the game, rpath `$ORIGIN`), Ninja, and Python (python-build-standalone). The game it builds needs glibc 2.31,
the PC's `libvulkan.so.1` and a Vulkan 1.3 driver; the AppImage's programs need glibc 2.34 (SteamOS 3, Ubuntu 22.04,
Debian 12, Fedora 36 or newer). `tools/engine.py toolchain` (repository root) makes that toolchain; the model is DKB
Recomp's `tools/make_release_linux.py`.

The setup step and Play run without a window (`AppRun cli prepare …`, `AppRun --play …` start aod-cli), which is what
RecompLauncher uses, on a Steam Deck too; only the launcher's own window needs WebKitGTK 4.1 from the system.

- Linux 6.7 or later is faster (userfaultfd write protection with `PAGEMAP_SCAN`); older kernels work.
- A Vulkan 1.3 driver with `VK_EXT_external_memory_host` (RADV, ANV, NVIDIA).

## A developer build

From the repository root (`python3`, 3.12 or newer):

```sh
python3 tools/engine.py toolchain    # LLVM, the sniper sysroot, SDL3 (docker or podman), deps, Ninja, Python
python3 tools/engine.py translator   # the shader translator (system .NET 10 SDK and git)
python3 tools/engine.py stage        # out/engine/linux
python3 tools/engine.py game --nup <your .nup>   # work/game-linux
```

The steps keep everything under ignored folders. Saves go to `work/saves/hwaoi196608` of the game folder, each run's
log to its `logs/`.

## Notes

- The script defaults to XWayland (`SDL_VIDEODRIVER=x11`). On NVIDIA with native Wayland, changing the window size or
  resolution from the menu ended the game with `wp_linux_drm_syncobj_surface_v1: error 3: Missing buffer`.
- libopus 1.5 and later (what distributions ship) has a much larger decoder state than the classic one. The hwopus
  service reserves at most the classic size for the game (`runtime/hwopus.c`); without that the first sound bank fails
  with "can't create resource".
- Audio buffers go back to the game while SDL still holds two device periods (`runtime/audout.c`); releasing them only
  as the device consumed them left less queued than one PipeWire period, which crackled.
- `HWAOI_AUDIO=none` runs without an audio device; `HWAOI_TRACE=audio` logs the queue depth once a second.
