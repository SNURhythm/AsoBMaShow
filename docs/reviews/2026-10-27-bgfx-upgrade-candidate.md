# Shader-compatible bgfx presentation candidate

The first candidate (`88cd6afd`) updated bgfx for the reported iPad Metal
presentation delay while preserving timer pacing, the iOS flip-after-render flag,
three drawables and an Apple GPU frame limit of three. The user reported no
improvement. The follow-up below changes only the Apple GPU frame limit to two;
its effect on device latency and cadence remains unmeasured. No display link or
diagnostic callbacks are added.

## Revisions

The application baseline is `cff3f2ffdcca7b7dbadc0c04aada7a82e3bec959`.

| Dependency | Baseline | Candidate |
| --- | --- | --- |
| bgfx.cmake | `6e2138a92033af5f1e24d262448e3a81b987df10` | `fb7480cbac04e58bd803fa80863250c47f2ba9b4` |
| bgfx | `0b1c1ecaca917acc093f755bf8468711a5b24aee` | `6d0d15f2a51a4072ea7e4fa476d1b188b47708d8` |
| bx | `fa641d8581f7f6f696a37abe4b80558aca161440` | `9e3fadf6f11380031486be704d2ff46ca143664f` |
| bimg | `b43fea9eae0e6a98118454a6d17c6cb25f5e6403` | `371d90098b1fd017cd00205979d5ef74b8c3ed62` |

The candidate is the August 22 snapshot. It includes these upstream changes:

- [#3715](https://github.com/bkaradzic/bgfx/commit/526b886105dd8cca22d8a94e7c49f21e9a1e5af6): the Metal GPU semaphore honors `maxFrameLatency`. The baseline always admitted three GPU frames. The first candidate requested three; the follow-up requests two as a separate experiment.
- [#3714](https://github.com/bkaradzic/bgfx/commit/0158b15c2398f56aec1d99fa480eaf78745cd978): rendering commits at the end of submission and presentation uses a separate command buffer.
- [#3748](https://github.com/bkaradzic/bgfx/commit/ac4da6369a9b710c95b25dd4f2aff983af51bd4b): empty frames avoid unnecessary drawable acquisition.

October 26 upstream was examined but not selected. The
[August 23 raw-buffer change](https://github.com/bkaradzic/bgfx/commit/dd961560d55db75a3ed2a7ee8e3ba0adbd814e77)
requires shader-container format 12 or newer. All 96 committed Metal, SPIR-V,
ESSL and DX11 binaries use format 11. Format 12 adds raw-buffer binding masks and
texture-dimension reflection; changing only the version byte would not migrate
those files correctly. The latest shader compiler also defaults to a newer Metal
language profile. A full shader/toolchain migration, October's FetchContent
layout and unretained-command-buffer optimization are outside this candidate.
The existing shader sources and binary payloads are unchanged.

## Compatibility changes

- Adapt the generated Metal patches to `renderer_mtl.cpp` and metal-cpp, still
  compiling the patched copy as Objective-C++ 20. Upstream tracked files remain
  untouched.
- Preserve the legacy reflection path through iOS 16 and UIKit main-thread
  ownership of layer initialization and resizing. Keep the drawable count at
  three independently of the GPU queue limit.
- Preserve buffer-backed shared RGBA/BGRA readback and the ordinary fallback for
  mipmaps, unsupported formats and Simulator. Synchronize managed resources only.
- Preserve the retained native-video texture handoff. This revision uses ordinary
  command buffers, which retain encoded resources; a configure-time guard requires
  a new ownership review if that upstream behavior changes.
- Keep Android Vulkan MSAA, swapchain-result, VSync-off and cached-readback fixes.
  Adapt readback to upstream's layer/aspect parameters and staging-size calculation.
- Replace the removed `bgfx/platform.h` includes and explicitly pass layer zero
  before the mip in the native readback test. Disable bgfx's newly available video
  decoder; application video continues through its existing implementation.
- Adapt the two image resize calls to bimg's updated `stb_image_resize2.h`, keeping
  linear, independent-channel filtering, including alpha, and the same default
  filters. Bit-identical output across the two resizer versions is not claimed.

## Initial candidate validation

- The full desktop build passes, and all 537 CTest checks pass at `-j 6`
  (60.84 seconds on the final run).
- Desktop `main`, rendering, video and iOS-platform test targets build successfully.
- All 87 selected CTest checks pass. They include real Metal rendering, shader
  loading, export readback, image decoding, and native NV12 texture lifetime.
  The existing NV12 fixture now exercises GPU limits 1, 2 and 3 in both single
  and multithreaded modes, including early handle destruction and decoder-pool
  reuse. This checks safety; it does not measure queue occupancy or latency.
- All 55 iOS build-setup tests pass. Generated Metal source also passed standalone
  macOS and iOS 15 syntax checks during integration.
- The unsigned iOS device build passes with deployment target 15.0, using
  `scripts/ios_firebase_deploy.sh --build-only`. Nothing was uploaded or installed.
- The signed Android restricted-file release APK builds with
  `scripts/android_firebase_deploy.sh --build-only`. The build does not upload it.
- Independent code review found no actionable defects. Physical-device timing
  remains unmeasured.

The broader build also exposed isolated scene and Android lifecycle test fixtures
missing boundaries introduced by the existing streak feature. Their adapters are
updated separately; production behavior and extracted methods remain unchanged.
Stale local Swift incremental objects/dependencies were backed up and regenerated
for the translation test; its subsequent build correctly performed no work.

## GPU-limit follow-up

The user reported that the first candidate did not improve the observed delay.
No new timing values or cadence measurements accompanied that report.

The follow-up sets `bgfx_init.resolution.maxFrameLatency` to two on Apple
platforms. Other renderers already used two. Both generated Metal layer patches
still explicitly set `maximumDrawableCount` to three. The bgfx revision, timer
pacing, reset flags, shaders and other rendering settings remain unchanged.

Follow-up verification: desktop `main` and the unsigned iOS device build pass.
The Metal readback check and both native-video lifetime tests pass; the latter
exercise GPU limits 1, 2 and 3. Generated desktop and iOS Metal sources retain
explicit three-drawable assignments. Nothing was uploaded or installed.

## Device comparison still required

Compare `88cd6afd` (GPU limit three) with this follow-up (GPU limit two) on the
affected iPad with identical chart, blur/BGA settings, frame cap, orientation and
warmup. Verify actual 120 Hz cadence
alongside HUD Present Delay. The separate presentation command buffer can move
the HUD's measurement boundary, so a smaller HUD number alone does not prove
fresher displayed state.

Both builds keep three drawables. GPU completion and display presentation are
different events. The earlier 60 Hz result with two drawables does not establish
what a two-frame GPU limit will do with a three-drawable pool. No performance
benefit is claimed for this follow-up before device measurement.

See the [investigation plan](../superpowers/plans/2026-10-27-metal-presentation-upstream-investigation.md)
for the controlled comparisons and the [earlier observations](2026-10-27-metal-hud-followup.md).
