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

- Chart metadata: text availability (`174`–`175`) and decoded banner
  availability (`192`–`193`). A raw BMS banner path is not equivalent to
  Beatoraja's successfully decoded `BMSResource` image.
- Constant scroll (`400`).
- Practice item availability and selection (`3000`–`3015`, `3020`–`3035`).

### Integer-value properties

- Profile history, wall-clock, and FPS: `17`–`37`, `45`–`49`, `333`.
- Stored score history: judgement counts/rates (`80`–`89`), last-play
  date/time (`243`–`249`), and rival score details (`271`, `280`–`289`).
- `SongInformation` analysis: density/peak/end-density/total (`360`–`365`,
  `368`). Until the analysis object exists in immutable chart state, retain
  the upstream `Integer.MIN_VALUE` sentinel.
- Player 2/3 judge-duration values (`526`–`527`) are currently hard-coded to
  zero instead of reading the equivalent player slot.

### Image-index properties

- Target and unrepresented configuration: `61`–`63`, `75`.
- Favourite song/chart state: `89`–`90`.
- Custom judge and judge-area configuration: `301`, `303`.
- Replay-save and unrepresented gameplay configuration: `321`–`324`,
  `341`, `343`, `350`–`353`, `360`–`361`, `400`.

`450`–`469` are deliberately absent: pinned Beatoraja only exposes replay
lane assignments from `MusicResult`, and returns zero during `BMSPlayer`
gameplay.

### Float and rate properties

- Practice-item position (`20`).
- Float loading progress (`165`); integer loading progress exists but only
  models an unloaded/loaded coarse state.
- Rival judgement rates (`285`–`289`).
- Chart analysis float values (`360`, `362`, `367`, `368`).

### String properties

- Rival and selected target: `1`, `3`.
- Global profile filters/configuration: `60`–`62`, `86`.
- Table, version, and IR: `1001`–`1003`, `1010`, `1020`–`1021`.
- Configured target-name neighbours (`200`–`219`).
- Practice item text, labels, and values (`1040`–`1095`).

### Timers

- Start-input and failure animation: `1`, `3`.
- 1P HCN state: `250`–`259`, `270`–`279`. These require JudgeManager's
  passing/increase state, which is distinct from the already wired long-note
  hold timers (`70`–`79`).
- Rhythm (`140`), including Beatoraja's `RhythmTimerProcessor` accumulator,
  section-line reset, BPM, and play-speed inputs. Keep it off until this exact
  state is captured; do not invent a measure pulse.
- Pomy character timers: `900`–`907`, `909`.
- Extended 1P key timers: bomb `1010`–`1099`, hold `1210`–`1299`, key-down
  `1410`–`1499`, key-up `1610`–`1699`, HCN active `1810`–`1899`, and HCN
  damage `2010`–`2099`.

### Native writers and 2P/3P scope

- Native property writers are not implemented. Beatoraja writers exist for
  lane cover (`4`, `5`), audio volumes (`17`–`19`), and practice position
  (`20`); the bridge currently accepts Lua callback writers only.
- The application has no two-player/three-player gameplay authority. The
  corresponding judge and timer families remain unavailable rather than being
  fabricated from 1P state.

### Note-display modes

- `PlayerConfig.showpastnote` is not exposed. The default is `false`, which is
  the behavior used by the shared projection: ordinary, mine, and hidden notes
  stop rendering once their timeline has passed. Do not retain an unhandled
  ordinary note behind the judgement line unless this exact option is added.
- `skin.note.dst2` is decoded and its lane geometry is preserved, but the
  source's PMS missed-POOR path is not implemented. In pinned `LaneRenderer`,
  a configured `dstnote2` changes ordinary-note eligibility by judgement state
  and then renders the missed-POOR descent separately. The current projection
  has only judged/dead state, not the required source judgement-state and
  descent timing.

### Existing parser and renderer work

- Preserve Beatoraja `TimeLine` speed-object state in `bms-parser-cpp` and
  carry the frame-local interpolated speed into shared gameplay authority.
  Pinned `LaneRenderer.getCurrentSpeed` combines it with `#SCROLL`; the parser
  currently exposes no speed-object model, so synthesis here would not be
  source-faithful.

- If AsoBMaShow adds selectable constant-speed play, carry its state through
  gameplay authority and map Boolean property `400` (`OPTION_CONSTANT`).

- Implement configurable Lift and Hidden planes and the source
  `isChangeLift` input path. The current Start+Select edge is retained, but
  neither source configuration nor renderer plane exists.
