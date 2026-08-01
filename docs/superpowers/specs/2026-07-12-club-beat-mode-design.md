# Club Beat Mode Design

## Goal

Add an optional synthetic club rhythm layer to gameplay and the music player.

- Bass kick on every chart quarter-note beat.
- Clap on measure beats 2 and 4 when those beats exist.
- Exact alignment across BPM changes, stops, seeks, and playback rates.
- Gameplay Club mode never lowers or replaces the earned clear mark.

## Settings and UI

Club mode is independent of playback rate and remains available at 100%.

- `gameplayClubModeEnabled` is shown as a standalone checkbox in the gameplay Ready options, outside the Assist Option and playback-rate assist card. It remains enabled for courses and applies to practice.
- `musicPlayerClubModeEnabled` is shown as a checkbox in the music-player transport, separate from the Pitch Shift/Time Stretch and rate dropdowns.

Both settings persist independently. Fixed conservative mix levels are used initially; this feature does not add volume sliders.

## Shared Beat and Sound Model

A platform-neutral club-beat module owns:

- Chart beat extraction from parsed measure/timeline timing.
- Beat-in-measure numbering.
- Deterministic kick synthesis.
- Deterministic clap synthesis.

The beat grid walks quarter-note positions in each parsed measure, uses timeline timing to honor BPM changes and stops, schedules kick on every beat, and schedules clap only on beats 2 and 4.

The kick is a short decaying sine with a downward pitch envelope. The clap is deterministic filtered noise with short staggered bursts. Both generators accept the target sample rate so gameplay and rendered output use the same sound design.

## Playback Paths

### Gameplay and Practice

The Jukebox loads the generated sounds as virtual resources and schedules them from the shared beat grid. Club events use the BGM bus, follow seek overlap behavior, and are unaffected by input keysound settings.

The Ready checkbox is passed through gameplay startup independently of `audio::PlaybackRate`. Course validation does not lock or reject it.

### Music Player

Chart music cache APIs accept a Club variant flag and produce distinct normal and Club cache paths. The Club variant mixes the generated beat layer into the rendered chart audio.

Toggling Music Player Club while a track is loaded asynchronously prepares the alternate cache, reloads it, restores the source position, and resumes only if playback was active. Adjacent preload and cache pruning use the selected variant.

### Replay and Export

`ReplayData` stores an optional Club flag with a legacy default of false. The flag reproduces Club audio during replay and replay/autoplay video export, but it is excluded from assist and clear-mark calculations.

Chart audio export accepts a Club render option and mixes the same shared beat plan and synthesis into the output WAV before video encoding.

## Validation

Focused tests cover beat placement through meter, BPM changes, and stops; deterministic synthesis; normal/Club cache separation; settings and replay round trips; and unchanged clear-mark eligibility. Desktop focused tests and `main` must pass. Mobile builds are only required if implementation introduces platform-specific code.

