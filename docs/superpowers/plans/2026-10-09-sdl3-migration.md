# SDL3 Migration Implementation Plan

> **For agentic workers:** Execute task-by-task using superpowers:executing-plans.

**Goal:** Make the existing application build and operate with our SDL3 component forks.
**Architecture:** Direct SDL3 APIs, common fork dependencies, existing bgfx/audio and gameplay architecture.
**Tech Stack:** C++23, Objective-C/Java, CMake/Xcode/Gradle, SDL3 and SDL3_ttf.
**Spec:** ../specs/2026-10-09-sdl3-migration-design.md

## Global constraints

Use migration/sdl3 and the existing component patch branches, no new worktrees.
Do not reformat whole files, rewrite rendering/audio, alter replay formats or deploy.
Preserve gameplay timing, saved bindings, text input, import and lifecycle semantics.

## Review focus

- Long-running timestamps and events within a millisecond preserve gameplay order.
- Hotplug/reordered controllers and displays keep bindings/settings meaningful.
- Borrowed drop/edit text survives only through explicit copies; no double frees.
- iOS cancelled touches, background/resume and cold URLs preserve input/lifecycle.
- Font work during asynchronous loading respects thread ownership.

## Tasks

- [x] Dependency/API conversion: update CMake/manifest to fork targets, observe
  existing application failing against SDL3, use upstream renaming helpers on
  application/test source only, resolve signatures and boolean/ownership changes.
  Files: CMakeLists.txt, vcpkg.json, src, tests, scripts using extracted fixtures.
  Verify: cmake --build cmake-build-debug --target main -j 6.
- [x] Input/display contracts: update SDLInputBackend and LogicalGameplayInputAdapter
  nanosecond conversion and device enumeration; update SDLDisplayBackend's native
  ID mapping and fullscreen semantics. Add assertions for large/submillisecond
  timestamps and noncontiguous display/controller IDs before implementing fixes.
  Verify input_timestamp_tests, input_device_registry_tests, sdl_touch_input_source_tests,
  sdl_display_backend_tests and existing gameplay tests.
- [x] Text/resource contracts: port TextView, TextInputBox, SkinTextAtlas and native
  surface helpers; preserve IME text and audit font thread affinity. Verify
  text_input_box_tests, text_view_font_lifetime_tests and text/skin renderer tests.
- [x] Mobile integration/fork patches: port required UIKit raw touch, async gamepad,
  IME and primary-scene behavior; update Android library/bootstrap and iOS Xcode
  framework/setup/audit references. Verify component build and scene regression,
  scripts/ios_release_verify.sh and Android build-only plus emulator smoke.
- [x] Finish: run full desktop CTest and platform verification, obtain independent
  branch review, fix material findings, push component commits before app pointers.

## Execution ledger

- Initial state: app 82889edb, SDL 7d5d0fb0f, SDL_ttf f408ee63 on clean patch branches.
- Ruling: use fork submodules on desktop as well as mobile to avoid mixing SDL
  versions and to exercise the same dependency sources; vcpkg retains other libraries.

- SDL3 renumbers consumer scancodes 258–286. Input profile schema 11 migrates
  supported keys by meaning; keys removed by SDL3 are retained as inert negative
  IDs for rebinding, rather than attaching their old number to an unrelated key.
- TextView font caches are per-thread; asynchronous skin atlases already open,
  rasterize and close their own fonts in the preparation call.

- Refreshed the built-in renderer PNG for SDL3_ttf text rasterization and metrics.
  Inspection localized changes to text (5,047/921,600 pixels beyond tolerance);
  the existing renderer timing/geometry JSON remains byte-for-byte unchanged.


## Verification completed 2026-10-09

- Desktop: full build (`cmake --build cmake-build-debug -j 6`) and all **452/452**
  CTest tests passed with `--output-on-failure -j 6`. Application smoke initialized
  Metal, refreshed the library and exited cleanly.
- iOS: `IOS_RELEASE_BUILD_JOBS=6 scripts/ios_release_verify.sh` passed on the final
  source tree: 66 native tests, 52 setup tests, 20 workflow tests, 16 artifact-audit
  tests, the unsigned device build and final packaged-artifact audit. The app
  embeds and links SDL3 and SDL3_ttf, with no SDL2 framework.
- iOS scene fixture on the simulator: initial window/frame, exact-once
  background/foreground transitions, same-process resume and warm/cold URL
  delivery passed. Component regressions cover primary-scene replacement,
  reconnect activity delivery and Unicode composition offsets. Simulator stopped
  after verification.
- Android: signed `restricted_file_accessRelease` build passed via
  `scripts/android_firebase_deploy.sh --build-only`; a follow-up incremental build
  passed with the final source up to date. Android 10 emulator startup, Vulkan
  rendering, Settings touch input, Home/resume with the same PID, and archive VIEW
  import into the visible library passed. Emulator stopped afterward.
- Independent final review found one systemic cancellation omission across eight
  custom gesture routes. Fixed matching-finger cleanup without release actions;
  new extracted-handler regression reproduced 22 failed assertions before the
  fixes, then passed. Real ranking viewport/RecyclerView integration also passed.
  Scoped re-review marked every finding addressed with no new material issues.
- Component pins: SDL `f210d0d53`, SDL_ttf `281779c`; both pushed to their dedicated
  patch branches before committing the application pointers.

No distribution upload was performed. Windows/Linux builds and physical-device
latency, audio, controller/MIDI, external-display and comprehensive IME behavior
were not verified on this machine. The preview cancellation fixture checks
forwarding; it does not simulate real-device delivery or the handler's timed
cancellation grace period.

The existing checkout was retained as instructed. Desktop dependencies use the
same pinned forks as mobile. The renderer golden update was limited to inspected
SDL3_ttf font differences; renderer timing and geometry expectations were kept.
