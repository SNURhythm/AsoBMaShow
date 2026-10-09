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

Frame timestamps come from the replay frame index, not wall-clock rendering
time. The exporter drains the encoder and checks the number of encoded frames
before reporting success. Audio encoding and MP4 muxing use the existing path.
Display vsync is temporarily disabled during Android export and restored on
exit; a driver may still limit presentation speed.

## Verification

The normal CTest suite includes `android_replay_codec_tests`, which covers
hardware selection, unsupported dimensions/rates/bitrates, missing Surface
support, vendor capability errors, and the Android 9 fallback policy.

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

On a Samsung SM-G781N running Android 13, the Qualcomm H.264 encoder exported
the 181.42-second Fresco replay at 2400x1080/60 fps in 186.90 seconds, compared
with 222.14 seconds for the threaded software path (about 16% less time).
These are individual runs, not a controlled benchmark average. The output
contained all 10,885 frames, decoded without errors, and its decoded audio
matched the software export exactly. The Surface integration tests passed all
three modes above. An Android 10 emulator also completed a software fallback
export with 1,260 frames at 1920x1080/60 fps.

Rendering and presentation remain the largest measured cost. Disabling vsync
did not demonstrate an additional speedup on this phone, and GPU readback and
upload remain candidates for future optimization.
