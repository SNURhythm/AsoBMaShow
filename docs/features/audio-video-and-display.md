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

## Verification

Use `audio_*_tests`, `jukebox_restore_tests`, `video_*_tests`,
`frame_pacer_tests`, `sdl_display_backend_tests`, `display_settings_manager_tests`,
and renderer/view tests. Compile shaders through the documented shader workflow.

## Related pages

- [Gameplay and scoring](gameplay-and-scoring.md)
- [Practice and analysis](practice-and-analysis.md)
- [Build, release, and verification](build-release-and-verification.md)
