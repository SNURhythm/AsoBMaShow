# Custom skin loading: native image rows

Full-size custom skin images no longer pass every pixel through the generic
image reducer. RGBA CIM streams inflate directly into the destination rows;
other CIM formats convert directly into those rows. Noninterlaced 8-bit RGBA
PNGs copy their reconstructed rows directly. Resizing, Adam7 interlacing, image
limits, cancellation and malformed-stream checks retain their existing behavior.
Cache sizes and skin resource admission rules are unchanged.

Both gameplay and music select use these shared image decoders. The benchmark
now supports type-5 music-select entries through the production session path,
with a 17-chart list and a valid calendar date.

## Measurements

Baseline: `1517a9237`. Apple M1 Pro macOS host; optimized application objects
(`-O3 -DNDEBUG`) with identical dependencies and benchmark code for both variants.
Only the image-decoder object changes between the measured binaries.

| Skin / screen | Load | Before | After | Reduction |
| --- | --- | ---: | ---: | ---: |
| LITONE12 gameplay | Cold | 446.8 ms | 336.0 ms | 24.8% |
| LITONE12 gameplay | Repeated | 352.6 ms | 273.2 ms | 22.5% |
| LITONE12 music select | Cold | 306.9 ms | 253.1 ms | 17.5% |
| LITONE12 music select | Repeated | 240.8 ms | 202.6 ms | 15.9% |
| ModernChic gameplay | Cold | 577.5 ms | 440.0 ms | 23.8% |
| ModernChic gameplay | Repeated | 99.5 ms | 78.4 ms | 21.2% |
| ModernChic music select | Cold | 368.0 ms | 271.9 ms | 26.1% |
| ModernChic music select | Repeated | 205.5 ms | 142.0 ms | 30.9% |

Each cold result is the median of nine sessions, each with a fresh preparation
service. Each repeated result is the median of eight sessions after discarding
the first cache-populating session. There is one before/after process pair per
skin/screen/load combination; order is reversed for music select. No builds or
other benchmarks ran during measurement. The LITONE12 gameplay cache has an
alternating working set; retaining an even number of warm samples avoids
selecting one side of that cycle as the median.

These are headless session creation **and destruction** timings. Package copying,
snapshotting and initial validation are outside the timed interval. Texture
uploads and movie devices are fakes; audio is absent. Dependencies from the
configured debug tree are held fixed, while the application objects (including
the CIM inflater) are optimized. The results isolate this code change and do
not establish release-app screen latency or physical mobile performance.

An initial exploratory selector fixture omitted calendar properties, causing
LITONE12 to convert year-zero dates. That fixture was corrected before the
before/after comparison; its exploratory timings are not used above.

## Verification

- Full native build and **454/454 CTest tests passed** (110.95 seconds).
- Image-decoder and loading benchmark tests pass. Existing PNG tests compare
  complete pixels against stb across color/depth modes, filters, transparency,
  fragmented IDAT chunks, interlacing and resizing.
- Added multi-row RGB/RGBA CIM checks for exact pixels, alpha, row boundaries,
  default/equal/larger target dimensions and truncated trailers. Existing CIM
  tests retain coverage of all six formats, reduction and cancellation.
- All sixteen real-skin benchmark invocations publish and destroy their
  sessions successfully, with texture/movie/live-resource cleanup checks.
- Independent source review found no correctness issues.

Raw samples, build commands and reproduction scripts are in
[the evidence directory](evidence/2026-10-09-custom-skin-loading/README.md).
