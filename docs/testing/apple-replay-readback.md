# Apple replay readback

iOS and unified-memory macOS replay exports use a shared Metal buffer as the
storage for their BGRA readback textures. The GPU blits into that texture,
then bgfx waits for completion and copies the buffer's rows into the existing
export queue. This avoids Metal's `getBytes` texture readback conversion.
It still copies pixels to the CPU; it does not share textures directly with
the video encoder.

The optimization applies only to single-mip, single-layer 2D RGBA8/BGRA8
textures with both `READ_BACK` and `BLIT_DST`, without render-target,
compute-write, or write-only usage. Other formats/layouts, allocation failures,
older OS versions, and Macs without unified memory retain the original path.
The buffer uses the device's required linear texture row alignment and cached
shared storage. The texture retains its backing buffer, so the existing
texture destruction and resize lifecycle also releases that storage.

See Apple's [buffer-backed texture documentation](https://developer.apple.com/documentation/metal/mtlbuffer/maketexture(descriptor:offset:bytesperrow:))
for the shared allocation and alignment requirements.

`cmake/BgfxMetalReadback.cmake` patches a generated copy of bgfx's Metal
implementation. The desktop build and iOS compatibility hook apply the same
patch. The pinned submodule is unchanged, and the existing iOS 16 shader
reflection workaround remains in place.

## Verification

```sh
cmake --build cmake-build-debug --target bgfx_metal_readback_tests -j 6
ctest --test-dir cmake-build-debug -R '^bgfx_metal_readback_tests$' --output-on-failure
python3 tests/ios_build_setup_tests.py
scripts/ios_firebase_deploy.sh --build-only
```

The real Metal test verifies changing pixels across 24 frames, channel order,
odd row widths, output bounds, mipmap fallback, external texture handle reuse,
and destruction/recreation for
both RGBA8 and BGRA8. It checks that eligible textures actually have a backing
buffer on unified-memory Macs. The iOS build test also checks that both Metal
patches reach the generated source without modifying the submodule or
recompiling unchanged generated files.

On an M1 Pro, a standalone 2400x1080 Metal probe averaged approximately
1.8 ms for render/blit/wait/readback with the original texture and 1.1 ms
with a buffer-backed texture (160 frames, identical pixels). Turning off
`allowGPUOptimizedContents` alone instead increased the time to about 2.5 ms.

The debug bgfx integration test's frame/readback section measured 10.36 ms
before and 5.67 ms after for RGBA8, and 10.72 ms before and 5.82 ms after for
BGRA8 at 2400x1080. These are individual host runs, with uploads and frame
advancement included. They are not full replay export timings or physical
iOS device measurements; encoder and scene preparation costs remain.
