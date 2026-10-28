# Direct Metal BGA composition

## Intent and scope

The user approved trying a direct path for a single opaque BGA with zero blur.
The latest user measurement uses blur, so this fast path is not expected to
explain that measurement. Inspect pause/resume scheduling separately without
changing it or adding telemetry. Keep corrected timer accounting and all prior
rendering fixes. Work in the current checkout and push verified work to PR 161;
no deployment.

## Approach

Use the existing final output view for an eligible prepared fullscreen BGA,
with brightness applied in its image or YUV/NV12 draw. Keep the existing
intermediate/composite path for blur, layers, missing resources, uncertain image
alpha and other renderers. This is the approach approved in the conversation;
Metal-only enablement and conservative image eligibility are implementation
choices. A broader all-content reroute would change layered blending and is
out of scope. Reusing the embedded-skin preflight would add allocations/layout
registration each frame, so use the existing fullscreen geometry instead.

Eligibility is zero/negative effective blur, exactly one base or miss surface,
and no layer; images must be known opaque from a 1- or 3-channel decode.
Unknown/alpha-bearing images retain the old path. Videos are opaque. RGB
conversion clamps before applying brightness to match the old RGBA8
intermediate's range; the direct path removes its quantization/resampling.
Geometry remains unchanged; avoiding the half-resolution intermediate may make
unblurred detail sharper. Brightness is clamped to [0,1], and nonfinite inputs
fall back. Clear the final view to opaque black for letterbox bars.

## Ownership and integration

Jukebox::trySubmitFullscreenDirect(frame, view, blurStrength, brightness) returns
true only after one successful draw and finalizing the prepared media lease.
Failure must leave the lease available for the same-frame fullscreen fallback,
without decoding again or changing output view clear state. Reuse session-loaded
programs, validate transient capacity before allocation, and keep replay export
on its existing composition path. No shader lookup inside frame submission.

VideoPlayer adds a tinted fullscreen entry point using dedicated Metal-capable
YUV/NV12 shaders; existing video paths retain their original shaders. Prepare
those programs only on Metal and fall back if unavailable. Shader compilation
uses the repository script and committed Metal/SPIR-V/ESSL outputs.

## Verification

Extend the existing native Metal BGA tests to check one direct draw, brightness,
orientation, opaque letterbox bars, fallback for alpha/layers/blur, and lease
release/retention. Exercise both native NV12 and software YUV tinted video using
the existing generated fixture. Retain blur-on/off tests and export checks.
Build rendering/video tests and desktop main, run the affected groups in parallel,
and compile unsigned iOS. Review the branch before pushing. Device latency
benefit remains unproven until the user compares the build.
