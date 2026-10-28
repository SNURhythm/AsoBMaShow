# Direct Metal BGA Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. The user authorized implementation of the proposed direct path; continue in the current checkout.

**Goal:** Avoid the intermediate/composite pass for eligible zero-blur Metal BGA frames.

**Architecture:** Jukebox selects a pinned single opaque surface and submits into the existing final view. Dedicated tinted video shaders preserve conversion/brightness order. A false return leaves the existing fallback and media lease intact.

**Tech Stack:** C++23, bgfx, Metal, existing shader compiler and GPU readback tests.

**Spec:** docs/superpowers/specs/2026-10-27-direct-bga-design.md

## Global Constraints

- Keep corrected timer accounting, render order, three drawables and telemetry removal.
- No deployment, worktree, new telemetry, or pause/resume scheduling changes.
- Metal-only initial path; blurred/layered/uncertain-alpha content falls back.
- Preserve same-frame media ownership, brightness, geometry and opaque output.
- Export continues using its existing composition path.

## Review Focus

- Blur/layer/alpha rejection must retain the exact lease for fallback.
- Fit/Fill/NoExpand and viewport offsets must not alter geometry or letterbox alpha.
- YUV values outside [0,1] clamp before brightness, including native NV12.
- Direct/composite transitions must not leave stale view clear or framebuffer state.
- Missing shaders, exhausted transient buffers or stale frames must not partially submit.

### Task 1: Direct fullscreen submission and fallback

**Files:** src/audio/Jukebox.{h,cpp}, src/video/VideoPlayer.{h,cpp}, src/main.cpp, shader_src/fs_yuvrgb_tint.sc, shader_src/fs_video_nv12_tint.sc, compiled shader outputs, tests/gameplay_bga_target_tests.cpp, docs/reviews/2026-10-27-metal-hud-followup.md.

**Interfaces:** Jukebox::trySubmitFullscreenDirect(const PreparedGameplayBgaFrame&, bgfx::ViewId, float blurStrength, float brightness) -> bool; VideoPlayer::renderFullscreenTinted(bgfx::ViewId, float x, float y, float width, float height, float brightness) const -> bool. Existing fullscreen APIs keep their behavior.

- [x] Extend the existing Metal readback fixture with direct image pixels/draw counts and rejection/lease checks; extend its video fixture with tinted NV12 and YUV pixel checks.
- [x] Run the focused target and observe failure before implementing the new behavior.
- [x] Implement eligibility, atomic single-frame submission/fallback, cached image/video shaders, and main-loop selection without touching blur-on composition or export.
- [x] Compile shaders with `cd shader_src && SHADERC=../bgfx/bgfx/.build/osx-arm64/bin/shadercRelease python3 make.py`.
- [x] Build `tests_rendering tests_video tests_platform_ios main` with `cmake --build cmake-build-debug --target tests_rendering tests_video tests_platform_ios main -j 6`.
- [x] Run `ctest --test-dir cmake-build-debug -L '^(area_rendering|area_video|platform_ios)$' --output-on-failure --no-tests=error -j 6`; expect all selected checks pass.
- [x] Run `scripts/ios_firebase_deploy.sh --build-only --skip-init`; expect unsigned build success.
- [x] Document pause/resume source findings and device report without claiming a measured cause; update PR scope, commit, review and push.
