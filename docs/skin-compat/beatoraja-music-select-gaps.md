# Beatoraja Music-Select Skin Compatibility Gap Report

Compatibility authority: Beatoraja commit
`c2ed5db1a46145ed10790c3872f717e95b59db9d` at
`/Users/xf/workspace/SNURhythm/beatoraja`.

Scope: `SkinType.MUSIC_SELECT` (type 5) `.luaskin` support routed through the
new `MusicSelectScene`, as designed in
`docs/superpowers/specs/2026-09-01-beatoraja-lua-music-select-design.md`.

Last updated: 2026-09-05.

## Verdict

The engine-level type-5 surface is in place and ledger-wide. The
`beatoraja-music-select-feature-ledger-v1.json` classifies all 1,019 source
rows `implemented` or source-defined no-op, and the plan checkboxes are
stale (the runtime/app-flow/type5-core plans still list most steps as open
even though the code shipped). The genuine remaining gaps are concentrated in
the **sensory surface** the design's "complete support" requires but no ledger
row currently encodes:

1. **Music-select skin sources that are videos decode as movies** (VERIFIED
   COVERED, see gap #1).
2. **No sound effects** play for select actions (**FIXED**: wired through
   `SkinSystemSoundService` in `feature/music-select-gaps`; all select SEs now
   resolve across a user-configurable sound-set folder in every Beatoraja
   extension — see gap #3).
3. **No default select BGM / decide sound** (**ADDRESSED**: a looping `SELECT`
   default and `DECIDE` on launch are wired; bundled `assets/select.wav` and
   `assets/decide.wav` ship in the repo, synthesized by
   `scripts/generate_select_sounds.py`, and a user-configured sound-set folder
   is searched first for `select.*`/`decide.*` — see gap #4).
4. **No chart background (BGA) compositing** behind the skinned selector
   (open).
5. A handful of **source input/event branches** are unmapped — mostly closed:
   `Num6` → Settings lands here, while the open skin-config key remains (see
   gap #8).

None of these are covered by a ledger row, so the ledger gate cannot see them.
The remaining work belongs in the existing runtime plan (`2026-09-01-beatoraja-music-select-runtime.md`,
Task 6 is the natural home for audio), plus one new movie-decode slice and one
SE-wiring slice.

## Root cause

The type-5 work prioritized the engine spine — two-pass Lua loading, canonical
model, resource planning, bar renderer, state bridge, controller, bar manager,
event/property factories, launch flow, toolbar — and the re-usable
media plumbing that already exists for gameplay/result skins
(`SkinMovieCatalog`, `VideoPlayerSkinMovieDevice`, the skin audio backends).
Connecting that plumbing to the type-5 pipeline was left behind:

- the JSON/Lua decoders never emit `SkinMovieResource` (now covered for select
  by shared movie-extension promotion — see gap #1);
- the scene never routes SE events, scratch ticks, `OPTION_*`, `FOLDER_*`, or
  `SELECT`/`DECIDE` BGM to any audio boundary;
- the preview service is a standalone worker that plays preview files only.

## Gap inventory

| # | Area | App today | Pinned Beatoraja | Planned fix location |
|---|------|-----------|------------------|----------------------|
| 1 | Movie sources in type-5 skins | VERIFIED COVERED: shared `SkinMovieCatalog::resolveMovies` promotes a movie-extension image resource to `SkinMovieResource`; `SkinResourceCatalog` skips movie paths at image-decode time | `JSONSkinLoader` treats `image` paths whose extension is a movie extension as movies | Covered by shared promotion, proven by `tests/skin_movie_catalog_types_tests` |
| 2 | Chart background (BGA) on select | Only setting mutation for play | Select scene composites its media behind the skin | `MusicSelectScene::renderScene` + compositor |
| 3 | Option/scratch/folder SE | WIRED: `OptionChangeSound`/`ScratchSound`/folder open-close play through `SkinSystemSoundService`; every select SE resolves across the user-configured sound-set folder (all four Beatoraja extensions) then the bundled root | `OPTION_*`, `SCRATCH`, `FOLDER_OPEN/CLOSE` play system sounds | `MusicSelectScene` audio wiring |
| 4 | Select BGM + decide sound | ADDRESSED: looping SELECT default in the preview service, `DECIDE` on launch; both resolve across the user-configured sound-set folder (all four Beatoraja extensions) | `SELECT` looped, `DECIDE` on start | Preview service default path |
| 5 | Preview without `#PREVIEW` | ADDRESSED: falls back to `SELECT` (resolved through the configured sound-set folder) | Fades back to `SELECT` | See #4 |
| 6 | `SongPreview` config | Gap (no user setting): previews always loop, gated only by `archiveChartPreviewEnabled` | `NONE`/`ONCE`/`LOOP` | Settings + preview service |
| 7 | `Num6` | ADDRESSED: `Num6` in the control-key set, `SDLK_6` bound, routes to Settings | Opens CONFIG | `controlKey()` + control-key set + `OpenSettings` action |
| 8 | Open skin-config key | Unmapped | Opens `SKINCONFIG` | Input binding |
| 9 | `keyconfig` vs `skinconfig` events | ADDRESSED: both open Settings, kept as separate controller cases | Distinct destinations (13/14) | Event controller + scene |

## Detail

### 1. Movie sources in type-5 skins don't decode as movies

VERIFIED COVERED by `tests/skin_movie_catalog_types_tests`.

The renderer and catalog already support select-skin movies end to end:

- `src/skin/beatoraja/Skin2DRenderer.cpp:771` `selectMovieTime()` and its use at
  `:5741` pick the movie frame from the visual clock;
- `src/skin/beatoraja/MusicSelectSkinSession.cpp:518-530` prepares a
  `SkinMovieCatalog` with `createSkinMovieDevice()` for the select session;
- `src/skin/beatoraja/Skin2DRenderer.cpp:4511-4514` resolves a prepared movie
  for an authored image state.

The shared `SkinMovieCatalog::resolveMovies` (`SkinMovieCatalog.cpp:132`)
promotes a `SkinImageResource` to a `SkinMovieResource` by extension
(`skinResourcePathIsMovie`) regardless of which decoder produced it, so the
JSON/Lua decoders never need to emit `SkinMovieResource` themselves.
`SkinResourceCatalog` skips movie paths at image-decode time
(`SkinResourceCatalog.cpp:2792,3248`), so the same resource is not also
decoded as a static image. The test proves that a movie-extension image
resource promotes to a prepared movie that loads exactly once and participates
in prepare/commit/submit, that two image resources sharing one movie path
materialize as a single player, and that still-extension image resources stay
images. This also covers the user-visible "background movies not rendering"
report for skins whose background `image` is a video, matching the pinned
`JSONSkinLoader` extension-based decision.

### 2. No chart BGA compositing on the select scene

`MusicSelectScene::renderScene()` (`src/scene/MusicSelectScene.cpp:2637-2649`)
renders only the skin session. The `bga`/`bgaexpand` events are wired to
settings that only affect *gameplay* presentation:

- `MusicSelectEventController.cpp:253-274` mutates `skinBgaMode`/`bgaEnabled`/
  `bgaDisplayMode`;
- `MusicSelectScene.cpp:2541-2543` `ApplyBgaEnabled` calls
  `context.jukebox.setVisualsEnabled(...)`.

The select scene never composites the selected chart's background video or
image behind (or above) the skin. Pinned `MusicSelector` presents chart media
on the select screen; the design lists "BGA" among select state the bridge must
consume. Decide whether select shows chart BGA at all (Beatoraja keeps most
skin layout over a songlist/background asset; the chart's own BGA is mainly a
gameplay media) and, if so, run it through the existing BGA views.

### 3. Select sound effects

**FIXED/WIRED.** `MusicSelectScene` now owns a `SkinSystemSoundService`
(`src/audio/SkinSystemSoundService.{h,cpp}`) created in `init()`. The
`OptionChangeSound` effect plays the `OPTION_CHANGE` sound, the
`ScratchSound` input action plays `SCRATCH`, a successful `openDirectory`
plays `FOLDER_OPEN`, and a successful `closeDirectory` plays `FOLDER_CLOSE`.
Playback goes through the same `AudioWrapper` skin-sound APIs as the preview
service (`loadSkinSound` + `playSkinSound`); the service resolves each intent to
its pinned Beatoraja filename across the search roots, tried in order — the
user-configured sound-set folder (when set) then the bundled `assets/` root —
and within a root tries Beatoraja's full extension order `.wav, .flac, .ogg,
.mp3` (`AudioDriver.getPaths`). The first existing file wins; a file missing
everywhere is skipped with a warning, so the scene never fails. Routing and
asset resolution are proven at the service level by
`tests/music_select_system_sound_tests.cpp`; the scene wiring is wired but not
scene-level tested because `MusicSelectScene` is not constructible in the unit
suite. `OPTION_OPEN` /
`OPTION_CLOSE` service entry points exist but the panel open-close wiring is
left to the pending input-branch closure.

The controller and input processor *emit* the source SE intents, and the scene
dispatches them to the service:

- `MusicSelectEventController.cpp:55-64` `changedWithSound()` /
  `refreshWithSound()` append `Effect::OptionChangeSound`, used by nearly every
  event (`10/11/12`, `40/42/43/54/55`, `72/73/74/75`, `77/78/79`, `308`,
  `321-324`, `330-332`, `341-344`, `350-353`, `360-361`, `400`);
  `MusicSelectScene::executeEvent` routes the kind to
  `systemSound_->playOptionChange()` (`MusicSelectScene.cpp:2575`).
- `MusicSelectEventEffectKind::OptionChangeSound` is the *only* sound effect
  kind (`MusicSelectEventController.h:9-26`).
- `MusicSelectInputProcessor.cpp:233,238,350,356` emit
  `MusicSelectInputActionKind::ScratchSound`; `MusicSelectScene::applyInputAction`
  routes it to `playScratch()` (`MusicSelectScene.cpp:2426`).
- A successful `openDirectory` plays `FOLDER_OPEN`
  (`MusicSelectScene.cpp:1248`) and a successful `closeDirectory` plays
  `FOLDER_CLOSE` (`MusicSelectScene.cpp:1617`).

Pinned source plays:
- `OPTION_CHANGE` on every option/configuration change and on replay-slot
  cycling — `EventFactory.java` (multiple) and `MusicSelectCommand.java:42,54`;
- `OPTION_OPEN`/`OPTION_CLOSE` on panel/option open-close —
  `MusicSelectInputProcessor.java:105,121,211`;
- `SCRATCH` on scratch input — `MusicSelectInputProcessor.java:196,201`;
- `FOLDER_OPEN` on enter — `MusicSelector.select()`,
  `MusicSelectCommand.java:136,141`, `MusicSelectInputProcessor.java:311`;
- `FOLDER_CLOSE` on exit — `BarManager.java:495`;
- `GUIDESE_*` when `config.isGuidese()` — the app exposes the persisted
  `guideSoundEffects` boolean but never plays its SEs.

The full source SE set lives in `SystemSoundManager.SoundType`
(`SystemSoundManager.java:129-151`): `SCRATCH`, `FOLDER_OPEN`, `FOLDER_CLOSE`,
`OPTION_CHANGE`, `OPTION_OPEN`, `OPTION_CLOSE`, `PLAY_READY`, `PLAY_STOP`,
`RESULT_*`, `COURSE_*`, `GUIDESE_*`, `SELECT`, `DECIDE`. A select scene only
needs the select subset.

The adapter above lands the select subset; `GUIDESE_*` remains open: play the
`GUIDESE_*` system sounds when the selected chart's guide is on and
`guideSoundEffects` is enabled.

### 4. No default select BGM, and no decide sound

**FIXED/WIRED.** `MusicSelectPreviewAudioService` now takes an injectable
`AudioPort` (play/stop) plus a default path and keeps the looping `SELECT`
system sound as its fallback: the worker starts it when the select screen opens
(`PreviewThread` plays the default on thread start,
`PreviewMusicProcessor.java:79-81`) and an empty preview switch routes back to
it. `MusicSelectScene::launchSelected` plays `DECIDE` through
`SkinSystemSoundService`. The scene wires the
AudioWrapper-backed port (`MusicSelectPreviewBgmPlayer` in
`MusicSelectScene.cpp`), which caches the default handle so returning to the
select BGM is instant. The default select BGM and the `DECIDE` sound resolve
through the same search as the SEs — `musicSelectSystemSoundPath` checks the
user-configured sound-set folder (when set) first, then the bundled `assets/`
root, in Beatoraja extension order `.wav, .flac, .ogg, .mp3` — so a set shipping
`select.ogg`/`decide.flac` (e.g. ModernChic's `Sound/`) is honored. The folder
is configured in the Music Select settings section: on iOS/Android a native
**Pick...** button launches the platform folder picker (off the main thread)
and persists the picked path plus its access token (an iOS security-scoped
bookmark / Android SAF tree URI in `skinSelectSoundSetBookmark`) so the folder
is re-accessible after relaunch; the row keeps a typed path that works on every
platform, so desktop/mac keeps the typed input as its only affordance. A stock
checkout now also has bundled defaults: the repo ships
`assets/select.wav` and `assets/decide.wav` (plus all eight select SEs) generated
deterministically by `scripts/generate_select_sounds.py` (pure-Python DSP, no
third-party deps, select is a seamless 2.0 s loop), so the default load never
fails on a fresh install. Bundled and user sound-set assets are now loaded via a
**bundle-aware read** (`decodeSkinSoundBundleAware`: `AudioWrapper::loadSkinSound`
→ byte read through `SDL_RWFromFile`, which resolves relative `assets/*.wav`
paths against the app bundle on iOS/macOS, decoded from memory, falling back to
the plain `sf_open` path for archives and absolute-path user files), so the
default sounds play inside the iOS sandbox and inside a macOS `.app` bundle
instead of only from the repo working copy. This is a deploy-time fix for iOS:
an existing iPad install must be rebuilt/reinstalled to pick up the bundled
sounds. Pause and
error/teardown silence preview audio explicitly
(`MusicSelectPreviewAudioService::silence`, used by
`MusicSelectScene::onPause`/`enterError`) instead of resuming the select BGM;
only folder navigation away from a song returns to the looping default (pinned
Beatoraja keeps the SELECT sound running across folder moves).

### 5. Preview with no `#PREVIEW` is silent (Beatoraja fades to select BGM)

**ADDRESSED.** `MusicSelectPreviewController::update` returns `nullopt` for an
empty preview path (`MusicSelectPreview.cpp:44-47`), and the preview service now
routes a `nullopt` switch to its looping default select BGM
(`MusicSelectPreviewAudioService.cpp`). The pinned fade-back over ~150 ms is
approximated by a simple switch to the default (the worker is one
request-serial + cancellation thread; `AudioWrapper` has no per-skin-sound
volume/fade helper), matching the ruling. With gap #4 fixed, empty preview
naturally routes to `SELECT`, which itself resolves across the configured
sound-set folder (and all four Beatoraja extensions) before the bundled root —
so a set that ships its select BGM as `select.ogg` is what the fallback plays.

### 6. `SongPreview` mode is not modeled

Beatoraja's `Config.SongPreview` is `NONE`/`ONCE`/`LOOP`
(`MusicSelector.java:204`) and the preview volume follows
`AudioConfig.systemvolume`. AsoBMaShow continues to loop previews at gain `1.0`
(`MusicSelectPreviewAudioService` passes `loop=true`), and models no new
setting: previews are gated only by the existing `archiveChartPreviewEnabled`
(which suppresses archive-chart previews and therefore defers those charts to
the select BGM), matching the task ruling. `NONE`/`ONCE` and `systemvolume`
honoring remain the un-surfaced part of this gap.

### 7. `Num6` is unmapped

**ADDRESSED.** `MusicSelectControlKey` now has `Num6`
(`src/music_select/MusicSelectInputProcessor.h:55-71`) and
`MusicSelectScene::controlKey()` binds `SDLK_6`
(`MusicSelectScene.cpp:371-392`). `MusicSelectInputProcessor::process` routes a
pressed `Num6` to a new `MusicSelectInputActionKind::OpenSettings`
(`MusicSelectInputProcessor.cpp:144-150`), and
`MusicSelectScene::applyInputAction` dispatches that action to the scene's
`openSettings()` — the same Settings entry the toolbar uses
(`MusicSelectScene.cpp:3641-3646`). This matches pinned `MusicSelector.input()`
mapping `NUM6` to the CONFIG screen (`MusicSelector.java:296-300`). The
input-processor routing is unit-tested by
`tests/music_select_input_processor_tests.cpp`
(`testNum6ControlKeyOpensSettings`); the scene-level dispatch
(`applyInputAction` → `openSettings`) is wired but not scene-level tested
because `MusicSelectScene` is not constructible in the unit suite.

### 8. Open skin-configuration key is unhandled

Pinned `MusicSelector.input()` opens `SKINCONFIG` on
`KeyCommand.OPEN_SKIN_CONFIGURATION` (`MusicSelector.java:298-300`).
AsoBMaShow's select scene binds no skin-config key and has no skin-config
destination; the `MusicSelectCommandKey` set
(`MusicSelectInputProcessor.h:73-82`) does not include it either. Beatoraja's
skin-configuration screen mutates the loaded `.luaskin` configuration without
re-activating the skin; the app has the configuration pipeline, but not the
trigger.

### 9. `keyconfig` vs `skinconfig` events collapse to Settings

**ADDRESSED with a recorded non-goal.** Pinned `EventType` distinguishes
`keyconfig` (13) and `skinconfig` (14) (`EventFactory.java`). AsoBMaShow has no
key-config surface (checked: nothing under `src/scene/` or `src/view/` provides
a key-config screen), so per the Task 4 ruling both events keep routing to the
Settings destination, but they remain **structurally separate `case`s** in
`MusicSelectEventController::execute` (`MusicSelectEventController.cpp:185-190`):
`case 14` keeps the one-line `effect(OpenSettings)`, and `case 13` carries a
comment reserving the slot for a future key-config surface. The controller test
asserts each event still emits exactly the settings-open effect
(`music_select_event_controller_tests.cpp:82-92`).

## Already covered (not gaps)

For calibration, these are implemented and ledger/evidence-backed:

- Type-5 header, two-pass configured Lua loading, target acquisition,
  built-in `SkinType 5` trait, `MusicSelectLaunchPolicy` dispatch
  (`src/music_select/MusicSelectLaunchPolicy.cpp`).
- `JsonSkin.SongList` decoding and `MusicSelectBarRenderer` bar-family drawing
  (`src/skin/beatoraja/MusicSelectBarRenderer.*`).
- Bar classes, movement, filters, sorting, directory stack, same-folder,
  favorites, search (`MusicSelectBarManager`, `MusicSelectRepositoryProjection`).
- Preview **audio** for charts declaring `#PREVIEW` with the 400 ms
  `TIMER_SONGBAR_CHANGE` delay and folder-stop rule
  (`MusicSelectPreviewController`, `music_select_preview_tests.cpp`).
- Selected-chart **banner/stagefile** static artwork (references `100`/`102`
  patched per selection via `musicSelectBuiltinImagePaths`,
  `MusicSelectSkinSession.cpp:124-130`) and `stageFile`/`banner` bridge
  booleans.
- IR ranking, replay slots, note-graph analysis (`updateSelectedChartAnalysis`),
  and the `bga`/`bgaexpand` **settings** projection (for gameplay).
- Toolbar (expanded/collapsed/hidden), search prompt, error screen, Settings
  origin, and no-return-to-Intro policy.
- Events `10-19, 40-42, 54-55, 72-75, 77-79, 89-90, 210-213, 308, 312,
  315-318, 321-324, 330-332, 340-344, 350-353, 360-361, 400` per the pinned
  `EventType` count, routed to their select SEs (see gap #3).

## Remediation roadmap

Proposed slices, each coherent test+ledger unit in the style of the existing
plans. None requires a whole-file formatter; media paths already exist and are
reused.

1. **Movie decode for JSON/Lua select sources** — covered: shared
   `SkinMovieCatalog::resolveMovies` promotes movie-extension image resources
   to `SkinMovieResource`; proven by `tests/skin_movie_catalog_types_tests`
   (type-5 movie fixture + `SkinMovieCatalog` assertion: a movie source is
   decoded, prepared, and drawn).
2. **Select SE wiring** — done: the `SkinSystemSoundService` adapter plays
   `OptionChangeSound`, `ScratchSound`, and `FolderOpenSound`/`FolderCloseSound`
   through the preview service's `AudioWrapper` skin-sound path. Remaining from
   this slice: `OptionOpenSound`/`OptionCloseSound` panel wiring and `GUIDESE_*`
   from the persisted `guideSoundEffects` setting (see gap #3).
3. **Default select BGM + preview parity** — done: `SELECT` default in the
   preview service (injectable `AudioPort` + default path), fallback to the
   default on empty preview, `DECIDE` on launch. Remaining from this slice:
   `SongPreview` `NONE/ONCE/LOOP` + `systemvolume` honoring (see gap #6).
4. **Input/event branches** — done: `Num6` (`SDLK_6`) → Settings; the
   `keyconfig`/`skinconfig` split recorded as separate Settings-routed cases.
   Remaining from this slice: gap #8, the open skin-configuration key
   (`OPEN_SKIN_CONFIGURATION` → `SKINCONFIG`), is deliberately out of scope
   (it needs a skin-config destination and re-activation semantics).
5. **Select BGA decision** — either composite chart media via the existing BGA
   views or mark chart-BGA-during-select a documented non-goal (matches the
   design's "BGA" ambiguity).
6. **Ledger coverage** — add rows for the select SE/movie/BGM surface so CI
   cannot regress it, and refresh the stale plan checkboxes in the three
   music-select plans (`type5-core`, `runtime`, `app-flow-toolbar`) so open
   steps reflect real outstanding work.

## Non-scope

- The native `DecideLoadingOverlay` and the separate Beatoraja `DECIDE` skin
  (type 6 / decide.json) are a distinct skin type and remain out of scope for
  type-5 select work; only the `DECIDE` *sound* on launch is called out above.
- No third-party skin defines behavior; the above is pinned to commit
  `c2ed5db1a46145ed10790c3872f717e95b59db9d`.