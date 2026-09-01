# Beatoraja Lua music-select skins

## Goal

Add complete Beatoraja Lua music-select skin support to AsoBMaShow. A selected
type-5 skin runs in a dedicated music-select scene; an explicit **Built-in**
selection runs the existing native main menu.

The local Beatoraja checkout at
`/Users/xf/workspace/SNURhythm/beatoraja` is the only compatibility authority.
This design is pinned to commit
`c2ed5db1a46145ed10790c3872f717e95b59db9d`. Third-party skins are acceptance
specimens only and cannot define behavior.

The work also removes application-owned chart-library work from
`MainMenuScene`, so the native and skinned selectors share one scanner and task
service. Scanning continues on both selector screens and pauses only during
gameplay, preserving the current application policy.

## User-visible application flow

Startup opens a new Intro scene containing the **AsoBMaShow** title and two
actions: **Start** and **Settings**.

Start resolves the persisted music-select skin choice:

| Music-select choice | Destination |
| --- | --- |
| Built-in | Existing native `MainMenuScene` |
| Selected, successfully activated type-5 Lua skin | New `MusicSelectScene` |
| Selected skin activation or runtime failure | Music-select skin error scene |

**Built-in** is a real item in the same target-aware skin list used for other
skin targets. It is the default when no type-5 selection is stored. A selected
skin failure never changes the stored selection and never falls back to
Built-in.

Once either selector is entered, the application has no route back to Intro.
The native selector does not gain one, and the skinned selector's Beatoraja
exit action remains an application-exit action rather than an Intro action.

Settings navigation carries an explicit origin:

| Settings origin | Back destination |
| --- | --- |
| Intro | Intro |
| Music-select skin error | Intro |
| Native main menu | The native main menu |
| Skinned music selector | The same live, paused skinned selector |

The error screen has both **Back** and **Settings**. Back returns to Intro.
Settings opened from the error screen also returns to Intro.

## Scope and source authority

The new catalog surface is Beatoraja `SkinType.MUSIC_SELECT`, numeric type `5`,
for `.luaskin` entries. Beatoraja's bundled `skin/default/select.json` is a
schema, object, and rendering oracle for the common JSON-shaped model returned
by Lua; this feature does not newly advertise JSON or LR2 music-select entries
in AsoBMaShow's skin list.

Complete support means every type-5 construct reachable through the pinned
Beatoraja Lua loader and music-select runtime, including:

- header loading, configured second execution, option/file/property export,
  returned-table conversion, and resource lookup;
- generic skin objects and destinations accepted by the selected loader;
- the `songlist` object and all nested images, text, numbers, lamps, trophies,
  labels, and negative distribution graph declarations;
- every music-selector branch in property, timer, event, and writer factories;
- every bar type and all selection, folder, course, score, rival, ranking,
  replay, preview, and option state those branches consume;
- mouse, keyboard, and controller behavior routed through the music-select input
  processor and key-property definitions;
- Lua `main_state`, timer utility, and pinned legacy host operations available to
  valid type-5 Lua skins.

A dependency required to reproduce an admitted type-5 construct remains in
scope even when it lives in a shared application subsystem. There is no
"unsupported but valid" compatibility category.

The authoritative source inventory includes at least:

- `skin/lua/LuaSkinLoader.java`;
- `skin/json/JSONSkinLoader.java`, `JsonSkinObjectLoader.java`,
  `JsonSelectSkinObjectLoader.java`, and the reachable `JsonSkin` model;
- `select/MusicSelector.java`, `MusicSelectSkin.java`, `SkinBar.java`,
  `SkinDistributionGraph.java`, `BarRenderer.java`,
  `MusicSelectInputProcessor.java`, and `MusicSelectKeyProperty.java`;
- every class under `select/bar` reached by the selector;
- the property, timer, event, and writer factories reached with a
  `MusicSelector` main state;
- the Lua host/facade code reachable from a type-5 script.

This list is a routing guide. The generated source-surface ledger, not a
hand-maintained list, is the completeness authority.

## Architectural choice

Use a dedicated `MusicSelectScene` for a selected type-5 skin. Keep
`MainMenuScene` as the Built-in presentation. Both scenes consume the same
application-owned selector services and data stores.

Do not add a skinned mode to the existing large `MainMenuScene`, and do not
create a second chart scanner or task queue for the new scene. UI-neutral work
is extracted behind shared interfaces; native layout, panels, focus, and other
presentation-specific state remain in `MainMenuScene`.

The selected-skin path reuses the existing Beatoraja compatibility pipeline:
package storage, configuration reconciliation, Lua runtime, canonical model,
resource planning, destination evaluation, renderer, revision leases, and
diagnostics. Components whose names are currently gameplay-specific are
generalized only where their behavior is already target-neutral. The type-5
path does not duplicate the decoder or renderer.

## Shared selector services

Application context owns one selector-services aggregate whose lifetime spans
Intro, Settings, both selectors, utility surfaces, gameplay, results, and error
screens. It starts once after application initialization and shuts down once
with the application.

The shared boundary owns UI-neutral work currently attached to
`MainMenuScene`, including:

- chart-repository access used by the selector;
- automatic chart-library refresh and rebuild execution;
- the serialized library-task queue, worker, progress snapshots, revisions,
  checkpoint pause/resume, cancellation, and completion reporting;
- application callbacks that enqueue library work;
- immutable library revisions consumed by selector presentations;
- shared state/services needed by the music player, Tasks, and IR utilities.

Native-only view construction, filters and panels as UI, list layout, modal
layout, focus, animation, and native input handling stay in `MainMenuScene`.
The new scene owns only Beatoraja selector presentation and behavior. Moving
the worker does not rewrite unrelated native UI.

The foreground-scene pause policy remains the current policy expressed by
`Scene::pausesBackgroundTasksForPerformance()`:

- `GamePlayScene` pauses the library worker and pauseable library tasks;
- Intro, Settings, the error screen, MainMenu, MusicSelect, result screens, and
  utility surfaces do not pause them;
- leaving gameplay resumes paused work from its existing checkpoint;
- `MusicSelectScene` never reports that it pauses background work.

Tests must demonstrate that scanning advances while either selector is
foreground and stops advancing only while gameplay is foreground. Extracting
ownership must not change the existing pause, checkpoint, queue, or progress
semantics.

Both selectors observe the same published library revisions. Native MainMenu
retains its existing refresh behavior. The skinned controller rebuilds its
Beatoraja bar state from those revisions using the same authoritative
repository data; it does not maintain an independent scan result.

## Type-5 compatibility ledger

Add a machine-readable type-5 source surface pinned to the full Beatoraja
commit hash. The extractor follows the actual loader and runtime reachability
rather than searching third-party skin syntax.

The ledger enumerates:

- Lua entry/header/configuration behavior and returned schema fields;
- all reachable generic object and destination fields, defaults, and
  conversions;
- all `JsonSkin.SongList` fields and `JsonSelectSkinObjectLoader` behavior;
- `SkinBar` constants, slots, preparation, drawing, and mouse behavior;
- all reachable bar classes and their observable state;
- music-select-specific integer, float, boolean, string, timer, event, and
  writer branches;
- selector input actions and their conditions;
- type-5 Lua host calls and return behavior;
- source-defined null, ignored, default, or no-op cases.

Every ledger row maps to production behavior plus focused runnable evidence,
or to a source-proven no-op/default. CI fails if a source row is unclassified,
if the pinned source hash changes, or if evidence named by a row is missing.
An implementation blank or fallback cannot be labeled as a source no-op.

## Loading and canonical model

Add type `5` to target traits and acquisition without a gameplay-keymode gate.
The catalog's Built-in item corresponds to no selected entry. A selected entry
uses the same configured acquisition transaction and revision lease as other
skin targets.

Lua loading follows the pinned two-pass behavior: header discovery executes the
entry without exported `skin_config`; configured loading exports the selected
configuration and executes the same entry again. The returned table is decoded
through the same JSON-shaped schema that Beatoraja sends to its JSON skin
object loader.

Extend the canonical model with a song-list object that retains all authored
and source-defaulted fields from `JsonSkin.SongList`:

- `id` and `center`;
- `clickable`;
- `listoff` and `liston` destinations;
- `text`, `level`, `lamp`, `playerlamp`, `rivallamp`, `trophy`, and `label`
  destination arrays;
- the optional negative distribution `graph`.

The model and renderer preserve authored ordering and identifiers. `SkinBar`
slot counts, row wrapping, center handling, on/off selection, text selection,
lamp and rival-lamp selection, trophy/label selection, level display, graph
display, and click-to-select or close-folder behavior follow the pinned Java
implementation.

## Music-select runtime

`MusicSelectSkinSession` owns the acquired activation, configured Lua runtime,
canonical document, prepared resources, rendering lifecycle, and deterministic
teardown for one selected-skin selector lifetime.

`MusicSelectScene` owns a controller corresponding to the pinned
`MusicSelector`, `BarManager`, `BarRenderer`, and
`MusicSelectInputProcessor` responsibilities. It consumes shared library,
score, profile, rival, IR, and configuration authorities and publishes the
current Beatoraja-shaped selector state.

An immutable per-frame music-select snapshot supplies the common renderer with
all source-required values. A music-select state bridge implements the exact
property, timer, event, writer, and text/value lookups reached by type-5 skins.
The bridge does not synthesize gameplay state or use arbitrary defaults when a
source branch defines an absent/null result.

Selection movement, bar interpolation, folder open/close, course selection,
preview changes, replay availability, score/ranking changes, skin input-enable
timing, and skin-authored actions follow the pinned selector call ordering.
An action that begins gameplay uses the same chart preparation and gameplay
scene path as Built-in MainMenu, so the shared scanner pause boundary remains
`GamePlayScene`.

## Input and application toolbar

A selected skin has an application-owned floating toolbar rendered above it.
The toolbar is not present on Intro, the error screen, native MainMenu,
gameplay, results, or other non-skinned-selector screens.

The toolbar has three persisted modes:

- **Expanded:** draggable handle, Music Player, Tasks, IR Uploads, Settings,
  Collapse, and Hide;
- **Collapsed:** a small draggable handle and Expand;
- **Hidden:** no visible overlay and no hit target.

Every toolbar control uses an icon from the bundled Font Awesome 6 Free Solid
font at `assets/fonts/fa-solid-900.ttf`, through the existing `ui_icons`
helpers. The visible controls do not use text labels; their action names remain
available as accessibility labels.

Mode and position are application/device settings, not profile settings. Hide
has no gesture, edge tab, keyboard shortcut, or in-selector escape hatch. The
recovery path is to restart the application, open Settings from Intro, and
change the persisted toolbar state. The toolbar does not expose Parsing Logs,
Add/Import Folder, Refresh/Rebuild Library, or Back to Intro.

Music Player, Tasks, and IR Uploads use the shared application services and
return to the same live `MusicSelectScene`. Settings pauses the scene and also
returns to that same instance. These application surfaces block skin input
while active and do not restart the Lua runtime.

Toolbar hit testing consumes its pointer input first. Input not consumed by an
application surface is routed through the skin's mouse handling and then the
pinned music-select input behavior for keyboard and controllers. The toolbar
does not mutate Beatoraja selector state except through the four explicit
application actions.

## Validation and failure behavior

Type-5 decoding, conversion, defaults, admission, and rejection occur only at
boundaries present in the pinned Beatoraja loader, model, and skin-object
behavior. Do not add AsoBMaShow-specific range checks, fixed caps, shape rules,
consistency checks, coercions, required fields, resource preflight rejection,
or semantic validation for type-5 content. Existing validators must be bypassed
or generalized where they would impose a rule that Beatoraja type 5 does not.

Operating-system, filesystem, allocation, graphics, and other actual runtime
failures are runtime failures rather than invented skin-validation rules. They
are reported when the corresponding operation fails.

Any selected-skin failure before or during the selector transitions to a
dedicated diagnostic screen after releasing the failed session safely. The
screen reports the selected skin path, the actual failing stage, and the full
available cause chain, including Lua source location or resource path when the
underlying failure provides it. It does not manufacture speculative reasons or
run extra preflight validation.

The error screen never activates Built-in and never clears or replaces the
stored type-5 selection. Back returns to Intro. Settings opens with error
origin and also returns to Intro, allowing the user to change the selection
before pressing Start again.

## Verification and definition of done

Committed, redistributable synthetic fixtures and focused tests cover every
ledger row. The suite includes:

- pinned source hash and complete ledger classification checks;
- type-5 header, catalog, Built-in-default, configuration, and activation
  tests;
- Lua returned-table conversion and exact source defaults;
- every song-list slot and bar class, including rendering and click behavior;
- all selector-specific properties, timers, events, writers, and input paths;
- controller snapshots for folder, course, score, rival, ranking, replay,
  option, preview, and live library-revision states;
- selected-skin resource/session lifetime and Lua restart boundaries;
- Intro, Settings-origin, utility-return, error, and no-return-to-Intro
  navigation;
- explicit Built-in, successful selected skin, startup failure, and runtime
  failure paths, proving zero automatic fallback;
- expanded, collapsed, hidden, drag, persistence, and restart recovery toolbar
  behavior, including the Font Awesome icon and accessibility-label contract;
- one application-owned library worker, with continuous progress on native and
  skinned selection screens, gameplay-only pause, and checkpoint resume;
- regression coverage for native MainMenu automatic scanning and task UI.

The skins under `/Users/xf/Downloads/Skins`, including available type-5 Lua
selectors, are local acceptance specimens. They may be loaded and exercised,
but their assets are not copied into the repository and their behavior cannot
override the pinned source ledger.

Verification ends with focused tests, the parallel CTest suite in
`cmake-build-debug`, and the desktop `main` build. Relevant unsigned mobile
compile/release verification is run in proportion to touched platform code.
No distribution action is performed, and no whole-file formatter is used.

## Explicit non-goals

- No automatic fallback from a failed selected skin.
- No selector-to-Intro navigation after Start.
- No toolbar on Built-in MainMenu or non-selector scenes.
- No toolbar Parsing Logs, Add/Import, Refresh/Rebuild, or Back action.
- No hidden-toolbar recovery gesture or shortcut.
- No duplicate scanner, library repository, or task worker.
- No extra type-5 validation absent from pinned Beatoraja.
- No third-party skin as a compatibility authority.
- No newly advertised type-5 JSON or LR2 catalog support in this change.
