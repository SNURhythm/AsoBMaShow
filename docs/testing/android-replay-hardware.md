# Android replay hardware export

Android replay export selects a hardware H.264 encoder through MediaCodec when
its advertised capabilities support the requested dimensions, frame rate,
bitrate, and Surface input. Android 10+ uses the platform hardware flag;
Android 9 excludes known platform software codec names. Missing capabilities,
media-service errors, or hardware/EGL setup failures select the existing
software encoder instead. An error after frame submission fails that export;
it does not splice software frames into a partially written hardware stream.

The renderer still produces its existing queued BGRA readbacks. A private EGL
context uploads each readback and draws it into the encoder Surface. Android
performs the color conversion and H.264 encoding. This avoids CPU YUV
conversion, but is **not direct Vulkan texture sharing**: GPU readback and an
upload remain. The context is restored after every operation so setup,
worker-thread submission, and cleanup do not replace bgfx's current EGL state.
The default EGL display is shared and is not terminated by the exporter.

Android Vulkan texture readbacks use a transfer-destination-only staging
buffer and request host-visible, cached memory without requiring coherency.
On the tested Adreno device, also requesting transfer-source usage excludes
the faster cached non-coherent memory type. After the GPU fence completes,
the exporter path invalidates the mapped allocation before copying its pixels,
as required by [Vulkan cache visibility rules](https://docs.vulkan.org/refpages/latest/refpages/source/vkInvalidateMappedMemoryRanges.html).
The existing uncached allocation fallback remains available. Upload buffers,
buffer readbacks, and screenshots keep their existing allocation policy.

Frame timestamps come from the replay frame index, not wall-clock rendering
time. The exporter drains the encoder and checks the number of encoded frames
before reporting success. Audio encoding and MP4 muxing use the existing path.
Display vsync is temporarily disabled during Android export and restored on
exit; a driver may still limit presentation speed.

## Verification

The normal CTest suite includes `android_replay_codec_tests`, which covers
hardware selection, unsupported dimensions/rates/bitrates, missing Surface
support, vendor capability errors, and the Android 9 fallback policy.
`bgfx_vulkan_readback_tests` compiles the actual patched readback and host-buffer
methods. It checks buffer usage, allocation requests, visibility after GPU
completion, fallback when cached memory is unavailable, mip sizes, allocation
offsets, and output bounds. Non-Android behavior is checked separately.

To run the real arm64 device integration test, provide an NDK and the Android
FFmpeg prefix produced by this project's build:

```sh
python3 scripts/android_replay_surface_verify.py \
  --ndk /path/to/android-sdk/ndk/28.2.13676358 \
  --ffmpeg-prefix /path/to/installed/arm64-android \
  --adb /path/to/android-sdk/platform-tools/adb \
  --serial DEVICE_SERIAL
```

Host `ffmpeg` and `ffprobe` must be on PATH. `--codec` can select a specific
device codec. The script compiles a standalone native test, copies it into
`/data/local/tmp`, and removes its temporary device files afterward. It does
not install the app. Unsupported test resolutions/rates fail explicitly.

The test encodes 320x180 at 60 fps and 2400x1080 at 60/120 fps, then decodes
every frame. It verifies dimensions, duration, timestamps, frame count, color
channels, orientation, alternating frame markers, and one-pixel grayscale
lines. The last check protects native-resolution text and notes from shader
texture-coordinate precision loss. Setup and cleanup run on one thread while
frame submission runs on another, matching the app.

## Device result

On a Samsung SM-G781N running Android 13, the 181.42-second Fresco replay
at 2400x1080/60 fps produced these timings:

| Export path | Total | MP4 rendering/encoding stage |
| --- | ---: | ---: |
| Threaded software | 222.14 s | 203.14 s |
| Qualcomm hardware H.264 | 186.90 s | 167.79 s |
| Hardware H.264 + cached Vulkan texture readback | 105.61 s | 81.97 s |

The readback optimization reduced total time by 43.5% and the MP4 stage by
51.1% compared with the initial hardware path. These are individual runs, not
a controlled benchmark average. The final output contained all 10,885 frames,
decoded without errors, and its decoded audio matched the earlier exports
exactly. Sampled frames at 30, 90, 160 and 175 seconds matched the initial
hardware output exactly in the top 2400x1020 region, excluding the changing
clock/uptime area. The completed export returned to the Records screen.

The Surface integration tests passed all three modes above. An Android 10
emulator also completed a software fallback export with 1,260 frames at
1920x1080/60 fps during the initial hardware implementation.

Final profiling separated 39.07 seconds of draw preparation and 35.57 seconds
of frame advancement from the concurrent 76.85-second encoder worker. GPU
readback and upload still remain. Disabling vsync alone did not demonstrate
an additional speedup on this phone.
