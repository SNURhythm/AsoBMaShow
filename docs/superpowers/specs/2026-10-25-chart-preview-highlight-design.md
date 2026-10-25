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

Audio analysis was considered but would delay browsing and read archive payloads.
A fixed percentage would be cheap but ignore musical structure. Timing activity
is the implementation choice because it uses already parsed chart metadata.

Reuse paged preview seeking and absolute visual synchronization. Fade only actual
rendered frames, at the device sample rate, after mix gain and before PCM clipping.
Gameplay keeps its existing path. Custom selectors reuse their preloaded chart
and native decoded PCM for synthesized preview playback; lifecycle boundaries
cancel and drain the preview worker before handing audio to gameplay.
No dependencies or parser changes are needed.
