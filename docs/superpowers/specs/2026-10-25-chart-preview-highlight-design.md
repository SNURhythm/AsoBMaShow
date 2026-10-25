# Chart preview highlights

Chart previews default to an estimated musical highlight, with a 300 ms fade-in.
The BMS Library settings offer Highlight, Beginning, and Preview File choices,
shared by built-in and custom selectors. Beginning preserves the existing start
and leading-silence trimming behavior. Preview File uses the existing looping
authored-file player, with a conditional “When no preview file” Highlight/Beginning
choice (default Highlight). Both absent and undecodable files use that fallback.
The user explicitly requested both selectors and the label “Preview File” rather
than naming another player.

Use parsed note and background-sound onsets, without reading media for analysis.
Score sustained activity over 12 seconds at measure boundaries, favoring the
interior of the chart and discounting isolated dense bursts. Empty, short, or
uninformative charts fall back to zero. This estimates activity, not a semantic
chorus; chart difficulty and authored keysound splits affect the estimate.

## Hybrid activity and entrance score

The user approved combining the sustained-window heuristic with Qwilight's
density-rise signal and requested equal base weights for placed and background
notes. Both count as one event; placed notes without keysounds still count,
while silent LN tails and unassigned background events do not. Keep sparse
one-second bins, cap each at 32, and take its square root.

For each existing eligible measure boundary, let P, L, R be sums over the prior
six seconds and the following two six-second halves. Keep the base score
`S = L + R + min(L, R)` and require two occupied seconds in each forward half.
Add `0.5 * max(0, min(L, R) - P) * 2 * sqrt(p * (1 - p))`, where p is the
candidate's relative position between first and last counted events. Award no
rise bonus without a complete six-second history. The bonus is at most S/6;
the coefficient is an implementation choice to keep sustained activity dominant.
Retain the 15–80% candidate bounds and the 24-second short-chart fallback.

Compared with choosing Qwilight's largest adjacent-bin rise directly, this
requires the increase to persist across the preview. Compared with the original
base score alone, a slightly quieter strong entrance can beat an established
plateau. A moving prior-window cursor keeps the calculation linear, with no
additional chart-duration-sized storage or media reads.
Score candidates at the nearest one-second bin, retaining their exact measure
time for playback. This prevents parser microsecond truncation near integral
seconds from scoring the previous bin and skipping a section entrance.

Audio analysis was considered but would delay browsing and read archive payloads.
A fixed percentage would be cheap but ignore musical structure. Timing activity
is the implementation choice because it uses already parsed chart metadata.

Reuse paged preview seeking and absolute visual synchronization. Fade only actual
rendered frames, at the device sample rate, after mix gain and before PCM clipping.
Gameplay keeps its existing path. Custom selectors reuse their preloaded chart
and native decoded PCM for synthesized preview playback; lifecycle boundaries
cancel and drain the preview worker before handing audio to gameplay.
No dependencies or parser changes are needed.

## Bounded overlap preparation follow-up

The user requested cheap audio-length probing before highlight playback and a
15-second lookback cap for durations still unknown after probing. A known or
cached longer sound must keep its true overlap. This cap applies only to unknown
past sounds, not to the duration of preview playback or future scheduled sounds.

Use bounded WAV/FLAC metadata reads and the existing conservative Vorbis probe
for small complete Ogg files, with bounded identification/final-page reads for
large Ogg files. Unsupported or malformed metadata stays unknown; do not ask a generic decoder to scan a file just to obtain a length.
Loose-file probes may seek over payloads but have bounded bytes/chunk counts.
For archives inspect only bytes already delivered by the existing batch reader.
Do not add a separate extraction pass. Reuse cached PCM before probing.

Store each asset's proven bound (or the requested 15 seconds for unknowns),
then evaluate every old onset independently. A recent instance must not keep
an older unknown-duration instance alive outside the lookback. Use the latest
past onset only to decide whether all prior instances can be deferred. Future
instances remain scheduled. Prioritize opening audio, then overlap checks,
then later sounds in next-onset order. Defer
expired native decoding behind preview-critical preparation while still warming
all referenced PCM for gameplay. Retain deferred archive bytes only under a
small fixed budget; decode immediately when that budget is full. Validate source
identity across probing/deferred decode and preserve cancellation.

Metadata probing has a small bounded cost; it is not free. It avoids delaying
first audio on irrelevant PCM decoding, but gameplay warming still eventually
decodes those assets. Compressed archives may still require extracting their
encoded entries before their metadata is available.
