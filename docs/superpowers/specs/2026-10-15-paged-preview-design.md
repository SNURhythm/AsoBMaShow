# Direct paged chart preview

The selected chart must preview quickly without writing a mixed audio timeline to
disk. The user chose progressive startup with a mixer that consumes prepared PCM
pages. Preparation continues to completion in memory, with no eviction or
playback backpressure, so pressing Play can reuse native-rate decoded pages.
Preview starts at an arbitrary chart frame, including tails of earlier events.

## Architecture

Extend `SoundData` with optional immutable paged output PCM. A single preparation
worker publishes stable metadata and individually owned pages with release/acquire
atomics. Neither metadata nor published pages change or disappear before output
is quiesced. Missing metadata is pending; invalid audio is a known empty sound.
The existing contiguous gameplay representation remains supported.

The shared `AudioMix` kernel reads either representation. A read-only readiness
check covers every sample that an upcoming callback could use, including fractional
interpolation endpoints, active voices and scheduled events. If any needed page
is missing, preview outputs silence without consuming its schedule or advancing
voices or the chart clock. The readiness check and mixing allocate nothing, take
no locks, perform no file access and never decode or wait.

`PagedPreviewSource` snapshots chart events/assets before returning from creation.
The preview player opens its dedicated backend, supplies the actual callback rate,
and starts preparation. The source allocates schedule capacity before callbacks
can begin. A worker prepares a two-second initial range, publishes startup-ready,
then traverses the remaining timeline in short windows and fully warms all chart
assets, including referenced assets before a highlight origin and unused WAV table
entries. Native PCM is retained in the existing unlimited `DecodedAssetCache`;
output-rate per-asset pages use the exact shared rational resampler. There is no
mixed PCM spool, no mixed PCM ring and no per-frame producer/consumer refill loop.
Archive extraction may still require temporary encoded files under the existing
decoder cache contract; those are not rendered audio.

The source owns a private `AudioCallbackState` and exposes an allocation-free
stereo-float render operation using the shared mixer. Render returns the number of
frames mixed, pending or end. It publishes its consumed position atomically for
UI use. Source state queries and bounded condition-variable waits belong to the
control thread only. Preparation errors stop output and remain visible in state.

The player retains device lifecycle, output format conversion/clipping, RMS,
spectrum and presentation-tail handling. Startup waits on source progress;
afterwards its control worker checks errors and route changes at a low frequency,
not to produce or copy PCM. Stop cancels preparation, joins workers, quiesces the
backend, and only then destroys schedule owners/pages. Natural EOF stops the
device after presentation but does not cancel remaining cache warming.

## Validation

Compare page-backed and contiguous mixing across page boundaries, mono/stereo,
overlapping voices, playback rates and missing-page resume. Verify readiness does
not mutate callback state or allocate. Compare resampled pages with full resampling
at 44.1/48 kHz; test delayed preparation, highlight tails, invalid/missing assets,
partial-cache handoff, complete warming, cancellation and no mixed output files.
Player fake-backend tests cover startup, underflow clock, gains/clipping, EOF tail,
errors, stop and rate changes. Run the desktop build and parallel CTest suite.
No app, emulator, simulator, deployment or persistent service launch is authorized.
