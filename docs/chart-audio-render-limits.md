# Chart audio rendering limits

Music-cache rendering (including adjacent-track preload) and chart/replay audio export share `ChartAudioRenderer`. They reject an oversized render with an explicit resource-limit error instead of allocating from arbitrary chart duration or publishing truncated audio. This does not reject the chart in the parser or prevent gameplay.

- The dense float output payload is limited to 128 MiB: 16,777,216 stereo frames at 44.1 kHz, approximately 6 minutes 20.44 seconds. Playback-rate scaling and complete decoded sound tails count toward this ceiling.
- Cumulative sample mixing is limited to 158,760,000 output frames across all keysounds and generated sounds (one hour of single-sound work). Club-beat plans are limited to 100,000 beats before generation.
- `RenderOptions::maxOutputFrames` and `maxMixedFrames` can lower these limits; values above the production ceilings do not raise them.
- Initial duration and sound-tail growth are checked before narrowing or allocation. Cancellation is checked before rendering/preload, between sounds, every 4096 mixed frames, between output blocks and before publication.
- WAV conversion uses one 4096-frame PCM block, not a second full-track PCM copy. A uniquely owned sibling staging directory holds the output until the complete file closes successfully; atomic replacement publishes it. Cancellation and errors remove only owned staging and preserve any preexisting destination.

The 128 MiB figure is a float-payload limit, **not a total process-memory guarantee**: growth may temporarily retain the old float buffer, and parsed chart data, decoded assets and archive batches consume additional memory. This change does not impose an aggregate decoded-asset budget or make underlying decoder/archive reads instantly interruptible. Storage failure is reported rather than treated as successful partial output.

Very long tracks, extreme playback durations and unusually heavy overlapping sound mixes may therefore be unavailable in the rendered music cache or export. Shorter ordinary charts retain their timing, rate conversion and full sound tails; there is no silent duration clamp.
