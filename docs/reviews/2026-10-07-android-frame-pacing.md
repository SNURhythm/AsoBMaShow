# Galaxy S20 FE frame pacing verification

Measured on 2026-10-07 KST using the connected SM-G781N (Android 13), its
120 Hz display mode, Vulkan, and a native 2400 × 1080 landscape surface.
The workload was the existing `simple-play-simple` custom skin with
`"Fresco" [Type 38]`, 3,431 notes, Easy gauge and BGA enabled. VSync remained
on, the app frame cap remained unlimited, and render scale remained 1.0.
The measurements cover unattended gameplay rendering, not a human input-latency test.

## Results

| Build/experiment | Measured gameplay seconds | Presented FPS | Frame interval p95 |
| --- | ---: | ---: | ---: |
| Current branch before swapchain fix | 45.06 | 84.333 | 16.987 ms |
| Add Android game category only | 49.98 | 84.211 | 16.981 ms |
| Explicitly request three buffers | 59.76 | 85.385 | 16.975 ms |
| Handle Android suboptimal swapchain correctly | 59.71 | 118.045 | 8.514 ms |
| Final release, probes removed, after rotation/resume | 90.01 | 118.216 | 8.514 ms |

The first fixed run's five-second intervals were 117.0–118.2 FPS. Its p99
frame interval was 8.578 ms, with two intervals above 12 ms in 7,050 presented
frames. This exceeded the requested sustained 110 FPS target.

The final release delivered 10,642 frames over 90.013 seconds. All five-second
intervals were 118.0–118.4 FPS. Its p99 was 8.590 ms and its longest interval
was 10.951 ms; no intervals exceeded 12 ms. Thermal status remained 1
(light), with the reported AP sensor at 39.3 °C and skin sensor at 37.9 °C
at the end. This is about 40% more displayed frames per second than the
current-branch baseline.

Measurements use the app's SurfaceView layer in `dumpsys SurfaceFlinger
--latency`, polling every 350 ms and deduplicating valid actual-presentation
timestamps. FPS is `(presented frames - 1) / elapsed presentation time`.
Loading and scene transitions are excluded. The in-app FPS counter and the
requested display mode are supporting checks, not the FPS measurement.

These are sequential runs on one phone with normal thermal management and
USB power, not a controlled comparison across devices or a guarantee for all
charts. No thermal limits, global performance settings or rendering quality
were overridden.

## Root cause and correction

Temporary driver probes showed `vkQueuePresentKHR` returning
`VK_SUBOPTIMAL_KHR` (1000001003) on every landscape frame. The swapchain used
identity pre-transform (1), while the surface reported a 90-degree transform
(2). Width, height, color/depth formats and reset flags were unchanged.
Nevertheless, bgfx recreated the swapchain each frame, including on the static
main menu, repeatedly waiting for the GPU and reallocating attachments.

Android's compositor already handles this identity-transform presentation.
The advisory result permits continued presentation; rebuilding the same
swapchain cannot remove the transform mismatch. The generated Vulkan source
now accepts suboptimal acquire/present results on Android. It still rebuilds
on out-of-date results, recovers lost surfaces, and handles SDL resize/reset.
Non-Android result handling and the iOS Metal backend are unchanged.

See [Android's Vulkan rotation guidance](https://developer.android.com/games/optimize/vulkan-prerotation)
and [Vulkan's swapchain result semantics](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain.html).

The existing MSAA attachment fix remains in the same generated source; the
vendored bgfx checkout is untouched. Temporary timing probes were removed.
The extra buffer request was also removed: this device's Vulkan surface
already requires at least three images, so changing the request had not
changed the actual image count.

## Samsung game classification

The app previously had no Android game category. It now declares
`android:appCategory="game"`, appears in Gaming Hub, and is listed by Android's
GameManager. The category-only run did not improve FPS. Samsung's service dump
still had no AsoBMaShow game profile, and Android reported no applicable
performance-mode intervention. The measured improvement came from fixing the
Vulkan recreation loop, without forcing a Samsung performance policy.

## Validation

- Signed restricted-file-access release APK built with `scripts/android_firebase_deploy.sh --build-only` and installed without clearing app data; final version code 1791317295.
- Desktop `main` build passed with six jobs.
- Vulkan MSAA and swapchain regression CTests passed. The latter compiles the actual generated acquire/present result switches for both Android and non-Android branches, checking success, suboptimal, out-of-date and lost-surface behavior. The Android suboptimal case failed before the correction.
- Final build displayed correctly after landscape → portrait → landscape, then background/resume. Original landscape preference was restored.
- Device DocumentsProvider, native refresh and skin-directory import instrumentation all passed again on the final APK. Coverage includes protected database/profile trees, destination reservations through source deletion, skin progress, cancellation, ownership and cleanup.
- The earlier release Java unit-test run passed all 41 tests.
- No iOS rebuild was run for this Android-only Vulkan behavior change. No Firebase upload was performed.
