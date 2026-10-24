# Android Emoji Fix Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan.

**Goal:** Render missing emoji in Android built-in text and scalable/bitmap skins.
**Architecture:** Use Android Paint/Canvas through a process-lifetime JNI bridge in SystemTextRasterizer. Preserve authored-font precedence, straight RGBA, shared baselines and existing memory budgets. Generalize TextView's existing native-system fallback to Android.
**Tech Stack:** C++23, JNI, Android Java graphics, SDL3, existing skin atlases.
**Spec:** User request to fix the failures reproduced on Galaxy S20 FE with APK 1ea0b7dd.

## Constraints and review focus

- Android API 28 minimum; no dependency or system-font changes.
- Native workers must attach/detach correctly; no Activity reference retained.
- Bound UTF-8 input, dimensions, pixel allocations and JNI results before copying.
- Missing/non-emoji glyphs must not become native emoji; preserve monochrome emoji.
- Native glyphs must align with authored fonts; retain tint and straight alpha.
- Follow current-branch commit/push policy, reuse Android version code 5, no distribution upload.

## Task 1: Regression

- [x] Retain the failing actual-APK device probe as repeatable tests covering built-in text and both atlas types.
- [x] Observe color-glyph failure against the existing APK.

## Task 2: Fix and validate

- [x] Add AndroidTextRasterizer.java and a bounded JNI bridge in SystemTextRasterizer.cpp; initialize before native workers in AsoBMaShowActivity.loadLibraries.
- [x] Expose Android text measurement and rendering; route TextView native fallback through it without changing authored precedence or iOS behavior.
- [x] Test color pixels, unsupported glyphs, bitmap/scalable baseline agreement, tint/alpha and budgets on device.
- [ ] Build affected desktop tests and Android APK, obtain fresh independent review, install and smoke-test the final build.
- [ ] Commit and push verified task changes; update linked PR 153.

## Review correction

The independent review identified default-font metrics clipping fallback ink.
A padded Canvas comparison reproduced 54 lost pixels in stacked accents, two in
Tibetan text, and nine in Arabic text. Raster bounds now include actual run ink,
with a separate horizontal offset, unchanged advance, and the actual baseline.
TextView and both atlas builders propagate these metrics. The same padded
comparison now loses zero pixels; dedicated device composition checks cover the
UI's overhang and baseline placement. No findings were reported for JNI lifetime,
attachment ownership, straight alpha, or allocation bounds.

The Galaxy's font covers every ICU-recognized emoji, so the device runner reports
a skip for the optional recognized-but-unavailable emoji case. Invalid scalar,
ordinary text, and byte-budget rejection remain exercised.

## Verification

- Android release build passed (version code 5).
- Actual APK device runner passed on Galaxy S20 FE / Android 13: color/monochrome
  emoji, straight alpha, byte limits, JNI workers, padded ink references,
  TextView overhang/baselines, and scalable/bitmap atlas baselines.
- Host text_view_font_lifetime_tests, skin_resource_catalog_tests and
  text_input_box_tests passed after the final metric changes.
