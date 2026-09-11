PS3Native
=========

A PlayStation 3 emulator for Android, built directly on top of current upstream [RPCS3](https://github.com/RPCS3/rpcs3).

This fork tracks the real RPCS3 tree rather than a snapshot. The archived [RPCS3-Android](https://github.com/RPCS3/rpcs3-android) port is folded in at [`android/`](android), rebased onto current upstream, and carried forward with additional fixes and interface work contributed by the [WinNative](https://github.com/WinNative-Emu) developers.

## What this is

Upstream RPCS3 targets desktop platforms. The Android port was published separately and then went stale against a moving upstream. The goal here is a single tree where:

* the emulator core is unmodified upstream RPCS3, so improvements land by merging upstream rather than by re-porting;
* the Android front end lives beside it in `android/` and builds an APK from that same source;
* Android-specific defects — threading, shutdown, configuration persistence, on-screen controls — are fixed in place;
* the interface is a Compose Material 3 front end sharing the design language of WinNative.

Everything below `android/` is additive. The upstream history is preserved intact.

## State

Working:

* boots and runs commercial titles at full speed on current Snapdragon hardware
* Vulkan renderer, PPU/SPU recompilers via a cross-compiled LLVM
* per-game configuration, backed by RPCS3's own custom-config mechanism
* generated PS2-style on-screen controls with sticky presses and reserved touch zones
* in-game menu with pause/resume, settings and shutdown
* game library with a built-in compatibility browser

Not finished:

* netplay has never been verified end to end; RPCN has no sign-in interface on Android
* an intermittent crash in the render queue is still being tracked
* audio backend selection is limited

## Building

The Android build needs LLVM and FFmpeg cross-compiled for `arm64-v8a` first. Both scripts fetch their own sources and write into `android/prebuilt/`:

```bash
cd android
./build-ffmpeg-android.sh
./build-llvm-android.sh
./gradlew assembleStandardDebug
```

Requirements: JDK 17, Android NDK 29, CMake 3.31, and a checkout with submodules (`git submodule update --init --recursive`).

The dependency scripts are the slow part and only need running when they change. `android/prebuilt/` is not tracked.

To build in CI, run the **Android APK** workflow manually from the Actions tab. It is dispatch-only, caches the cross-compiled dependencies against the build-script hashes, and shares a ccache across both the dependency and application builds, so a run that only touches app code reuses everything else.

## Flavors

The same APK is published under several package names. Some Android vendors gate their high-performance CPU and GPU governors on an allowlist of package names, so a build installed under one of those identifiers is scheduled more aggressively. Pick whichever performs best on your device.

| Flavor | Package name | Gradle task |
| --- | --- | --- |
| `standard` | `com.ps3native.standard` | `assembleStandardDebug` |
| `antutu` | `com.antutu.ABenchMark` | `assembleAntutuDebug` |
| `ludashi` | `com.ludashi.benchmark` | `assembleLudashiDebug` |
| `pubg` | `com.tencent.ig` | `assemblePubgDebug` |

All four are the same emulator and carry the same name and icon. Because a package name is unique on a device, a flavor cannot be installed alongside the real application that owns that identifier, and only `standard` is suitable for distribution through an app store.

## Frame generation

The Vulkan presenter can interpolate extra frames between the ones the RSX actually renders. It happens in the presenter, on the swapchain images, after the RSX has finished with a frame; the emulator core is not involved.

There are two engines. They are mutually exclusive — the presenter drives one interpolator per frame — and both are reached from **Frame Gen** in the library menu or the in-game menu's Frame Gen tab.

| Engine | Needs | What it is |
| --- | --- | --- |
| Lossless Scaling (LSFG) | Your own copy of `Lossless.dll` | The Lossless Scaling interpolation shaders, ported to Vulkan |
| DIS | Nothing — ships inside the APK | Dense Inverse Search optical flow, fully open source |

The master toggle, the multiplier (2×/3×/4×) and the target rate are shared and apply to whichever engine is selected. Only the quality knob is per-engine, because the two engines measure their cost differently.

### Lossless Scaling (LSFG)

**The shaders are not redistributed and nothing ships with the APK.** You point the app at your own copy of `Lossless.dll`; its PE resource tree is walked for the shader blobs, which are translated to SPIR-V once and cached in app storage. The DLL is parsed as data and never executed. Until that import succeeds, LSFG cannot be enabled.

Its quality knob is a percentage: the optical-flow pyramid is run at a fraction of the frame's resolution — 40% at Ultra Performance, 50% at Performance, 70% at Balanced, 100% at Quality. The setting is a ceiling rather than a target; when the game renders below the output resolution the pyramid is derated to that ratio instead, because searching above the resolution the frame actually carries tells the search nothing. Lower is cheaper, and softens fine detail during fast motion.

### DIS

**Nothing to buy and nothing to import.** DIS is a complete open-source Dense Inverse Search optical-flow frame generator, built into the APK as a chain of compute shaders, so it works on a fresh install with no `Lossless.dll` and nothing bought from anyone. Select it and turn frame generation on.

Its quality knob is stated in pixels rather than as a percentage: the **shorter side of the flow pyramid**, at Fast 180, Balanced 252 or Quality 360, with Balanced the default. A fixed pixel budget costs the same whatever resolution the game outputs, whereas the same percentage would cost four times as much at twice the resolution without telling the search anything more about the motion. Lower is cheaper; higher tracks small or fast-moving detail better.

### Both engines

Whichever engine is running, motion is estimated at the reduced resolution its quality knob sets, and the interpolated frame is produced at the swapchain extent — the size the frame is actually presented at.

Interpolation costs one extra frame of latency — holding the newer frame back is what makes interpolating between two of them possible. It buys smoothness, not response.

Generated frames occupy display refresh intervals, so the multiplier is sized against the panel's refresh rate and the game's actual frame rate. A game already running near the panel's refresh rate has nothing to gain.

## Credits

* the [RPCS3](https://github.com/RPCS3/rpcs3) team, for the emulator
* the [RPCS3-Android](https://github.com/RPCS3/rpcs3-android) port this build started from
* the [WinNative](https://github.com/WinNative-Emu) developers, for the interface work and Android fixes

### Frame generation

Neither interpolation chain — [`rpcs3/Emu/RSX/VK/lsfg/`](rpcs3/Emu/RSX/VK/lsfg) or [`rpcs3/Emu/RSX/VK/dis/`](rpcs3/Emu/RSX/VK/dis) — is original work. They reach this tree through:

* **[lsfg-vk](https://github.com/PancakeTAS/lsfg-vk)** by PancakeTAS and contributors (MIT) — the original Vulkan reimplementation of the Lossless Scaling frame generation chain, and the source of its structure: the mipmap pyramid, the alpha/beta/gamma/delta flow passes and the generation pass.
* **Camille LaVey**, in the **[Eden Emulator Project](https://git.eden-emu.dev/eden-emu/eden)** (GPL-3.0-or-later) — the port of that chain into an emulator's Vulkan renderer, landed as eden PR #4263, *[vulkan, android] Initial implementation of LSFG-VK*. This is the work that made the feature possible here: getting the Lossless Scaling compute chain running correctly on Vulkan on mobile GPUs is the hard part, and it was already solved. Every `lsfg_*.cpp` and `lsfg_*.hpp` in this tree descends from `src/video_core/renderer_vulkan/present/` in that port and keeps its SPDX headers. That includes the pacer: `lsfg_pacer` began as `frame_gen_pacer` and has since been rewritten around the panel refresh rate and the guest present rate, but the design is theirs. What is written for this project is the RPCS3 side — `VKFrameGeneration`, the presenter wiring in `VKPresent`, and the settings and menu surface around them.
* **[DXVK](https://github.com/doitsujin/dxvk)** — copyright Philip Rebohle, Joshua Ashton, Robin Kertels and Jeffrey Ellison, zlib/libpng licence. Its DXBC-to-SPIR-V shader translator is vendored at [`3rdparty/dxbc`](3rdparty/dxbc), taken from a standalone repackaging of those files rather than from the DXVK tree itself. Steam builds of Lossless Scaling ship the shaders as DXBC rather than SPIR-V, so they are translated once on import. This entry is the acknowledgment the zlib licence asks for.
* **[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)** by THS — the shaders themselves. `Lossless.dll` is proprietary and remains the property of THS; it is read from the user's own installed copy at runtime, and neither it nor any shader extracted from it is redistributed here.
* **[LSFG-Android](https://github.com/FrankBarretta/LSFG-Android)** by FrankBarretta — the first project to run the lsfg-vk pipeline on Android, and a reference while this port was written. It takes a different route, compositing over a `MediaProjection` capture in a system overlay rather than inside a renderer, so no code is shared with it. Its repository is not under a single licence: the root is MIT, `lsfg-vk-android/` is MIT inherited from lsfg-vk, and the `LSFG-Android/` application subtree is under a custom licence that forbids app-store publication and commercial use.
* **DIS optical flow frame generation** by **qwertypower** ([DEVAR Entertainment LLC](https://github.com/qwertypower)) (GPL-3.0) — a complete open-source implementation of a Dense Inverse Search frame generator: the pyramid, the patch search, the sparse-to-dense step, the variational refinement and the warp, written as Vulkan compute shaders. It is the second engine here and the one that needs nothing from anywhere else. See [DIS frame generation](#dis-frame-generation) below.
* **DIS optical flow** — the algorithm and its reference implementation come from [OpenCV](https://github.com/opencv/opencv) (`DISOpticalFlow`), which adopted Till Kroeger's original [OF_DIS](https://github.com/tikroeger/OF_DIS). The tuned constants in the shaders — the densify confidence epsilon, the refinement's delta, gamma and zeta — are OpenCV's, and are why the chain works on luminance scaled to 0–255 rather than 0–1.
* **[WinNative](https://github.com/WinNative-Emu/WinNative)** — the route DIS took into this tree. qwertypower's engine landed there first, as WinNative PR #731, running inside WinNative's own Vulkan compositor; the files under `rpcs3/Emu/RSX/VK/dis/` are a port of that implementation from C into RPCS3's Vulkan renderer. The shader code is unchanged; the sources moved from standalone `.comp` files compiled by `glslc` at build time to string literals compiled by glslang at runtime, and their comments were stripped.

Licensing, stated plainly: most of this tree is GPL-2.0-only, but the frame generation sources are not. lsfg-vk is MIT, which is compatible with both GPLv2 and GPLv3, but the files under `rpcs3/Emu/RSX/VK/lsfg/` descend from Camille LaVey's Eden port rather than from lsfg-vk directly, so they carry its GPL-3.0-or-later terms. The files under `rpcs3/Emu/RSX/VK/dis/`, including every DIS shader source embedded in `dis_shaders.cpp`, are qwertypower's under GPL-3.0. Those SPDX headers have to survive, and a build with frame generation compiled in contains GPL-3 code. What was written for this port carries no such inheritance: `lsfg_dll.*`, which walks the PE resource tree, `lsfg_dxbc.*`, which bridges to DXVK's translator, and the RPCS3 side — `VKFrameGeneration`, the presenter wiring, and the settings and menu surface around both engines.

### DIS frame generation

The second engine is a complete open-source implementation of **Dense Inverse Search** optical flow, contributed by **qwertypower** (DEVAR Entertainment LLC) under GPL-3.0.

Unlike the Lossless Scaling path it depends on nothing the user has to own or install. The whole chain ships inside the APK — fourteen compute shaders, thirteen stages, the luminance pass built twice because its storage format is baked into the SPIR-V and R16F storage support is not guaranteed — and runs in the same presenter, so frame generation is available on a fresh install with no `Lossless.dll`.

The algorithm is DIS, and its reference implementation is OpenCV's `DISOpticalFlow`, which in turn adopted Till Kroeger's [OF_DIS](https://github.com/tikroeger/OF_DIS). What is original is the Vulkan compute realisation of it — the pyramid, the descriptor and barrier layout, the parallel replacement for OpenCV's sequential propagation scan, and the refinement schedule that trades quality against the number of frames being generated — built to run inside a mobile presenter at frame rate. Its own pacer did not come across: here both engines are paced by `lsfg_pacer`.

| Stage | Shaders | What it does |
| --- | --- | --- |
| Luminance pyramid | `dis_luma_r16`, `dis_luma_r32` | One channel per pyramid level on the 0–255 scale the tuned constants expect, so the search passes move half the bytes, or a quarter of them where R16F storage is unavailable and the R32F variant is used instead, and no longer collapse RGBA per fetch |
| Gradients | `dis_gradient` | Sobel-style luminance gradient of the previous frame, one pyramid level per dispatch |
| Patch inverse search, coarse to fine | `dis_inverse_search`, `dis_propagate` | Gauss-Newton descent over 8×8 patches, initialised from the coarser level; then spatial propagation, which scores each sparse cell against its four neighbours at doubling distances and keeps the best |
| Sparse grid to dense flow | `dis_densify` | Confidence-weighted upsample of the stride-3 sparse grid into a per-pixel flow field |
| Variational refinement | `dis_vr_prep`, `dis_vr_d1`, `dis_vr_d2`, `dis_vr_w`, `dis_vr_coef`, `dis_vr_sor`, `dis_vr_add` | Warp and residual, first and second derivatives, smoothness weights, the 2×2 system assembled from the brightness and gradient constancy terms, a red-black SOR solve for the increment, and the guarded add back into the flow |
| Warp to the in-between frame | `dis_interpolate` | Samples the previous and next frames along the dense flow and blends them at time *t* |

The engine sits behind its own resolution knob — Fast, Balanced or Quality, in pixels on the frame's shorter side — kept separate from the Lossless Scaling percentage, and the two engines are mutually exclusive because the presenter drives one interpolator per frame.

## License

Most files are licensed under GNU GPL-2.0-only; see [LICENSE](LICENSE). Some files are licensed differently — check the individual file headers. This project is not affiliated with or endorsed by Sony Interactive Entertainment.
