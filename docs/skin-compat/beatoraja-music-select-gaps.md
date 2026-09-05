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
2. **No sound effects** play for select actions.
3. **No default select BGM / decide sound**, and the preview path diverges from
   the pinned `PreviewMusicProcessor`.
4. **No chart background (BGA) compositing** behind the skinned selector.
5. A handful of **source input/event branches** are unmapped.

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
| 3 | Option/scratch/folder SE | WIRED: `OptionChangeSound`/`ScratchSound`/folder open-close play through `SkinSystemSoundService` | `OPTION_*`, `SCRATCH`, `FOLDER_OPEN/CLOSE` play system sounds | `MusicSelectScene` audio wiring |
| 4 | Select BGM + decide sound | None | `SELECT` looped, `DECIDE` on start | Preview service default path |
| 5 | Preview without `#PREVIEW` | Plays silence | Fades back to `SELECT` | See #4 |
| 6 | `SongPreview` config | Ignored (always loop) | `NONE`/`ONCE`/`LOOP` | Settings + preview service |
| 7 | `Num6` | Unmapped | Opens CONFIG | `controlKey()` + control-key set |
| 8 | Open skin-config key | Unmapped | Opens `SKINCONFIG` | Input binding |
| 9 | `keyconfig` vs `skinconfig` events | Both open Settings | Distinct destinations (13/14) | Event controller + scene |

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

### 3. Select sound effects are structured but never played

**FIXED/WIRED.** `MusicSelectScene` now owns a `SkinSystemSoundService`
(`src/audio/SkinSystemSoundService.{h,cpp}`) created in `init()`. The
`OptionChangeSound` effect plays the `OPTION_CHANGE` sound, the
`ScratchSound` input action plays `SCRATCH`, a successful `openDirectory`
plays `FOLDER_OPEN`, and a successful `closeDirectory` plays `FOLDER_CLOSE`.
Playback goes through the same `AudioWrapper` skin-sound APIs as the preview
service (`loadSkinSound` + `playSkinSound`); the service resolves each intent
to its pinned Beatoraja filename under the injected asset root and skips (with
a warning) when a file is missing, so the scene never fails. Routing is proven
by `tests/music_select_system_sound_tests.cpp` and ledger rows
`select.sound.effect.option-change`, `select.sound.effect.scratch`,
`select.sound.folder-open`, `select.sound.folder-close`. `OPTION_OPEN` /
`OPTION_CLOSE` service entry points exist but the panel open-close wiring is
left to the pending input-branch closure.

The controller and input processor *emit* the source SE intents; the scene
drops them all.

- `MusicSelectEventController.cpp:55-64` `changedWithSound()` /
  `refreshWithSound()` append `Effect::OptionChangeSound`, used by nearly every
  event (`10/11/12`, `40/42/43/54/55`, `72/73/74/75`, `77/78/79`, `308`,
  `321-324`, `330-332`, `341-344`, `350-353`, `360-361`, `400`).
- `MusicSelectEventEffectKind::OptionChangeSound` is the *only* sound effect
  kind (`MusicSelectEventController.h:9-26`).
- `MusicSelectScene::executeEvent`'s switch (`:2401-2547`) has **no
  `OptionChangeSound` case** and falls through to `default: break`.
- `MusicSelectInputProcessor.cpp:233,238,350,356` emit
  `MusicSelectInputActionKind::ScratchSound`, but
  `MusicSelectScene::applyInputAction` (`:2248-2360`) has no case and no audio
  invocation.
- Folder open/close play nothing: the scene's `openDirectory`/`closeDirectory`
  are silent.

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

Fix: give `MusicSelectScene` a small system-SE adapter (same
`AudioWrapper` skin-sound APIs the preview service already uses), handle
`OptionChangeSound`, `ScratchSound`, and add `FolderOpen`/`FolderClose`
effects; play `GUIDESE_*` when the selected chart's guide is on.

### 4. No default select BGM, and no decide sound

Beatoraja's `PreviewMusicProcessor` is initialized with the looping
`SELECT` system sound as its default fallback
(`MusicSelector.java:173-174`, `PreviewMusicProcessor.java:41-43,79-81`), and
`MusicSelector.render()` starts it on `prepare()` and on bar move
(`:188`, `:613-615`). AsoBMaShow's `MusicSelectPreviewAudioService` has no
default music: it plays the preview file or leaves the worker idle
(`MusicSelectPreviewAudioService.cpp:75-78`).

Similarly, `MusicSelectScene::launchSelected` shows the decide overlay
(`showDecideOverlay`, `:1656`) without the source `DECIDE` sound.

### 5. Preview with no `#PREVIEW` is silent (Beatoraja fades to select BGM)

`MusicSelectPreviewController::update` returns `nullopt` for an empty preview
path (`MusicSelectPreview.cpp:44-47`), so the scene switches audio to nothing
(`MusicSelectScene.cpp:2630-2633`). Pinned behavior falls back to the default
select BGM and fades volume on stop (`PreviewMusicProcessor.java:85-101,
115-133`). With gap #4 fixed, empty preview naturally routes to `SELECT`.

### 6. `SongPreview` mode is not modeled

Beatoraja's `Config.SongPreview` is `NONE`/`ONCE`/`LOOP`
(`MusicSelector.java:204`) and the preview volume follows
`AudioConfig.systemvolume`. AsoBMaShow hard-codes loop with volume `1.0`
(`MusicSelectPreviewAudioService.cpp:95`:
`playSkinSound(*handle, 1.0F, true)`), and only models
`archiveChartPreviewEnabled` (archive suppression) in the scene
(`MusicSelectScene.cpp:191-198`). The setting is not exposed or honored.

### 7. `Num6` is unmapped

`MusicSelectControlKey` has no `Num6` (`src/music_select/MusicSelectInputProcessor.h:55-71`)
and `MusicSelectScene::controlKey()` omits `SDLK_6`
(`MusicSelectScene.cpp:241-249`, it jumps `SDLK_6` -> `SDLK_7`). Pinned
`MusicSelector.input()` maps `NUM6` to the CONFIG screen
(`MusicSelector.java:296-300`). AsoBMaShow's equivalent is the Settings
scene. Add `Num6`, bind `SDLK_6`, and route it to the same Settings entry the
toolbar uses.

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

Pinned `EventType` distinguishes `keyconfig` (13) and `skinconfig` (14)
(`EventFactory.java`). AsoBMaShow routes both to the same Settings destination
(`MusicSelectEventController.cpp:185-186`), losing the distinct key-config
destination. The toolbar-spec even calls out Settings as the one application
destination; decide whether key-config gets its own surface or is an explicit
non-goal.

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
  `EventType` count, minus the sound sink described above.

## Remediation roadmap

Proposed slices, each coherent test+ledger unit in the style of the existing
plans. None requires a whole-file formatter; media paths already exist and are
reused.

1. **Movie decode for JSON/Lua select sources** — covered: shared
   `SkinMovieCatalog::resolveMovies` promotes movie-extension image resources
   to `SkinMovieResource`; proven by `tests/skin_movie_catalog_types_tests`
   (type-5 movie fixture + `SkinMovieCatalog` assertion: a movie source is
   decoded, prepared, and drawn).
2. **Select SE wiring** — add a system-SE adapter reusing the preview service's
   `AudioWrapper` skin-sound path; handle `OptionChangeSound`, `ScratchSound`,
   add `FolderOpenSound`/`FolderCloseSound`/`OptionOpenSound`/`OptionCloseSound`
   effects; play `GUIDESE_*` from the persisted `guideSoundEffects` setting.
3. **Default select BGM + preview parity** — `SELECT` default in the preview
   service, fade-back on preview end, `DECIDE` on launch, and `SongPreview`
   `NONE/ONCE/LOOP` + `systemvolume` honoring.
4. **Input/event branches** — add `Num6` (`SDLK_6`) → Settings; add open
   skin-configuration handling or record it as an explicit non-goal; decide the
   `keyconfig`/`skinconfig` split.
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