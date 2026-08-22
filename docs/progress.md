# Gameplay-skin compatibility progress

This log records verified completed work and explicitly scoped in-progress
work from [`docs/todo.md`](todo.md). The pinned authority is Beatoraja commit
`c2ed5db1a46145ed10790c3872f717e95b59db9d` in
`~/workspace/SNURhythm/beatoraja`.

## In progress

- Table strings use the selected difficulty-table path now. The pinned
  `PlayerResource.setTableinfo()` fallback additionally searches the user's
  ordered `Config.tableURL` collection for direct launches; Aso has no
  equivalent per-profile table configuration, so those launches retain the
  source reset value rather than selecting an arbitrary imported table.

## Completed

- The pinned gameplay-skin ledger now classifies all 771 source-surface rows:
  755 implemented rows have row-exact executed native evidence, 16 rows retain
  their pinned source-defined no-op evidence, and none remain `missing`.
  Redistributable Lua, JSON, and LR2 fixtures prove the genuinely shared
  slider/resource/rate-binding/destination model and identical textured draw
  trace. Format-exclusive gameplay families are decoded and asserted by their
  native all-field/all-command fixtures instead of being treated as
  equivalent. `beatoraja_gameplay_skin_ledger_tests.py --require-complete`
  executes every row's bound native runner as the CI gate.

- The conformance evidence now loads matching Lua and JSON fixtures through
  the pinned Beatoraja frontends, compares their source-owned cases with Aso,
  and records consumed viewport/time/state inputs. The performance runner
  refuses to transplant candidate code into its baseline and requires the
  same marked Lua/JSON/LR2 workload on both revisions. Local ModernChic 4.6
  acceptance publishes all 12 gameplay entry/mode sessions (5/7/10/14 keys
  by LN/CN/HCN), matches generated pinned selector/draw slots, and compares
  decoded-model resource references with prepared catalog identities.

- Practice now has a source-shaped no-audio `BMSPlayer.STATE_PRACTICE` phase.
  Before its held first input begins an attempt, selector `1080`, row
  availability/selection `3000`–`3035`, practice text strings, and committed
  row-event changes all use the retained ten-row controller. The start path
  applies `PracticeModifier`'s visible-note/background split and TOTAL
  correction, source frequency timing, DP flip and random options, the
  five-category/nine-gauge `GaugeProperty` table, and the pinned percentage
  JUDGERANK window rule. Normal practice gameplay remains outside that state,
  so its practice booleans stay false. Verified by
  `practice_configuration_tests`, `gameplay_gauge_rules_tests`,
  `playfield_visual_state_tests`, `play_skin_state_bridge_tests`, and
  `play_skin_session_tests`.

- String properties `60`–`62` and `86` now mirror the persisted Beatoraja
  `PlayerConfig` values in live play, replay watching, and replay export:
  ModeFilter/DifficultyFilter display strings plus raw `sortid` and
  `chartReplicationMode`. Aso keeps the source defaults (`ALL`, `TITLE`,
  `ALL`, `RIVALCHART`) and preserves configured strings verbatim. Verified by
  `app_settings_store_tests` and `play_skin_state_bridge_tests`.

- `skin.note.dst2` now implements Beatoraja PMS missed-POOR drawing through
  live gameplay, replay watching, and replay export. The selected skin
  publishes lane-zero's shared destination geometry; projection mirrors
  `LaneRenderer`'s retained cursor, late-BAD hold, STOP/BPM no-speed descent,
  `dstnote2` clamp, and source normal-note states (`0` or `>= 4`). The
  renderer uses the normal sprite, source clip/position behavior, note
  expansion, and the inherited final Constant-pass alpha. Verified by
  `playfield_projection_tests`, `skin_draw_command_tests`, and
  `play_skin_session_tests`.

- String properties `1020` (`irname`) and `1021` (`irUserName`) now carry
  their distinct source authorities through live play, replay viewing, and
  normal/course replay export. `1020` retains the first persisted provider id,
  including a disabled provider. `1021` requests the authenticated Tachi user
  document and uses the first successful `username`; no local profile or
  provider fallback is substituted when authentication is absent or fails.
  Verified by `tachi_driver_tests`, `playfield_visual_state_tests`,
  `play_skin_state_bridge_tests`, and a desktop `main` build.

- Target-neighbour strings `200`–`219` now use persisted source-shaped
  `PlayerConfig.targetid` and `targetlist` values in live play, replay
  watching, and normal/course replay export. The bridge reproduces
  `StringPropertyFactory.createTargetname`'s first-match ring and ten-slot
  wrap, then applies `TargetProperty` names, including generic Java-float rate
  text, missing-rival `NO RIVAL`, IR labels, and the fallback `MAX`. A missing
  configured id remains the source empty-string branch. Verified by
  `app_settings_store_tests` and `play_skin_state_bridge_tests`.

- Gameplay `rival` and `target` strings (`1`, `3`) now share their pinned
  `BMSPlayer` target-score-player source, independently of Aso's pacemaker
  presentation. Static and next-rank targets expose their `TargetProperty`
  player label; no Aso RivalDataAccessor produces the source `NO RIVAL` or
  local-self empty branches, and without a gameplay RankingData instance IR
  targets produce `NO DATA`. Practice remains empty. G-Battle does not invent
  a player name absent from its record. Verified by
  `play_skin_state_bridge_tests`.

- `#SPEEDxx` / `#mmmSP` speed objects now flow from the shared parser through
  immutable chart state, the live and replay-export authorities, selected-skin
  duration selectors, and the built-in traversal. The frame-local multiplier
  follows pinned `LaneRenderer.getCurrentSpeed`: base `1.0`, clamped linear
  interpolation between authored points, then the final point's value. It is
  multiplied with `#SCROLL` rather than replacing it; Constant forces only
  that multiplier to `1.0` outside practice. The parser dependency is committed
  and pushed as `bms-parser-cpp` `7ffff21`.

- Selected difficulty-table launches now capture `PlayerResource`'s table
  strings: name (`1001`), folder label (`1002`), and its exact
  level-before-name full value (`1003`). Aso's table symbol and raw level make
  the same `TableDataAccessor` `tag + level` label as the pinned source.
  Verified by `play_skin_state_bridge_tests` and a desktop `main` build.

- Lift/HIDDEN state now follows `PlayConfig`: disabled 0.1 defaults, source
  finite `[0,1]` sanitization, persistence, gameplay capture, and the exact
  `ControlInputProcessor.setCoverValue` selection order. A short
  START+SELECT conjunction switches between the two enabled planes, while
  automatic fixed-Hi-Speed recalculation remains gated by
  `hispeedAutoAdjust`. Selected-skin rendering now also mirrors
  `LaneRenderer`: dynamic fractional offsets 3/4/5 come from lane region
  zero, HIDDEN preserves its prior y while disabled and hides with -255 alpha,
  and Lift moves the shared note origin while shortening its Hi-Speed scroll
  height. Verified by `app_settings_store_tests`,
  `play_skin_state_bridge_tests`, and `skin_draw_command_tests`.

- Aso's initial practice launch configuration is available to
  `practice_item` text/label/value selectors. The audit corrected the boolean
  selectors: Beatoraja gates `state_practice` and every visible-row
  availability/selection selector on its distinct `STATE_PRACTICE` UI, which
  Aso does not implement, so they remain false rather than infer menu state
  from an active practice attempt. Verified by `practice_configuration_tests`
  and `play_skin_state_bridge_tests`.

- Constant play now forwards `PlayConfig.duration` and
  `constantFadeinTime` through selected-skin projection. Normal notes, long
  notes, and section/BPM/STOP lines share the pinned `LaneRenderer` time
  window and alpha, including the signed negative-fade branch and the strict
  positive-fade endpoint. Verified by `playfield_projection_tests`,
  `app_settings_store_tests`, and a desktop `main` build.

- `notesDisplayTimingAutoAdjust` (`75`) now implements its source behavior,
  not only its image selector: eligible PGREAT/GREAT/GOOD judgements in live
  play or practice mutate the separate display-only `judgetiming` state using
  Beatoraja `JudgeManager`'s signed 30,000µs Java-truncating step. Replay and
  autoplay stay excluded. Verified by `playfield_projection_tests` and
  `app_settings_store_tests`.

- `PlayerConfig.showpastnote` now persists as `showPastNotes` and reaches
  live play, replay presentation, and replay export. The projection mirrors
  the pinned `LaneRenderer` branch precisely: after the timeline has passed,
  only an unresolved ordinary note is retained; judged/dead notes, mines, and
  invisible notes remain absent. Verified by `playfield_projection_tests` and
  `app_settings_store_tests`.

- String property `1010` now returns AsoBMaShow's CMake-declared application
  version (`0.0.1` in this build), matching Beatoraja's controller-level
  version source. Verified by `play_skin_state_bridge_tests`.

- Image indexes `75`, `321`–`324`, `343`, `350`–`353`, `360`–`361`, and
  `400` now carry their exact raw `PlayerConfig`/`PlayConfig` values through
  persistent app settings into immutable gameplay presentation. Boolean
  `OPTION_CONSTANT` shares the same captured `PlayConfig` flag. Its renderer
  behavior is recorded above; the remaining forced-speed path requires parser
  speed-object authority. Verified by `app_settings_store_tests` and
  `play_skin_state_bridge_tests`.

- Image indexes `301` and `303` now carry the source `PlayerConfig`
  `customJudge` and `showjudgearea` booleans. Both values persist in app
  settings and are captured for gameplay rather than inferred from Aso's
  judge display controls. Verified by `app_settings_store_tests` and
  `play_skin_state_bridge_tests`.

- Stagefile/backbmp booleans (`190`–`191`, `194`–`195`) now cover the last
  standard JDK `ImageIO` fallback in pinned `PixmapResourcePool`: Type-0
  WBMP. The shared bounded decoder already covered the native image formats,
  LibGDX CIM, and FFmpeg WebP path. Verified by
  `image_file_decoder_tests`.

- `TIMER_RHYTHM` (`140`) now mirrors pinned `RhythmTimerProcessor` behavior:
  source-order integer accumulation from frame deltas, the active BPM, and
  default 100% `BMSPlayer` play speed, followed by at-most-one section-line
  reset to the current skin clock. Verified by
  `play_skin_state_bridge_tests` across two section boundaries and a BPM
  change.

- Image index `341` now exposes the active auto-shift lower bound with the
  pinned `GrooveGauge` index. The value is preserved in immutable gameplay
  authority from `StartOptions`, rather than inferred from the currently
  selected gauge. Verified by `play_skin_state_bridge_tests`.

- Player total-play-time fields `17`–`19` now sum each local attempt's pinned
  `PlayDataAccessor.writeScoreData()` duration: the final playable-note
  timestamp truncated to whole seconds. The duration travels in score
  provenance across modern-result retries, is projected into score schema v12,
  and schema migration backfills matching existing attempts from chart
  metadata. Imported IR rows remain excluded. Verified by
  `score_provenance_tests`, `score_provenance_db_tests`, and
  `play_skin_state_bridge_tests`.

- Built-in FloatWriter IDs `17`–`19` now mutate master, keysound, and BGM
  volume respectively, as pinned `FloatPropertyFactory.RateType` mutates
  `Config.AudioConfig`. The writer values retain the normalized slider range,
  stage in invocation order, and reach the live Aso audio boundary only after
  the skin frame submits. Lane-cover remains correctly non-writable and
  practice-position awaits a source-compatible practice menu. Verified by
  `play_skin_state_bridge_tests`, `play_skin_session_tests`,
  `gameplay_skin_validator_tests`, and a targeted desktop build.

- Extended 1P lane timers: `1010`–`1099`, `1210`–`1299`, `1410`–`1499`,
  `1610`–`1699`, `1810`–`1899`, and `2010`–`2099` now use the same
  `LaneProperty` skin offset and `SkinPropertyMapper` ranges as Beatoraja.
  Classic long-note, HCN-increase, and HCN-damage state remains distinct.
  Verified by `play_skin_state_bridge_tests`, including a 24K offset-10
  regression case.

- `SongInformation` density, peak density, end density, and TOTAL now live in
  immutable chart state. The calculation follows the pinned constructor's
  whole-second bins and long-note accounting; numeric property factories keep
  its Java integer/float conversion and missing-information sentinels.
  Verified by `playfield_chart_visual_model_tests` and
  `play_skin_state_bridge_tests`.

- `TIMER_STARTINPUT` (`1`) now uses `BMSPlayer`'s strict post-`input` delay
  transition and preserves the first observed timer-clock timestamp. Verified
  by `play_skin_state_bridge_tests`.

- `TIMER_FAILED` (`3`) now starts from the active survival-gauge failure event,
  matching the source `STATE_FAILED` transition without using an inferred
  numeric gauge threshold. Verified by `play_skin_state_bridge_tests`.

- SongReview image indexes `89` and `90` now capture the active chart's
  pinned SHA-256 review bitmask. Each selector projects its own favourite and
  invisible pair to the source none/favourite/invisible states (`0`/`1`/`2`),
  including invisible precedence. Chart schema v5 uses the source review
  columns, migrates existing chart favourites to `FAVORITE_CHART`, and keeps
  unrelated bits when the app's chart-favourite toggle changes. Verified by
  `chart_repository_tests`, `chart_library_scanner_tests`,
  `play_skin_state_bridge_tests`, and a desktop `main` build.

- Normal 1P HCN active and damage timers (`250`–`259`, `270`–`279`) now use
  the captured HCN increase/damage state rather than ordinary long-note hold.
  Verified by `play_skin_state_bridge_tests`.

- Persisted score properties `80`–`89`, float rates `85`–`89`, and last-play
  properties `243`–`249` now read an immutable ScoreData-equivalent captured
  at gameplay activation. Its judgement values come from the first attempt to
  establish the high EX score (computed from PGREAT/GREAT, not a denormalized
  storage field); its timestamp is the latest local play, exactly as pinned
  `ScoreData.update()` and `PlayDataAccessor.writeScoreData()` keep them.
  Verified by `play_skin_state_bridge_tests` and
  `score_provenance_db_tests`.

- Player-history properties `30`–`37` and `333` now use the corresponding
  locally persisted aggregate: play count, clear count, five judgement totals,
  and the source-defined note subtotal. Imported IR projections are excluded,
  as they do not pass through Beatoraja `PlayDataAccessor.updatePlayerData()`.
  Verified by `play_skin_state_bridge_tests` and
  `score_provenance_db_tests`.

- Wall-clock integer properties `21`–`26` now read the local calendar at the
  same evaluation boundary as `MainController.getCurrnetTime()`. Verified by
  `play_skin_state_bridge_tests`.

- Player-2 and player-3 judge-duration properties `526`–`527` retain the
  pinned `JudgeManager.getRecentJudgeTiming()` out-of-range value of zero,
  matching Aso's absent 2P/3P authority slots.

- Play-level aliases `45`–`49` now share property `96`'s immutable chart
  play-level value, matching `IntegerPropertyFactory.createPlayLevelProperty`.

- Banner options `192`–`193` now preserve the pinned gameplay resource state:
  `BMSResource.setBMSFile()` leaves its banner texture null, so `no_banner`
  is active and `banner` is inactive even for a declared BMS banner path.

- `PomyuCharaProcessor` timers `900`–`907` and `909` now also consume declared
  PLAY `pmchara` cycles. Activation reads the source `.chp` through the skin
  filesystem and follows `PomyuCharaLoader`'s motion ordering, defaults,
  side-specific mapping, and positive-cycle update rule; absent or failed
  character sources retain the source one-millisecond defaults. Character-image
  rendering remains a separate unsupported visual path. The extractor and
  frame-state contracts are verified by `beatoraja_skin_model_tests` and
  `play_skin_state_bridge_tests`.

- Target-option image indices `61`–`63` now follow the pinned BMSPlayer
  target lifecycle: regular gameplay uses the default `TargetProperty`
  `ScoreData.option` of zero, practice retains the absence sentinel
  `Integer.MIN_VALUE`, and G-Battle projects its persisted target as
  `1P + 10 * 2P + 100 * DP`. Verified by `score_provenance_tests` and
  `play_skin_state_bridge_tests`.

- The practice-position rate `20` exposes the pinned freshly created
  `PracticeConfiguration` value of zero and now accepts its source writer.
  The active practice session retains the scroll offset; it is applied only
  after successful skin submission and rebuilds the next ten-row viewport,
  including the two-row double-play scroll range. Verified by
  `practice_configuration_tests`, `play_skin_state_bridge_tests`, and
  `play_skin_session_tests`.

- Float loading progress `165` now follows the bridge's source-compatible
  Loading/Loaded authority: zero before resource completion and one after
  BMSPlayer's media-ready transition. Verified by
  `play_skin_state_bridge_tests`.

- `song_no_text` / `song_text` (`174`–`175`) now carry Beatoraja's
  `SongData.CONTENT_TEXT` bit from the chart scan through gameplay authority.
  The scanner checks immediate non-directory `*.txt` children with
  case-insensitive matching, stores the result per chart, and forces a normal
  library rescan on migration because prior metadata lacks that source fact.
  Android SAF enumeration transfers the same per-directory result. Verified
  by `chart_library_scanner_tests`, `chart_repository_tests`, and
  `play_skin_state_bridge_tests`.

- `current_fps` (`20`) and boot-time hours/minutes/seconds (`27`–`29`) now
  capture live outer-loop runtime authority: rendered frames are measured in
  one-second windows, while uptime begins with `ApplicationContext`
  construction like `MainController.boottime`. Verified by
  `play_skin_state_bridge_tests` and targeted `main` object compilation.

- G-Battle now passes the persisted target `ChartScoreWrite` into gameplay.
  Rival selectors `271`, `280`–`289`, and float rates `285`–`289` therefore
  use the same target-score/judgement-count model as Beatoraja
  `ScoreDataProperty`; absent target records keep its zero or sentinel values.
  Verified by `play_skin_state_bridge_tests` and targeted gameplay/menu
  compilation.
