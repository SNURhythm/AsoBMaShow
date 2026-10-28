# Capture 26: active built-in play meets its targets; completion causes the large hitches

The new phase/stage recording locates the large gameplay-tagged hitches in
**post-note result preparation/persistence and the later deferred scene
transition**. None of the 6,855 retained strictly active manual-play frames
missed its request deadline or a display refresh. Normal rendering still has
headroom; this capture does not establish a saturated CPU or GPU.

## Source and cohort

User-supplied `AsoBMaShow-performance-log 26.txt`, Metal presentation timing v2,
analyzed against source `1aaf825e`. The log does not embed build/device identity.
The [derived summary](data/2026-10-28-metal-log26-summary.json) records its SHA-256,
cohorts, distributions, exceptional frames and display gaps. Private raw paths
are not committed.

The ring contains 8,192 consecutive accepted callback tokens, 14665–22856,
spanning 69.134 seconds, generation zero and zero reported dropped updates.
Earlier tokens were overwritten. All numeric values are finite; there are no
open or invalid CPU stages. Only final token 22856 lacks published CPU data.
Token continuity is not proof that every OS callback or intended refresh was
delivered. Missing display timestamps are not automatically dropped frames.

| Observed phase | Frames | Tokens |
| --- | ---: | --- |
| Active | 6,855 | 14665–21519 |
| Post-notes | 24 | 21520–21543 |
| Post-notes → ending | 1 | 21544 |
| Ending | 241 | 21545–21785 |
| Unknown / non-gameplay | 1,071 | 21786–22856 |

All observed gameplay is `manual`, with `skin_selected=0`; neither custom-skin
stage executes. Strict active selection requires `cpu_collected=1`, phase mask
exactly 4, mode manual, and zero open/invalid stage masks. This captures about
57.118 seconds of active play, not the whole song. No statistical outliers are
removed. Durations below are milliseconds; quantiles linearly interpolate at
`(n - 1) * p`. Timestamp intervals use paired nonzero values, and CPU stage
statistics require a completed scope (`count > 0`). Stages are inclusive elapsed
wall time, including possible waits, not CPU busy time.

## Active play

| Interval / stage | Median | P99 | Maximum |
| --- | ---: | ---: | ---: |
| Preparation start → present request | 1.080 | 3.044 | 4.146 |
| Scene update | 0.065 | 0.541 | 1.247 |
| Scene rendering | 0.757 | 1.309 | 2.164 |
| Snapshot capture, inside scene rendering | 0.014 | 0.031 | 0.048 |
| BGA preparation, inside scene rendering | 0.00046 | 0.00473 | 0.017 |
| Fullscreen BGA CPU submission, outside scene rendering | 0.002 | 0.007 | 0.025 |
| `bgfx::frame` CPU elapsed | 0.254 | 1.536 | 2.043 |
| Whole command-buffer GPU execution | 1.198 | 1.381 | 2.101 |

These rows must not be summed: some are nested and CPU/GPU work can overlap.
Every active frame has a display timestamp. Request-deadline margin remains
at least **4.117 ms**. All 6,854 adjacent display intervals are
8.333250–8.333708 ms; display-minus-target is −0.03246 to +0.03283 ms.
The frame with the largest preparation-to-request interval is 18275: scene
update 1.156 ms, scene rendering 1.347 ms, and frame submission 1.644 ms.
The tail is distributed across these stages rather than a large capture or
BGA-preparation spike.

Median preparation-to-display remains **16.631 ms**, with GPU-end-to-display
**14.150 ms**. That interval is largely scheduled presentation time, not GPU
execution. It is not an input-to-photon or input-to-sound measurement and does
not justify delaying preparation or changing input timestamps.

### What the small projection number does not mean

`GamePlayScene::capturePlayfieldVisualState` runs the detailed projection path
only for a selected skin. In this built-in run, it merely sets the
`useParserBackedBuiltInTraversal` descriptor inside the projection scope.
The 0.000208 ms median therefore does **not** measure built-in visible-note
traversal. Actual built-in presentation work remains inside `scene_render`.

Subtracting the completed capture, projection and BGA-preparation child
durations from each active scene-render duration leaves median/P99
**0.740/1.290 ms**, or 97.9% of aggregate scene-render time. This residual
includes built-in presentation, geometry, UI/overlays and other rendering work;
it is not a measured exclusive note-drawing or text cost. Fullscreen BGA
submission occurs after scene rendering and is not subtracted from it.

For further active-loop investigation, subdivide built-in presentation and
UI/overlay work, then separate encoding/resource work from waits inside
`bgfx::frame`. The current scopes identify those envelopes, not a proven
function-level optimization. Custom-skin replay and planner findings from the
[previous investigation](2026-10-28-active-gameplay-rendering-investigation.md)
were not exercised by this capture.

## The actual large stalls

1. **21544, post-notes → ending:** scene/gameplay update takes **64.957 ms**,
   including **63.550 ms** in `scheduleResultTransition`'s result-persistence
   scope and **1.226 ms** in realtime authority stop/transfer. Preparation to
   request totals 65.454 ms, while GPU execution is 1.130 ms. The displayed
   frame is 58.325 ms after its target, with a 66.668 ms gap from the previous
   display. This directly classifies this capture's hitch as terminal work.
   The persistence scope includes replay/result construction and settings/file
   persistence; it does not isolate a particular write, `fsync`, compression
   operation or database query. Capture 25's similar event lacked these fields;
   the result supports the earlier hypothesis without retroactively proving
   that capture's exact call stack.
2. **21785, ending:** the frame itself renders/displays on target, then
   `sceneManager.handleDeferred()` takes **401.736 ms**. Realtime stop inside
   this deferred work is only 0.000292 ms, so that measured stop call does not
   explain the long transition. Cleanup, load/resume and other deferred
   callbacks still need separate attribution.
3. **21786, first non-gameplay frame:** `bgfx::frame` takes **62.084 ms**,
   scene rendering 5.455 ms and GPU execution 6.970 ms. The observed display
   gap from 21785 to 21786 is **466.674 ms**. Both deferred transition work and
   the first following submission contribute; GPU execution alone does not
   account for this gap.
4. **22616, non-gameplay:** preparation takes **355.117 ms**, of which
   scene update accounts for 107.009 ms and scene rendering 0.151 ms.
   Approximately **247.957 ms of preparation remains outside those scopes**.
   Event handling/input dispatch and other pre-render work are possible
   locations, but there are no spans proving attribution. Frame submission
   adds 7.982 ms; GPU work is 2.890 ms. This frame has no display timestamp;
   the next observed display is 383.339 ms after frame 22615. Subsequent
   50.001/175.003 ms display gaps contain no-submission callbacks and must not
   be interpreted as continuous GPU stalls.

The strongest demonstrated responsiveness targets are terminal persistence and
deferred scene-transition work. For the user's active-rendering investigation,
the remaining measurable envelope is built-in scene rendering and submission,
with no missed active refresh in this retained window. This does not prove that
other charts, devices, skins, or earlier parts of this session are hitch-free.

## Verification

Independently checked CSV cohort counts, token continuity, finite timestamps,
stage validity, strict-active deadlines/display intervals, and the four
exceptional records against the raw log. Scope nesting and the built-in
projection branch were checked against source. This change contains analysis
and derived data only; no application build or test-suite run is required.
