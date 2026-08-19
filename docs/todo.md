# TODO

## Gameplay-skin compatibility gaps

This is the source-audited inventory for the pinned Beatoraja checkout ~/workspace/SNURhythm/beatoraja
`c2ed5db1a46145ed10790c3872f717e95b59db9d`. It lists a property only when
Beatoraja's `BMSPlayer` path can provide non-default data and the gameplay-skin
bridge currently returns a hard-coded default, a sentinel, or no value.

Do not add behavior for arbitrary legal numeric cache IDs: Beatoraja exposes a
wide integer/timer lookup domain, but those are not all defined properties.
Likewise, selector/result/IR properties whose own `BMSPlayer` path returns the
source default are not compatibility work for gameplay skins.

### Boolean properties

- [x] Chart text availability (`174`–`175`) now persists the same folder-level
  `SongData.CONTENT_TEXT` fact as Beatoraja: an immediate non-directory
  `*.txt` child, case-insensitive. The metadata migration intentionally
  invalidates the old cache because it cannot reconstruct that scan result.
- [x] Decoded stagefile (`190`–`191`) / backbmp (`194`–`195`) availability
  records the shared decoder's successful BMS-relative image load instead of
  a raw header path. It now covers the pinned native/CIM paths, FFmpeg WebP
  fallback, and the remaining standard-JDK `ImageIO` Type-0 WBMP reader.
- [x] Banner availability (`192`–`193`) follows pinned `BMSResource`: its
  gameplay load path does not populate the banner texture, so `no_banner` is
  true and `banner` is false even if a chart declares `#BANNER`.
- [x] Constant scroll (`400`) now captures the persisted `PlayConfig`
  enablement flag for both its image-index and boolean forms. Its source
  duration/fade projection is complete; only the separately tracked parser
  speed-object authority still prevents Constant's forced-speed path.
- [x] Initial practice item availability and selection (`3000`–`3015`,
  `3020`–`3035`) now follow the captured `PracticeConfiguration` viewport:
  the ten default visible rows are available and the initial `START TIME`
  cursor is selected. Aso has no source-compatible `STATE_PRACTICE` menu, so
  user-driven row selection and scrolling remain unavailable.

### Integer-value properties

- [x] Profile history: locally recorded play/clear/judgement counters
  (`30`–`37`, `333`) reconstruct Beatoraja `PlayerData` from local score
  attempts, excluding imported IR records.
- [x] Wall-clock fields (`21`–`26`) read the local calendar at property
  evaluation, matching `MainController.getCurrnetTime()`.
- [x] Total-play duration fields (`17`–`19`) now reconstruct
  `PlayerData.playtime` from local score attempts. Each attempt retains
  Beatoraja's `PlayDataAccessor.writeScoreData()` duration: the whole seconds
  of the final playable note. Score schema v12 stores that value, backfills
  matching existing attempts from chart metadata by the same exact identity
  precedence, and excludes imported IR rows from the aggregate.
- [x] Live FPS (`20`) and application-uptime fields (`27`–`29`) now capture
  the outer render-loop frame count and the `ApplicationContext` construction
  timestamp, matching `Gdx.graphics.getFramesPerSecond()` and
  `MainController.getPlayTime()` as live application values.
- [x] Play-level aliases (`45`–`49`, `96`) read the active chart's immutable
  SongData-equivalent play level.
- [x] Stored score history: judgement counts/rates (`80`–`89`) and last-play
  date/time (`243`–`249`) reconstruct Beatoraja's `ScoreData.update()`
  semantics from local attempts: the first high-EX-score judgement record and
  the latest-play timestamp. Imported IR rows are excluded because the source
  reads its local `ScoreData` database.
- [x] G-Battle rival score details (`271`, `280`–`289`) now retain the target
  `ChartScoreWrite` through gameplay activation. They reproduce
  `ScoreDataProperty`: rival EX score is `PGREAT * 2 + GREAT`; per-judge
  values require a target record; integer and float rates use that target
  record's own note count. Other pacemaker modes still correctly have no
  rival `ScoreData` record and therefore retain the pinned zero/sentinel
  branches.
- [x] `SongInformation` analysis: density/peak/end-density/total
  (`360`–`365`, `368`) is retained in immutable chart state. Missing analysis
  continues to return the upstream `Integer.MIN_VALUE` sentinel.
- [x] Player 2/3 judge-duration values (`526`–`527`) return zero. Pinned
  `JudgeManager.getRecentJudgeTiming(player)` has the same fallback for the
  absent 2P/3P slots in Aso's single-player authority.

### Image-index properties

- [x] Target play-option images (`61`–`63`) now follow BMSPlayer's target
  lifecycle: normal gameplay installs `TargetProperty`'s default option `0`,
  practice retains the upstream `Integer.MIN_VALUE` absence sentinel, and a
  G-Battle target projects its persisted 1P/2P/DP options with the pinned
  `1P + 10 * 2P + 100 * DP` encoding.
- [x] Favourite song/chart states (`89`, `90`) now read the pinned
  SHA-256-keyed `SongReview.favorite` bitmask: `FAVORITE_SONG`/`CHART`
  (`1`/`2`) and `INVISIBLE_SONG`/`CHART` (`4`/`8`). Each index maps its own
  pair to none/favourite/invisible (`0`/`1`/`2`), with the source's invisible
  precedence. Chart schema v5 stores the source review columns, migrates
  existing chart favourites to bit `2`, and retains the unrelated song and
  invisible bits when the app's chart-favourite toggle changes.
- [x] Custom judge and judge-area configuration (`301`, `303`) now preserve
  the exact `PlayerConfig` boolean values in app settings and capture them at
  gameplay activation for `IndexType`.
- [x] Player configuration image indexes: display-timing auto-adjust (`75`),
  replay-save slots (`321`–`324`), Guide SE (`343`), extra-note/mine/scroll/
  long-note modifiers (`350`–`353`), 7-to-9 settings (`360`–`361`), and
  constant-scroll enablement (`400`) now retain their raw pinned configuration
  values through gameplay-skin activation.
- [x] `notesDisplayTimingAutoAdjust` now also performs the pinned
  `JudgeManager` display-only `judgetiming` update: PGREAT/GREAT/GOOD while
  playing or practising adjust in Java's signed 30,000µs steps inside ±150ms.
  The mutable display timing is independent of input timing and has no added
  update-path clamp.
- [x] Gauge auto-shift lower bound (`341`) now carries the active
  `StartOptions::gaugeAutoShiftLowerBound` through immutable gameplay state
  and exposes its pinned `GrooveGauge` index.

`450`–`469` are deliberately absent: pinned Beatoraja only exposes replay
lane assignments from `MusicResult`, and returns zero during `BMSPlayer`
gameplay.

### Float and rate properties

- Practice-item scroll authority for `20`: the bridge now exposes pinned
  `PracticeConfiguration`'s default zero position, but Aso has no separate
  `STATE_PRACTICE` menu to carry user-adjusted scroll state.
- [x] Float loading progress (`165`) exposes the exact completed source value
  while gameplay is loaded (and zero before it). The bridge has no source
  authority for intermediate AudioDriver/BGAProcessor fractions.
- [x] G-Battle rival judgement rates (`285`–`289`) share the captured target
  `ScoreData` record with their integer counterparts. Non-G-Battle targets
  retain the source `Float.MIN_VALUE` absence sentinel.
- [x] Chart analysis float values (`360`, `362`, `367`, `368`) now read the
  immutable `SongInformation` equivalent and retain the upstream
  `Float.MIN_VALUE` sentinel when it is unavailable.

### String properties

- Rival and selected target: `1`, `3`.
- Global profile filters/configuration: `60`–`62`, `86`.
- [x] Selected difficulty-table launch strings (`1001`–`1003`) now retain the
  active local table name and the pinned `TableData.tag + level` folder title;
  `tablefull` preserves `PlayerResource`'s level-before-name concatenation.
  Beatoraja's separate direct-launch fallback searches the per-profile ordered
  `Config.tableURL` list, which Aso does not own, so it correctly remains
  empty outside a selected difficulty-table context.
- [x] IR provider string (`1020`) now captures the first persisted Aso IR
  provider id in live play and replay export, including a disabled provider,
  matching `PlayerConfig.irconfig[0].irname` rather than a UI display label.
- IR account string (`1021`) remains unavailable: it requires the first
  connected `IRStatus.player.name`, while Aso persists no equivalent remote
  account-name authority.
- [x] Application version (`1010`) returns the CMake-declared AsoBMaShow
  project version, matching `MainController.getVersion()`'s application-level
  source instead of a gameplay-scene fallback.
- Configured target-name neighbours (`200`–`219`).
- [x] Initial practice item text, labels, and values (`1040`–`1095`) expose
  the captured source-compatible ten-row `PracticeConfiguration` viewport.
  User-driven menu mutation remains unavailable with the separate
  `STATE_PRACTICE` UI.

### Timers

- [x] Start-input animation (`1`) follows the decoded skin `input` delay and
  records the first strict post-delay frame timestamp.
- [x] Failure animation (`3`) starts from the captured active survival-gauge
  failure transition.
- [x] 1P HCN state: `250`–`259`, `270`–`279` uses the captured
  JudgeManager-equivalent passing/increase and damage state, independently of
  the ordinary long-note hold timers (`70`–`79`).
- [x] Rhythm (`140`) follows `RhythmTimerProcessor`: it retains the Java
  per-frame BPM accumulator, walks the source's section-line cursor one entry
  per frame, and resets to the skin clock only after that frame's accumulator
  write. Aso has no replay/autoplay speed-key authority, so its source-default
  100% play-speed input is used.
- [x] Default Pomyu processor timers: `900`–`907`, `909` follow pinned
  `PomyuCharaProcessor` lifecycle and its unconfigured one-millisecond motion
  cycles.
- [x] Declared PLAY `pmchara` objects now extract their timer cycles from the
  selected `.chp` source with pinned `PomyuCharaLoader` ordering: `#Patern` /
  `#Pattern`, `#Texture`, then `#Layer`; `#Frame`/`#Flame` and `#Anime`
  defaults; side-specific timer mapping; and `setPMcharaTime`'s positive-cycle
  rule. Character-image rendering remains a separate unsupported visual path;
  it does not change the source timer authority.
- [x] Extended 1P key timers: bomb `1010`–`1099`, hold `1210`–`1299`,
  key-down `1410`–`1499`, key-up `1610`–`1699`, HCN active `1810`–`1899`,
  and HCN damage `2010`–`2099`. These follow `LaneProperty` and
  `SkinPropertyMapper` rather than the display-lane index.

### Native writers and 2P/3P scope

- [x] Native audio-volume writers (`17`–`19`) now apply the pinned
  `FloatPropertyFactory.RateType` `Config.AudioConfig` targets for master,
  keysound, and BGM volume. The bridge stages the normalized slider value and
  applies it to Aso's live audio boundary only after the authored skin frame
  has submitted.
- The pinned `FloatPropertyFactory.RateType` has no writer for lane cover
  (`4`, `5`). Practice-position writer (`20`) remains unavailable: Aso has no
  `STATE_PRACTICE` configuration/menu state to mutate. Lua callback writers
  remain supported independently of these built-in writer IDs.
- The application has no two-player/three-player gameplay authority. The
  corresponding judge and timer families remain unavailable rather than being
  fabricated from 1P state.

### Note-display modes

- [x] `PlayerConfig.showpastnote` persists through gameplay/replay/export
  presentation and matches the pinned `LaneRenderer` condition: only an
  unresolved ordinary note remains after its timeline; resolved notes, mines,
  and hidden notes do not gain a past-note path.
- `skin.note.dst2` is decoded and its lane geometry is preserved, but the
  source's PMS missed-POOR path is not implemented. In pinned `LaneRenderer`,
  a configured `dstnote2` changes ordinary-note eligibility by judgement state
  and then renders the missed-POOR descent separately. The current projection
  has only judged/dead state, not the required source judgement-state and
  descent timing.

### Existing parser and renderer work

- [x] `bms-parser-cpp` now preserves Beatoraja `TimeLine` SPEED objects:
  `#SPEEDxx` definitions and `#mmmSP` (base-36 `1033`) references retain both
  each propagated multiplier and its explicit-object marker. Gameplay and
  replay-export authority reproduce `LaneRenderer.getCurrentSpeed`'s default
  `1.0`, clamped linear interpolation, and final-value behavior. The renderer
  composes that factor with the independently active `#SCROLL` rate; Constant
  forces only SPEED to `1.0` outside practice, exactly as the pinned source.

- [x] `PlayConfig.enableConstant` now applies `LaneRenderer`'s duration and
  signed fade window to selected-skin normal notes, long notes, and timeline
  lines. The exact endpoint behavior is retained (positive fade includes the
  target row and excludes the fade endpoint); its forced-SPEED path now shares
  the source-faithful speed-object authority above.

- [x] Lift/HIDDEN `PlayConfig` state now persists the pinned defaults and
  `[0,1]` finite ranges, captures the live enablement/ratios for all matching
  selectors, and follows `ControlInputProcessor.setCoverValue`'s scratch
  routing plus its `isChangeLift` START+SELECT target toggle.
- [x] Selected-skin Lift/HIDDEN rendering now mirrors `LaneRenderer`: lane
  region zero drives fractional reserved offsets `OFFSET_LIFT` (`3`),
  `OFFSET_LANECOVER` (`4`), and `OFFSET_HIDDEN_COVER` (`5`); HIDDEN preserves
  its previous y while disabled and applies -255 alpha; Lift raises the shared
  note origin and shortens the source Hi-Speed scroll span. Authored cover
  destinations consume the runtime values after `LaneRenderer` overwrites the
  configured reserved offsets' dynamic fields, as in the pinned source.
