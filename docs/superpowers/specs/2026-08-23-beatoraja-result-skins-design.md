# Beatoraja result and course-result skins

## Goal

Make Beatoraja music-result (`SkinType.RESULT`, type `7`) and course-result
(`SkinType.COURSE_RESULT`, type `15`) skins selectable and renderable in
AsoBMaShow. The local Beatoraja checkout at
`/Users/xf/workspace/SNURhythm/beatoraja` is the compatibility authority.
This design was examined against commit `c2ed5db1`.

The feature follows the same lifecycle, selection, configuration, failure,
and fallback rules as existing gameplay skins. A deliberate built-in selection
uses AsoBMaShow's native result layout; a selected skin that cannot activate is
reported as a selected-skin failure and is not silently replaced.

## Scope

Support Beatoraja result types 7 and 15 for the same document formats already
accepted for gameplay skins: `.luaskin`, `.json`, and `.lr2skin`.

The reader must follow Beatoraja's format and type rules. Structural and
resource-safety checks remain, but gameplay-keymode admission and other
gameplay-only restrictions must not reject a type 7 or 15 document. Unsupported
application capabilities must not turn an otherwise valid result skin into a
catalog-validation failure; they are runtime diagnostics at the relevant
capability boundary.

## Settings and persisted selection

Rename the user-facing Gameplay Skins tab to **Skins** and generalize its
controller/presentation terminology. Replace gameplay-only traits with target
traits keyed by Beatoraja skin type:

| Target | Beatoraja type |
| --- | ---: |
| Existing gameplay key-mode targets | 0, 1, 2, 3, 4, 16, 17 |
| Result | 7 |
| Course Result | 15 |

The traits card displays every target and exposes the existing package,
configuration, revalidation, and safety controls for its selected entry.
Profile settings migrate the current `selectedGameplayEntries` map to a
neutral type-keyed selection map without losing existing gameplay selections.
Read compatibility for legacy settings is retained; new writes use the neutral
representation.

Activation likewise becomes target-aware: game play requests its existing
key-mode-derived target, and `ResultScene` requests type 7 for a music result
or type 15 for a final course result.

## Shared pipeline and result session

Keep one reusable pipeline for package reads, configuration reconciliation,
document loading, resource preparation, texture ownership, destination
evaluation, 2D rendering, revision leases, diagnostics, and configuration
writes. Rename or extract only the interfaces that are actually target-neutral;
do not duplicate the static decoders or renderer.

Add a result session/factory adjacent to the gameplay session boundary. It owns
the selected activation, validated model, configured Lua runtime when needed,
prepared resources, and release lifecycle. It receives an authoritative
per-frame result snapshot from `ResultScene` and submits the authored skin via
the shared renderer.

`ResultScene` continues to own result persistence, course progression, remote
results, ranking UI, and scene transitions. It chooses built-in or selected
skin before layout construction and keeps application-only diagnostics as modal
overlays above the selected skin when they require user action. Ordinary native
result controls are not layered over an active selected skin.

## Result bridge and actions

Implement `ResultSkinStateBridge` as an `ISkinFrameState`-compatible source for
the shared renderer. Its mapping is defined from Beatoraja's result property,
timer, and event factories, using AsoBMaShow's existing `RhythmState`,
`ChartMeta`, `ResultPresentationModel`, timing analytics, gauge history, and
course-session data. It supplies result values, rank/clear predicates,
judgement totals and early/late values, score/max-score/rates, chart metadata,
and result graph inputs without synthesizing unrelated gameplay state.

Existing model graph objects remain shared: gauge, note-distribution, BPM, and
timing-distribution graphs use the result bridge's authoritative data.

Map authored result events to their AsoBMaShow equivalents: exit/back,
retry/replay, rankings, and course continuation where applicable. App-only
persistence and IR error details remain native modal controls. Do not reject a
skin during catalog validation because an application-only action is not
available for a particular result context; surface a contextual runtime
diagnostic instead.

## Validation and testing

Add source-backed fixtures for types 7 and 15 in Lua, JSON, and LR2 forms.
Tests cover:

- header/catalog admission for types 7 and 15 without gameplay-keymode gates;
- decoding and canonical model parity for result-only graph and timing fields;
- selector mappings for local, remote, and course result data;
- result event routing and unavailable-context behavior;
- profile migration plus all-target settings-card rendering and selection;
- deliberate built-in, ready selected, and selected-failure lifecycle paths;
- resource/session teardown and diagnostics.

Run focused tests during development, then the existing CTest suite in
`cmake-build-debug` and the desktop `main` build. No whole-file formatting is
used.
