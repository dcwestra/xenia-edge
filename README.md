<p align="center">
    <img height="160px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" />
</p>

<h1 align="center">Xenia Edge — edge-linux fork</h1>

<p align="center">
An unofficial fork of <a href="https://github.com/has207/xenia-edge">Xenia Edge</a> focused on Linux, AMD/Batocera support, <strong>Kinect</strong>, rendering accuracy and performance — <strong>developed with an AI coding assistant</strong>.
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
- **Testing covered a small set.** Five games: *Fable II*, *Fable III*, *Red Dead Redemption*, and the Kinect games *Fable: The Journey* and *Kinect Sports*. Two machines:
  - Fedora 44 on an NVIDIA Quadro T2000 laptop, with a Kinect for Xbox 360 (model 1414).
  - Batocera 43 on an AMD Radeon RX 5700 with an Intel i7-9700.

  Other games and hardware may behave differently, for better or worse.
- **Upstream's maintainer has asked not to receive AI-generated pull requests** (see [AGENTS.md](AGENTS.md)). This fork respects that and keeps its changes here.

## Kinect for Xbox 360

**This fork plays Kinect games with a real Kinect for Xbox 360 sensor on Linux.** Upstream Xenia has no Kinect camera support, so these games can't be played there.

Tested by the owner with a Kinect for Xbox 360 (model 1414) on Fedora 44:

- ***Fable: The Journey*** is playable. Getting the game's attention, Kinect ID sign-in and saving, horse brushing, reins and steering all work.
- ***Kinect Sports*** is playable. The hand-cursor menus, signing in, bowling and the stadium crowd area all work, and **players appear as their Xbox avatars**, fully dressed and with animated faces.

### How it works

Kinect games do their own skeletal tracking and Kinect ID face matching, in code shipped on the disc. The console only supplies the camera feed and a few system services, and that is what this fork adds:

- **The camera.** Depth and color at 640x480 and 30 fps, read straight from the sensor over USB (libusb), and delivered to the game the way the console does.
- **The tilt motor, LED and accelerometer.** Games can aim the sensor at the floor and the player, as on a console.
- **Kinect ID.** The identity data games store, and the links between Kinect IDs and profiles.
- **The Kinect system dialogs.** The pause message and the sign-in prompt, with a hand cursor to press their buttons.
- **Xbox avatars.** Avatars are built from the system avatar asset pack, the same data the console uses. Kinect Sports carries the pack on its disc, and discs with a Kinect system update (such as *Fable: The Journey*) carry a newer one.
  - Bodies, heads with animated eyes, eyebrows and mouths, nose, chin and ear shapes, hair, clothes, shoes, accessories and the avatar's colors are all decoded from it, with the body hidden under clothes as on the console.
  - Games can dress players in their own outfits: *Kinect Sports* puts its sports kits on them.
  - The first game with the pack on its disc installs it, so later avatar games have it too.
  - A profile without an avatar gets one generated from its profile ID, saved to the profile.
- **An avatar editor.** **Profile → (your profile) → Edit avatar…** edits a signed-in profile's avatar with a live preview, and saves it to the profile, so every game shows the same avatar.

### Setting it up

1. Plug the Kinect in with its power and USB adapter. `libusb-1.0` must be installed; most distributions have it.
2. Give your user access to the sensor with the udev rule in [`tools/kinect/`](tools/kinect/). The file explains how to install it. Then replug the Kinect.
3. Copy the game configs from [`game-configs/`](game-configs/) (`4D530A1A` for *Fable: The Journey*, `4D5308C9` for *Kinect Sports*) into your Xenia `config/` folder. They turn on `allow_nui_initialization`, plus `allow_avatar_initialization` for *Kinect Sports*.
4. Stand about 2-3 m in front of the sensor. *Kinect Sports* doesn't track seated players.

Related settings:

- `kinect_tilt` (on): lets games move the tilt motor.
- `kinect_record_path` and `kinect_playback_path`: record a session to a file and play it back later without a sensor. Recording takes about 22 MB/s.
- `log_kinect`: logs the camera and Kinect service traffic.
- `avatar_asset_pack_path`: the system avatar package (`FFFE07DF00000002` from a disc's `$SystemUpdate` folder, or *Kinect Sports'* `AvatarAssetPack`), for avatar games whose disc doesn't carry one.

### Limits

- **Linux only**, and only the original **Kinect for Xbox 360 (model 1414)**. The Xbox One Kinect isn't supported.
- **Two games tested.** Other Kinect games may reach parts that aren't implemented yet.
- **No Kinect audio.** The microphone array and voice commands aren't implemented.
- **Avatars are still missing some details.** Height and weight aren't applied yet. The editor only offers the items in the avatar pack, not marketplace or award items.
- **Not yet tried with Kinect on Batocera.**

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
- [`game-configs/`](game-configs/) holds recommended per-game settings for *Fable II*, *Fable: The Journey* and *Kinect Sports*.

## Known issues

- On NVIDIA, *Fable II* can still show an occasional single-frame white or blue flash on an object. `occlusion_query = "strict"` (in the recommended config) reduces it. It has not been seen on AMD with the game's own texture morphing, but testing there is still short.
- AMD/RADV can't map the emulator's guest memory directly (`VK_EXT_external_memory_host` rejects shared memory). Readback therefore goes through an extra copy, which is safe but slower than on NVIDIA.
- These changes were tested on five games only. Everything else is as upstream left it, for better or worse.

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
