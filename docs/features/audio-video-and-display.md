# Audio, video, and display

## Intent and user flow

The media stack schedules chart audio, BGA/video, music-player playback, and
optional generated layers while letting players choose safe device, latency,
volume, display, VSync, and frame-cap settings. Changes that could disrupt an
active device or window are applied through explicit runtime boundaries.

## Code map

- `src/audio/` contains Jukebox, device manager, backends, chart audio,
  generated metronome/Club Beat, playback rate, and music-player services.
- `src/video/` owns decode state, display settings, frame pacing, renderer
  coordination, SDL display backend, and video-player memory pressure handling.
- `src/rendering/` owns bgfx setup, render plans, post-processing, batching,
  uniforms, cameras, and shader-facing primitives.
- Settings scenes and models project safe configuration and capability state.

## Boundaries and invariants

Audio scheduling follows authoritative chart time; playback rate changes output
rate without redefining the chart timeline. Backends expose capability/runtime
state through portable interfaces, and risky reconfiguration has rollback or
recovery behavior. Memory-pressure handling may evict idle media and decoded
artwork but preserves active playback resources according to their lifecycle.

Music Player releases fullscreen video and restores its prior visual settings
before scene destruction while it owns those overrides. Unused and already-
exited scenes leave shared BGA policy unchanged. This teardown does not stop
native music playback or refresh the UI.

On Apple Metal renderers, supported 8-bit SDR H.264/HEVC playback retains
VideoToolbox frames and imports their NV12 planes directly as textures. The
fragment shader performs range-aware YUV-to-RGB conversion while drawing;
there is no CPU pixel conversion, readback, or upload on this path. Native
texture views retain the CoreVideo mappings until queued GPU work completes.
Other renderers and unsupported formats use the existing software decoder.
A failed native import or decode reopens software playback at the current
timeline position. Skin movies requiring a strict preallocation bound keep
the existing CPU budget because VideoToolbox owns its decoder pool.

`HardwareVideoDecoder.h` holds shared format negotiation;
`HardwareVideoFrame.h` defines the native import boundary. The first backend
is `HardwareVideoFrameApple.mm`; Android and Windows playback currently
remain software-decoded. The storage codec dialog probes the same decoder
and import path before reporting hardware playback capability.

## Archived chart asset budgets

Gameplay and audio export keep a 64 MiB concurrent extraction scheduling budget.
Gameplay splits it between extraction and the decode queue when those stages
run concurrently. Each stage admits one oversized asset when otherwise empty;
other entries wait until capacity is available. Serial extraction also accepts
oversized assets. The budget is not a maximum audio file or archive size, and
it does not cap decoded PCM, decoder dictionaries, or consumer-retained data.
Cancellation, consumer failure, and missing-entry-only retry remain enforced.

## Selected charts and background work

Selected-chart audio rendering and music playback do not inherit the speculative
preload quotas. Adjacent-track preloading retains its 128 MiB output, one-hour
cumulative mixing, and Club Beat planning bounds. Foreground rendering instead
checks actual WAV/container and arithmetic representability, cancellation, and
allocation failures.

Selected chart stage/back/banner images accept oversized encoded sources and
are resized to a shared 2048-pixel display image. Chart-owned images do not consume
authored-skin quotas. Library thumbnails, image worker concurrency, retained cache
budgets, and authored-skin/package limits remain bounded.

## Verification

Use `audio_*_tests`, `jukebox_restore_tests`, `video_*_tests`,
`frame_pacer_tests`, `sdl_display_backend_tests`, `display_settings_manager_tests`,
`music_player_video_lifecycle_tests`, and renderer/view tests. Compile shaders
through the documented shader workflow.
`hardware_video_frame_tests` and its threaded variant exercise real Metal
sampling, color conversion, and decoder-pool reuse after GPU completion.
`gameplay_bga_sampling_metal_tests` covers native playback, seeking, and fallback.

## Related pages

- [Gameplay and scoring](gameplay-and-scoring.md)
- [Practice and analysis](practice-and-analysis.md)
- [Build, release, and verification](build-release-and-verification.md)
