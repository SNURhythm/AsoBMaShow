# Native chart source selection

Goal: OS-native selection and copying in the read-only chart source overlay on
iOS, Android, macOS and Windows, preserving source decoding and practice highlights.

Design: keep the current overlay and toolbar; host a native selectable text
surface in its document rectangle. Use UITextView, Android TextView, NSTextView,
and Windows Rich Edit respectively. Practice highlights remain independent of
manual selection. Convert source UTF-8 ranges to native UTF-16 indices once per
highlight update. Backends marshal view operations to the correct UI thread and
remove their controls on close, scene cleanup or pause. Escape/back requests
cross back through owned state, never an asynchronous scene pointer.

- [x] Implement and test common Unicode range conversion and native lifetime contract.
- [x] Implement independent Apple, Android and Windows surfaces.
- [x] Integrate native display, highlights, scrolling, theme and layout into the overlay.
- [x] Verify focused tests, native platform compilation where available and desktop build.
- [x] Review, commit, push and update PR #154 with validation limits.

The existing document renderer remains the fallback when no native window is
available (including headless tests). Linux is not a released app target in this
repository; this work does not introduce a Linux UI toolkit dependency.

Validation: desktop `main` and six focused tests pass, including actual AppKit
selection/copy and cleanup. Apple strict ARC compilation passes for macOS, iOS
device and simulator; Android JNI C++ and Java compilation passes. Windows
compilation and the registered native Windows integration test require a Windows
toolchain unavailable on this machine. Mobile gestures and Windows native input
still need platform runtime smoke testing.

Review fixes: application backgrounding closes the source surface through the
application lifecycle callback, and Escape suppression uses SDL event timestamps
to catch a delayed duplicate of the native closing key. Focused lifecycle and
overlay regression tests cover both cases.
