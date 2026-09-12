# Archive chart loading improvements

## Changes

- All chart loads now use differential resource reconciliation instead of a separate destructive archive loader. Shared decoded sounds retain their handles across sibling charts and resource reloads; removed sounds are retired. Archive identity uses the existing size/mtime-based cache key, so replacing an archive invalidates its retained sounds.
- Newly published sounds enter the live chart sound map immediately, including when a load is subsequently cancelled. This keeps partially completed loads visible to the next reconciliation rather than leaving untracked decoded resources.
- Missing sounds and all referenced images/videos share one archive extraction plan. Visual descriptors and layer-image requirements are prepared before consuming bytes. Video materialization and image loading still finish before playback; there is no event-time extraction.
- A bounded worker pipeline overlaps extraction with decoding. Encoded bytes remain charged while queued and while being consumed, then are released immediately. Normal concurrent reads have a 32 MiB extractor budget and a 32 MiB consumer budget; pending item count is also bounded. These are encoded-buffer budgets, not total process-memory limits: compressed scratch, decoder output, textures, and video state are additional. One oversized item can advance through an otherwise empty stage; serial archive backends can also hold their current output entry while waiting for admission.
- Resampling runs outside the shared sound-map mutex. Publication revalidates cancellation, successful-unload generation, duplicate identity, and authoritative output rate, retrying conversion when the rate changes.
- ZIP extraction checks cancellation during reads, inflation, and checksumming; cached 7-Zip handle acquisition is cancellable while retaining exclusive access to its shared stream. Cancellation terminates fallback chains.

## Synthetic timing check

The same debug-build probe alternates between two manually constructed chart descriptions referencing 64 shared, generated one-second stereo WAVs and one image. Audio uses the real decoder and resampler, a fake 44.1 kHz output device, and bgfx Noop. The input WAVs are 48 kHz; ZIP uses stored entries. This measures resource loading, not parser or library-scan time.

| Seven warm sibling switches | Before | After |
| --- | ---: | ---: |
| ZIP median | 252.721 ms | 1.857 ms |
| ZIP audio decodes per switch | 64 | 0 |
| Loose-file median | 3.061 ms | 3.097 ms |
| Loose-file audio decodes per switch | 0 | 0 |
| ZIP extraction plans per switch | 2 | 1 |

The first ZIP load measured 211.777 ms before and 93.266 ms after; first loose-file loads measured 149.130 ms and 56.416 ms. These are single samples, not cold-storage benchmarks. The warm medians demonstrate retained-sound reuse, not a general speedup estimate for real libraries, compressed archives, or mobile devices. The retained ZIP extraction plan on subsequent switches loads the image, not the shared audio.

## Regression coverage

- Shared decoded handles, ID aliases, newly added/removed sounds, ordinary resource reload, timestamp-only and isolated size-changing archive replacement, already-cancelled loads, and empty charts, using real ZIP and 7-Zip fixtures.
- One mixed audio/visual extraction plan, video readiness before playback, and prepared layer-image and poor-only image availability; existing visual-only tests remain applicable.
- Pipeline consumption before producer completion, backpressure with bytes charged through consumption, oversized-entry progress, cancellation of blocked producers, queued-buffer disposal, worker joining, consumer rejection, and propagation of consumer exceptions.
- Deterministic reader-adapter tests for partial delivery followed by failure, missing-only retry, repeated backend deliveries without repeated consumption, and no fallback after cancellation or consumer rejection. The default batch-loader adapters remain the real archive readers.
- Audio lookup/playback and independent loading during conversion, output-rate changes and rollback/recovery, successful versus failed unload, duplicate publication, and cancellation.
- Large ZIP-entry cancellation, long empty-deflate-block runs before and after payload output, shared 7-Zip-handle waits, and terminal cancellation across archive fallback routes. Stored/deflated integrity tests cover empty entries and chunk boundaries through all four reader APIs, including corrupt CRC rejection with and without checkpoints.

No parser, shader, deployment, or distribution behavior is changed.

## Solid 7z classification follow-up

The reported slow iOS chart launch exposed a separate classification bug: the supplied archive has one solid 7z block, but its SDK item-level `kpidSolid` values are empty. The archive-level property is true. Listing and extraction validation now use that property as the fallback, preserving explicit item-level values and querying the archive-level property once per batch rather than once per entry.

Persisted entry indexes advance to version 3. Chart database migration 11 invalidates previously non-solid `.7z`/`.cb7` scan classifications and 7z completed-scan markers, without clearing chart metadata or user records. The next library scan replaces misclassified playable charts with the existing solid-archive entry requiring unarchiving. Regression tests cover real solid and non-solid 7z files, vector/streaming extraction, old disk indexes, database upgrades, and that library transition.

Follow-up verification: the supplied archive reports 1,159 solid files and one directory; the desktop build and all 355 tests pass (88.81 seconds), as does the unsigned iOS Release build. The scoped review has no outstanding findings. No build was distributed.

## Verification

- Desktop `main` and all affected targets build successfully.
- All five focused suites pass together: archive batch orchestration, bounded pipeline, archive concurrency/integrity, Jukebox resource loading, and AudioWrapper lifecycle (8.43 seconds total).
- Independent loader/audio and archive reviews have no remaining actionable findings after follow-up fixes.
- The final synthetic probe reproduces zero sound decoding on all seven warm ZIP switches.

## Verification limitations

The first full desktop rebuild succeeds. Its full parallel CTest run passes 354 of 355 tests; `visual_catch_up` times out in the unchanged EOF/shutdown test. A stack sample shows `VideoPlayer::stopPredecoding` joining a decoder parked in its output condition-variable wait. Repeating only the unchanged `testEofAndShutdown`, without calling Jukebox, audio loading, or archive code, reproduces the timeout. The relevant VideoPlayer and fixture sources match HEAD byte-for-byte. This separate shutdown issue is not changed here.

A second full parallel run also passes 354/355: `visual_catch_up` passes, while `builtin_renderer_characterization_tests` reports a legacy-versus-captured trace mismatch. The renderer test passes immediately when rerun alone. These runs are not represented as a consistently green full suite; no renderer or VideoPlayer fix is included in this archive-loading change. The final five focused suites pass after the last loader correction.

Optional sanitizer runs are unavailable on this machine: a trivial TSan-instrumented empty program exits with signal 11; the ASan runtime stalls during dyld/malloc initialization before `main` and was terminated. Neither result is claimed as a successful sanitizer check.
