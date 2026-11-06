# Skin practice compatibility audit — 2026-11-06

Reference: local `../beatoraja`, commit
`83574048ba489082eb46df7fe9806b464bf1f526`.

This audit compares skin-facing practice behavior, including its setup inputs,
properties, clocks, and rendering. It preserves the native practice product's
chart-time storage, selectable count-in, looping, results, and analytics.
It is a source comparison plus behavioral regression coverage, not a claim that
every third-party skin has been exercised on a device.

## Corrections

| Area | Difference corrected | Reference implementation |
| --- | --- | --- |
| Section range | Frequency was applied to the boundaries even though native chart timestamps were never scaled. Keep the selected original-chart range while the audio clock changes speed. | `play/BMSPlayer.java`, frequency conversion followed by `PracticeModifier` |
| Setup input | Add mode-specific key/arrow/wheel navigation, held-key repeat, coarse/fine analog ticks, correct horizontal scratch direction, and DP/Pop'n modifier mappings. | `play/PracticeConfiguration.java`, `PracticeModeControls` |
| Pop’n mode | Preserve five-key identity inside the native nine-lane layout so practice help and graph colors do not use the nine-key rules. | `play/SkinPractice.java`, `skin/SkinNoteDistributionGraph.java` |
| Loading deadline | First-lane start respects authored load-start plus load-end time, and Start/Select cannot shorten that deadline. | `play/BMSPlayer.java`, `STATE_PRACTICE` |
| Skin time | Use absolute frequency-adjusted chart position for skin elapsed time and graph cursors, with the source preroll/tail and end margin. Skin animation/event timestamps use wall time; native gameplay and replay timestamps stay in chart time. | `play/BMSPlayer.java`, `STATE_READY`, practice `playtime` |
| Loading options | Practice setup is outside PRELOAD even while media is loading. | `skin/property/BooleanPropertyFactory.java`, options 80/81 |
| Rival data | Practice exposes no rival ScoreData, even when selection retained a target. | `play/BMSPlayer.java`, practice target initialization |
| Song metadata | Static total-note, distribution, and SongInformation properties retain original-chart values; score rates and judgement graphs use attempt data. | `song/SongData.java`, `skin/property/IntegerPropertyFactory.java`, `skin/SkinNoteDistributionGraph.java` |
| BPM | Current BPM and lane-duration properties reflect practice frequency; static song BPM metadata stays original. | `play/BMSPlayer.java`, `skin/property/IntegerPropertyFactory.java` |
| Character motion | Start neutral motion during setup, clear attempt motion timers on returning, and reset previous-note counters for another attempt. | `play/BMSPlayer.java`, `PomyuCharaProcessor.init` |
| Rhythm | Preserve the source's integer `100 / frequency` section-line factor, including its truncation behavior. | `play/RhythmTimerProcessor.java` |
| Authored Practice | Suppress BGA media when the skin owns a Practice object. Respect static removal and perform runtime preparation before updating a positive-item viewport. | `play/SkinBGA.java`, `play/SkinPractice.java`, `skin/Skin.java` |
| Legacy graphs | Retain separate graph textures for normal/judge/early-late views, use the source update intervals, and preserve negative-marker/positive-frequency guards. | `play/SkinPractice.java`, `skin/SkinNoteDistributionGraph.java` |

Configuration row domains, gauge categories, random options, visible-row limits,
practice item property IDs, event IDs, scroll semantics, labels, legacy geometry,
and prepared font character coverage were also compared. Tests exercise setup,
playback, return to setup, speed changes, and graph switching.

## Deliberate differences retained

- Aso owns chart-time ranges, presets, selectable count-in, loops, result capture,
  and analytics. It does not replace these with beatoraja's automatic return to
  `STATE_PRACTICE` after every attempt.
- Native touch menu controls and Start Section remain available. A brief valid
  first-lane touch is latched through the next update rather than requiring the
  contact to remain down between event processing and rendering.
- The requested Litone12 no-BGA fallback keeps a read-only legacy panel visible
  when the skin hides every playback BGA destination. This intentionally extends
  upstream visibility behavior; it does not re-enable setup input.
- A bundled system font and platform audio/rate implementation replace Java's
  configurable system-font/audio backends.
- Invalid native ranges remain bounded; private Java JSON configuration files
  are not imported as a compatibility format.

## Verification

- Desktop `main` and affected test targets built successfully.
- The 148-test union of `area_practice`, `area_input`, `area_skin_runtime`, and
  `area_rendering` passed across the group run and focused correction rerun.
- Additional chart-model and resource-catalog checks passed (150 distinct tests
  total); the visual-state check also passed again in the focused run.
- The lane-cover scene-input Python checks passed.
- No device visual retest or distribution deployment was performed.
