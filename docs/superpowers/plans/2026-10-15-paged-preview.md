# Paged preview implementation plan

Spec: `docs/superpowers/specs/2026-10-15-paged-preview-design.md`

Global constraints: work in the current checkout/branch; no worktrees, app/device
launches or deployment; preserve local style; only root runs CMake builds (one at
a time). Commit and push completed verified work, per repository instructions.
Read relevant source before editing. Tests must exercise behavior. The controller
coordinates integration/builds and review. Do not spawn nested subagents.

### Task 1: Immutable output pages and shared mixer support

Own `src/audio/AudioMix.h`, `src/audio/AudioMix.cpp`, new
`src/audio/PagedPcmSound.h` (and cpp if warranted), and a focused test file.
Add optional `SoundData::pagedOutput` ownership. Publish stable metadata (channels,
frame count, fixed page-pointer slots) and immutable int16 output-rate pages.
Metadata/page allocation and publication are producer operations; callback reads
are lock-free and non-owning. Zero frames denotes a resolved invalid asset, null
metadata denotes pending. Published pages survive until owners are destroyed after
callbacks stop. Prefer 4096 output frames per page.

Extend activation/mixing without changing contiguous gameplay behavior. Add a
read-only `ScheduledMixReady` operation with the same scheduling time/rate/frame
inputs as `ActivateScheduledSounds`, covering existing active sounds and events
that activate in that block, including interpolation endpoints and loops. Missing
pages/metadata returns false and changes nothing. Callback operations must not
allocate, lock, decode or access files. Validate exact mixing equivalence,
mono/stereo, boundaries, fractional rates, overlapping/offset schedules, invalid
assets and missing-page resume. Provide the precise public API for Task 2. Root
will register/build tests; do not run CMake concurrently or edit CMakeLists.txt.

### Task 2: Progressive per-asset preparation and direct rendering

Own new `src/audio/PagedPreviewSource.h/.cpp` and focused tests. Consume Task 1's
API. Snapshot events through `chart_audio::CollectChartTimedAudioEvents` with bus
classification as existing preview does. Snapshot asset paths and timeline end;
do not retain the chart graph. Reuse `DecodedAssetCache` and asset extension/path
resolution for archives/Android, respecting its failure classifications.

Public API should support Create(chart, shared cache, firstFrame44100, gains),
Begin(actualOutputRate), allocation-free Render(stereo float span, frameCount),
GetState(), WaitForChange(timeout), Cancel(). Create snapshots synchronously and
Begin allocates owners/schedule capacity before worker launch; the worker finalizes
the schedule before publishing startup-ready and no callbacks run before that.
Omit past events whose tails have already ended at the origin, avoiding a burst of
useless activation work on the first callback. Render state owns its
AudioCallbackState and calls shared readiness,
activation and mixing. Use frame-based monotonic chart position; starvation never
advances. Source state reports startupReady, preparationComplete, durationKnown,
duration/position in 44100 chart frames, cancelled and error. Render reports Data,
Pending or End and a count, never allocating error strings. Exact API may be
refined with controller before Task 3.

Worker prepares a two-second startup window at arbitrary origin, including earlier
overlapping tails. Use short forward timeline windows to prepare only necessary
pages before startup; continue all the way to full preparation without waiting for
playback consumption. Inspect future metadata as reached; determine final duration
from chart timeline and asset tails. After scheduled ranges finish, warm every
remaining native/output page for all WAV entries. Missing invalid assets become
known empty; operation failures are terminal. Use shared Ratio/Cursor for exact
output-rate page conversion with seek, including right endpoint native pages.
Batch native `ReadRange` calls across contiguous missing pages within each short
window: it leases a decoder per call, so a separate call for every output page
would repeatedly initialize compressed decoders. Include `ReferencedWavTable`
entries as well as selected `WavTable` entries when warming gameplay's cache.
All publication is monotonic; worker cancellation joins off callback. Test startup
before full preparation, highlight tails, resampling, bus gains, invalid files,
partial/full cache reuse, retained full preparation and absence of mixed PCM files.

### Task 3: Preview player and menu integration

Replace PreviewStreamPlayer's music-stream source/ring/refill pipeline with the
paged source. Keep backend setting application, gains before clipping, output RMS,
spectrum, timing-aware EOF drain, error reporting and route-change rejection.
Preallocate float scratch and split unusually large callback blocks safely.
Control worker waits for startup and errors/EOF, never mixes or refills. Do not
cancel full preparation on natural EOF; explicit Stop cancels and joins source,
quiesces callbacks before releasing source, and stops backend even after error.
Update MainMenuScene and its extracted fixtures to create the paged source and
retain decoded cache handoff on Play. Remove unused preview-only mixed-source API
and branches if safely separable; keep native music streaming unchanged.
Update player tests with fake paged source or real fixtures, retain meaningful
startup/clock/gain/spectrum/EOF/error/cancellation coverage. Register all sources
and tests in CMake as needed; automatic iOS source discovery needs no new entries.
Run desktop main and affected tests, then full CTest -j6. Independent review must
check callback safety, lifetime, progressive behavior, gain parity, timeline and
no rendered preview disk output. Fix findings, commit and push.
