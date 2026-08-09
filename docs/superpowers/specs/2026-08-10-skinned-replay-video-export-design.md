# Skinned Replay Video Export Design

## Goal

Render gameplay portions of normal and course replay-video exports with the
currently selected Beatoraja gameplay skin.  Export must never silently change
the requested visual output: a selected skin that is unavailable or cannot
create an export session fails before audio encoding, MP4 creation, or Photos
library work begins.  With no skin selected, export retains the established
built-in renderer behavior.

Only gameplay is in scope.  The existing built-in result and course-result
screens remain unchanged when result-screen export is enabled.

## Existing Boundaries

`ReplayVideoExporter` currently constructs and drives a `BMSRenderer` directly
in separate normal-replay and per-course-stage loops.  Gameplay instead owns a
`PlayfieldVisualStateStore`, a `PlayfieldPresentationEventFanout`, a
`PlayfieldProjection`, and a `PlayfieldPresentationCoordinator`.  The
coordinator warms the built-in renderer, optionally drives `PlaySkinSession`,
and owns the exact whole-frame BGA/skin command ordering.

`GamePlayScene::acquireGameplaySkinForAttempt` currently owns the selected-skin
lifecycle request, `PlaySkinSession::create` context, diagnostic-history
recording, and failure conversion.  That construction path must become shared;
the exporter must not copy it.

## Architecture

Introduce a narrow reusable gameplay-skin session factory that consumes:

- the existing `ApplicationContext` skin services and lifecycle acquisition
  callback;
- a keymode, immutable `PlayfieldChartVisualModel`, initial captured visual
  state/projection, and export/gameplay safe UI bounds;
- the caller's desired session serial/profile/viewport and a diagnostic sink.

It returns one of three explicit outcomes:

- **BuiltIn:** no skin is selected for the keymode;
- **Ready:** an owning `PlaySkinSession` and immutable identity;
- **Failed:** the first authoritative skin diagnostic, already recorded by the
  common diagnostic sink.

`GamePlayScene` replaces its local construction block with this factory.  The
exporter uses the same factory, preserving one source of truth for acquisition,
resource preparation, state initialization, viewport geometry, and
diagnostics.

For every exported gameplay attempt, build the same presentation graph used by
gameplay: `PlayfieldVisualStateStore`, `BMSRenderer` as the built-in
presentation, `PlayfieldPresentationEventFanout`, `PlayfieldProjection`, and
`PlayfieldPresentationCoordinator`.  Replay event application updates the
store and forwards the same event to the coordinator.  Each video frame
captures the shared state, projects it, then calls coordinator
`prepareFrame`/`render`.  The coordinator therefore remains the sole owner of
skin evaluation, BGA command placement, embedded/fullscreen composition, and
skin failure handling.

The export adapter does not accept pointer/touch input.  It continues to
populate recorded touch/ghost state from replay data, because that state is
already part of the immutable visual snapshot.

## Preflight and Failure Semantics

Preflight occurs before audio rendering, temporary-directory work, video
encoder allocation, MP4 output creation, or Photos-library authorization.

- A normal replay constructs its one optional skin session during preflight.
- A course replay constructs and retains a session for every stage during
  preflight.  This intentionally uses bounded additional resources to ensure a
  later stage cannot fail after expensive output work has begun.
- A `BuiltIn` outcome retains existing built-in export behavior.
- A `Failed` outcome aborts the requested export with its authoritative skin
  diagnostic code/message and creates no video output.  It does not fall back
  to the built-in renderer.
- A skin failure after successful preflight similarly fails the export and
  removes temporary output, rather than emitting a mixed skin/built-in video.

Normal configuration writes and normal selected-skin filesystem semantics are
preserved by the shared `PlaySkinSession`; export does not introduce a special
overlay, filesystem restriction, or alternate Lua runtime.

## Course and Result Behavior

Course stages may have different keymodes.  Each stage resolves the selected
skin for its own keymode, and the preflight retains the corresponding session
for that exact stage.  The course result screen and normal result screen remain
built-in, because result skins are outside the supported feature set.

## Tests

Focused tests must cover:

1. no selected skin retains built-in replay export;
2. an unavailable/invalid selected skin fails before audio or MP4 work and
   exposes the original diagnostic;
3. a valid selected skin receives the same captured state, projection, replay
   events, and prepared BGA frame as the built-in presentation path;
4. a skin frame failure aborts and cleans temporary output rather than falling
   back mid-video;
5. course preflight detects an invalid later stage before any course audio/video
   output work; and
6. every normal/course gameplay frame uses the common coordinator path, while
   result frames remain built-in.

Desktop focused tests are the primary loop.  Native/session changes require the
unsigned iOS release verification script after desktop verification; no build
is deployed or uploaded.
