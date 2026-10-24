<p align="center">
    <img height="160px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" />
</p>

<h1 align="center">Xenia Edge — edge-linux fork</h1>

<p align="center">
An unofficial fork of <a href="https://github.com/has207/xenia-edge">Xenia Edge</a> focused on Linux, AMD/Batocera support, rendering accuracy and performance — <strong>developed with an AI coding assistant</strong>.
</p>

> [!IMPORTANT]
> **This is not the official Xenia Edge.** It is a personal fork, not affiliated with or endorsed by Xenia, Xenia Canary or Xenia Edge.
> Please **do not report problems with this fork to the upstream projects**, and do not send its changes upstream on its behalf — use this repository's issue tracker instead.
> For the official emulator, see [has207/xenia-edge](https://github.com/has207/xenia-edge). Its original README is kept here as [README.upstream.md](README.upstream.md).

## How this fork was made: AI disclosure

**The code changes in this fork were written by an AI coding assistant**, Anthropic's Claude (Claude Code), working in this repository at the direction of the repository owner ([@dcwestra](https://github.com/dcwestra)).

How the work was done:

- **The owner set the goals and tested every change on their own hardware.** They played the games, reported what they saw, captured screenshots and logs, and decided what to keep.
- **The AI did the engineering.** It read the emulator's code, proposed and wrote the changes, built them, ran the unit tests and investigated problems. That included profiling, Vulkan validation, reading guest memory and disassembling game code, and analyzing crash dumps.
- **No Xenia developer has reviewed these changes.** The owner directs and tests the work but is not an emulator developer.

What that means for you:

- **Expect mistakes.** The changes compile, pass the existing unit tests and fixed the problems they were aimed at in the games that were tested. They have not had the scrutiny upstream changes get.
- **Commits are marked.** Every commit made with the AI carries a `Co-Authored-By: Claude` trailer, so you can always tell what it touched.
- **Testing covered a small set.** Three games: *Fable II*, *Fable III* and *Red Dead Redemption*. Two machines:
  - Fedora 44 on an NVIDIA Quadro T2000 laptop.
  - Batocera 43 on an AMD Radeon RX 5700 with an Intel i7-9700.

  Other games and hardware may behave differently, for better or worse.
- **Upstream's maintainer has asked not to receive AI-generated pull requests** (see [AGENTS.md](AGENTS.md)). This fork respects that and keeps its changes here.

## Key changes

Unless noted, every new behavior is controlled by a configuration option (cvar), so it can be turned off if it causes trouble.

### Rendering accuracy

- **Texture fetch offsets at the sampled mip level** (`texture_fetch_offsets_at_lod`, on). Xenia scaled texel offsets by the base texture size even when sampling a smaller mip; upstream already had a FIXME about this.
  - In *Fable III*, the depth-pyramid reducer then copied one corner texel per level instead of a conservative minimum. The game's GPU culling hid castle walls and objects behind trees, so they flickered in and out.
  - Fixed at the root, with no game-specific workaround. Correct culling draws more, so busy areas can run a little slower than before.
- **Per-shader depth bias** (`shader_depth_bias`). This is for content that relies on exact ties in the 360's 24-bit depth. *Fable II*'s dog fur shells flicker without it; a recommended config is in [`game-configs/`](game-configs/).
- **Render-to-texture readback can no longer hang or crash when the GPU can't map guest memory directly.** On AMD/RADV, GPU output read back by the CPU was copied into memory the game had just protected. The copy faulted while holding the emulator's global lock and hung the game on a loading screen.
- **A clear warning when Vulkan falls back to a software renderer** (for example llvmpipe, after a driver update without a reboot), instead of a silent black screen. Hardware GPUs are now always preferred.
- **A warning when the Vulkan pipeline cache was made by another driver version.** After a driver update, pipelines are rebuilt during play, which shows up as brief flashing or missing objects.

### Performance

- **Upload hoisting** (`vulkan_hoist_shared_memory_uploads`, on). CPU-to-GPU memory uploads that nothing earlier in a submission used are moved to its start, instead of ending the render pass for each one.
  - *Fable II* gameplay went from **28.4 to 42.6 fps** on the T2000 in the owner's A/B test.
- **Skipping unused stencil-bit passes** (`vulkan_skip_unused_transfer_stencil_bits`, on). This applies on GPUs without `VK_EXT_shader_stencil_export`, such as NVIDIA.
  - Copies into depth targets need 8 extra draws for the stencil bits. A small compute pass now finds which bits are actually set, and conditional rendering skips the draws for bits that aren't.
  - This saves about 0.6 ms per frame in *Fable II*.
- **A lock-free guest clock.** Upstream's guest clock dropped the fractional tick on every read, so games that poll the timebase saw guest time run slow: **7.8% slow** in the new `clock_test`. Reading the clock also took a lock on a hot path.
- **Scheduling fixes:** sub-millisecond guest waits and absolute timeouts are handled correctly, multi-object waits use a futex, and spinning guest threads re-check their timeouts without extra system calls.

### Linux and Batocera

- **Runs on Batocera 43 and other distributions with an older glibc and C++ runtime.** The `XENIA_PORTABLE_LINUX` build option links the C++ runtime statically, binds the few glibc 2.43-only symbols to older versions and drops the PCRE2 dependency.
- **Batocera integration** ([`tools/batocera/`](tools/batocera/)). It adds **xenia-edge** as a third Xbox 360 emulator choice in EmulationStation, next to Batocera's Wine-based Xenia builds.
  - It goes through Batocera's own launcher, so controllers, the hotkey exit, power and video modes all keep working.
  - Everything is installed under `/userdata`, so it survives Batocera updates, and saves are shared with the Wine builds.
- **X11-only GTK builds work.** Batocera's GTK has no Wayland backend, and the window code called a Wayland function on startup.
- **The picture no longer freezes while the game keeps running.** Repaint requests from the GPU thread could race with the UI thread whenever an overlay, such as an achievement pop-up, was on screen, and get lost for good.
- **Starting in fullscreen hides the menu bar** on Linux.
- **Reliability fixes:**
  - SDL no longer swallows SIGTERM/SIGINT.
  - Crashes write a backtrace to the log.
  - The pipeline cache is saved atomically, per GPU, every minute.
  - GPU staging buffers are freed without draining the GPU.
  - A warning appears when `vm.max_map_count` is too low for demanding games; the Batocera launcher raises it automatically.
  - The in-game menu no longer renders its text as black boxes when the only CJK font installed is one the UI can't load.

### Diagnostics for investigating problems

- **`log_fps`:** every few seconds, logs the frame rate, the longest frame (split into waiting for the guest, guest waits and GPU waits), GPU time by kind of work, render target copy statistics, readback statistics and how many frames are queued.
- **Shift+F12:** saves a burst of 90 frames as PNGs, with a per-draw log for each frame (shaders, texture bindings, resolves, CPU readbacks).
- **Cvars for deeper investigation:** `log_function_disassembly` (logs the PPC, IR and x64 of one guest function), `perf_map` (lets `perf` name JIT code), `log_shared_memory_range` and `shader_texture_fetch_zero`.
- **Shader cache merging:** [`tools/merge_shader_storage.py`](tools/merge_shader_storage.py) merges one machine's shader and pipeline storage into another's, so a new machine can start with pipelines already known.

### Game-specific files

These are separate from the code, and every patch is **off by default**. Copy what you want into your Xenia storage folder.

- [`game-patches/`](game-patches/) holds patches for *Fable II* and *Fable III*. They sit alongside the community [game-patches](https://github.com/xenia-canary/game-patches) files, because Xenia merges every patch file for a game.
  - A working *Fable II* Collector's Edition unlock. The community version crashes this game version when the Guild Cave loads.
  - A *Fable III* Limited Collector's Edition unlock. It is new, and its in-game result is not yet confirmed.
  - The *Fable II* Realtime Texture Morphing patch, from himdo's research. In this fork it is only needed if `readback_resolve` is off.
- [`game-configs/`](game-configs/) holds recommended per-game settings for *Fable II*.

## Known issues

- On NVIDIA, *Fable II* can still show an occasional single-frame white or blue flash on an object. `occlusion_query = "strict"` (in the recommended config) reduces it. It has not been seen on AMD with the game's own texture morphing, but testing there is still short.
- AMD/RADV can't map the emulator's guest memory directly (`VK_EXT_external_memory_host` rejects shared memory). Readback therefore goes through an extra copy, which is safe but slower than on NVIDIA.
- These changes were tested on three games only. Everything else is as upstream left it, for better or worse.

## Building

Linux needs clang. See [docs/building.md](docs/building.md) for dependencies.

```sh
./xb setup
./xb build --config=release
```

### Building for Batocera (or another older distribution)

1. Configure a separate build tree with the portable option, generate its `version.h`, and build:

    ```sh
    cmake -S . -B build-batocera -G "Ninja Multi-Config" \
          -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
          -DXENIA_PORTABLE_LINUX=ON
    python3 -c "import importlib.util as u; s = u.spec_from_file_location('xb', 'xenia-build.py'); m = u.module_from_spec(s); s.loader.exec_module(m); m.generate_version_h('build-batocera')"
    cmake --build build-batocera --config Release --target xenia-app
    ```

    If your toolchain has no static `libstdc++.a` (Fedora: package `libstdc++-static`), unpack one somewhere and pass `-DXENIA_PORTABLE_LINUX_STATIC_LIB_DIR=<directory>`.

2. Install it on a Batocera machine reachable over SSH:

    ```sh
    tools/batocera/deploy.sh <ssh host> [local Xenia storage folder to copy patches/ and config/ from]
    ```

    The script first checks that the binary will load there: glibc 2.40 at most, and a static C++ runtime. Files it would replace are kept as backups.

3. Restart EmulationStation, then choose **xenia-edge** as the emulator for a game or for the whole Xbox 360 system.

## Credits

- [Xenia](https://github.com/xenia-project/xenia), [Xenia Canary](https://github.com/xenia-canary/xenia-canary) and [Xenia Edge](https://github.com/has207/xenia-edge), whose developers built the emulator this fork changes.
- [himdo/Fable-2-Recomp](https://github.com/himdo/Fable-2-Recomp) (MIT), for the *Fable II* texture-morphing and dog-fur findings, and just-harry for the morphing diagnosis.
- [DanielCoffey1/Fable-III-PC](https://github.com/DanielCoffey1/Fable-III-PC), whose research pointed at the *Fable III* visibility shader. Only its findings were used, no code.
- The [game-patches](https://github.com/xenia-canary/game-patches) community.

## License

Same as upstream: BSD, see [LICENSE](LICENSE). Third-party components keep their own licenses.
