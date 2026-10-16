# Framework-wide idle UI scheduling

## Intent and constraints

The user wants settings, chart viewer, pause menus, and the rest of the UI to
benefit from idle optimization. The selected approach preserves immediate
drawing and existing batching; a retained drawing-cache migration is not
justified by measured costs. Existing View objects and Yoga layout remain.

- Preserve appearance, interaction, 120 Hz animation support, and 1 ms input polling.
- Apply the framework on desktop, Android, and iOS, not only the main menu.
- Keep gameplay, running previews, media, and time-driven skins continuous when required.
- Keep service processing, completion handling, and deferred navigation alive while drawing sleeps.
- Preserve native background/surface/export ownership and frame-retirement rules.
- Do not introduce per-widget display-list caches, a new UI toolkit, or a visual redesign.
- Work in the user-requested `feature/ui-idle-scheduling` branch; no worktree or deployment.
- Verify with headless tests and builds; do not launch the app or devices.

## Existing problem

The current iOS selector optimization records and compares complete frames after
the scene has updated and generated its drawing commands. Other scenes do not
opt in. Several widgets use rendering to advance momentum, evaluate animation
clocks, or accept image completions. Skipping drawing without tracking those
obligations can freeze content.

Immediate drawing itself is not assumed expensive. The target is repeated work
when there is no visual change. Actual CPU/battery savings require profiling;
the implementation must prove skipped work through deterministic call counts.

## Architecture

### Presentation demand, independent of drawing

A small platform-independent presentation scheduler tracks:

1. A thread-local visual revision, invalidated by effective UI changes.
2. A cross-thread completion revision, published without touching View objects.
3. The next animation frame or absolute redraw deadline requested while drawing.
4. Surface/scene restoration and explicit presentation requests.
5. A one-second graphics-resource maintenance deadline, retained from the existing path.

The application loop continues input and service work. Before entering scene
rendering, it asks whether a frame is needed. A settled scene without a due
deadline bypasses scene traversal, command construction, batching, and GPU
presentation. A changed frame still draws the entire scene normally.

A frame scope collects redraw requests from all render contexts, including
manually owned children and custom View drawing. Animation requests use the
existing display pacing. Timed requests use SDL's monotonic millisecond clock.
Scope state is thread-local so export rendering cannot replace owner UI demand.

Revision acknowledgment occurs only after successful presentation, using the
revision captured before drawing. Changes made during drawing remain pending
for a settling frame. Failed, discarded, or interrupted native frames cannot
consume invalidation. Scene changes and surface replacement force fresh frames.

### Explicit invalidation

View style, visibility, geometry, content, and structural mutations invalidate
presentation only when the effective value changes. Text and image mutations
also invalidate. Layout requests invalidate when Yoga is dirty; explicit layout
application compares published geometry so repeated no-op layout calls settle.

Input and window events conservatively invalidate presentation, covering custom
interaction state. Custom canvas/model setters also invalidate so asynchronous
and programmatic changes do not depend on input. Theme/language, resource and
viewport changes are included. This first implementation uses coarse revisions,
not fine-grained dependency graphs or caches.

### Animation and completion requests

- Scroll momentum requests another frame while active.
- Marquee text requests frames during motion and a deadline during stationary holds.
- Text editing requests the next caret blink boundary instead of continuous frames.
- Recycler scrollbars request frames during fades and a deadline during holds.
- Visible keybomb previews remain animated at display cadence.
- Artwork glow changes invalidate through its value setter; audio analysis/service
  processing continues independently.
- Image decode completion/failure/drop notifies the owner through a revision signal;
  workers never call or retain View pointers. Completion is applied on the normal
  owner rendering path, which is awakened by that signal.

Static preparation may still happen when a frame is drawn. This is compatible
with immediate drawing; evolving state must declare a future frame or completion
notification, rather than rely on unconditional rendering.

### Scene activity

Scenes declare whether custom drawing needs continuous presentation. Unknown or
time-driven skin rendering remains conservative. Built-in static scenes opt into
the common idle contract; settings previews, chart listening, music/video,
gameplay and pause report their actual active state.

An ordinary built-in gameplay pause can settle after transient effects. A course
menu that leaves playback running must remain continuous. Skins whose timing
contract is not known remain continuous. Custom chart/graph geometry is preserved;
it need not be converted into UI batches to benefit from pre-render idle gating.

Service polling is initially retained where needed for correctness. UI values
must avoid invalidating on equal assignments. Expensive presentation-only polling
is gated by revisions or displayed-time deadlines where the existing state allows
it; no background operation is suppressed merely because the screen is idle.

### Existing batching and lifecycle

Remove the post-render whole-frame recording/comparison path once the new gate
is integrated. Keep immediate batching and retained glow submission semantics,
including the real backend result required by CPU fallback.

Export preview requests bypass ordinary idle gating. Android suspend/resume and
native-window retirement retain their existing ordering. iOS generation changes
and discarded prepared frames invalidate the schedule. Idle iterations use the
normal pacing path, not the render-failure fallback spin. On uncapped desktop and
Android loops, a display-rate idle pacer replaces the VSync wait lost by skipping
GPU submission (120 Hz fallback when no display rate is available). Explicit
caps and active-frame pacing remain unchanged. Logical frame counters
continue advancing so next-turn deferred callbacks cannot stall.

## Validation

- Fake-clock scheduling: initial/dirty/continuous/deadline/maintenance frames,
  successful acknowledgment, mutations during draw, nested scopes, completion
  signals, failed/discarded frames and independent owner threads.
- Real View mutation tests: equal assignments stay idle; style, structure,
  visibility, geometry, text, image and custom canvas changes wake presentation.
- Animation tests: momentum ends, marquee holds, caret boundaries, scrollbar
  holds/fades, and hidden/clipped animations do not request unnecessary frames.
- Integration fixtures: settled settings/chart/pause paths stop drawing; listening,
  previews, course pause and skins continue; completions and deferred work remain live.
- Existing rendering/readback tests preserve draw order, opacity, clipping and
  retained-effect fallback behavior.
- Desktop and mobile compile checks, full parallel CTest, and diff review.

No FPS-only or synthetic timing result will be presented as a measured battery
improvement. Optional telemetry distinguishes scene update, UI drawing and
presentation so subsequent profiling can justify further optimization.
