# Player Foundations Design

**Date:** 2026-07-10  
**Integration branch:** `feature/player-foundations`  
**Base:** `develop` at `600efcb`

## Purpose

Build the shared foundation required by the later non-IR player features: configurable hardware input, latency and audio/video controls, trustworthy score provenance, multiple portable player profiles, and versioned rulesets. Existing keyboard and touch gameplay, settings, scores, and replays must continue to work without data loss.

This is the first independently shippable milestone in the larger BMS-player roadmap. Practice and timing analysis, new chart formats and modes, accessibility and localization, courses, score migration, and skins will be specified and implemented as later milestones on top of these contracts. Internet Ranking and its server are explicitly excluded.

## Scope

The milestone includes:

- Per-profile, per-player, per-key-mode bindings for keyboard, SDL game controllers, analog axes, touch, and MIDI-capable devices.
- Press-to-bind configuration, live input testing, conflict detection, dead zones, analog scratch direction, and reset-to-default.
- Audio output-device, sample-rate, buffer-size, and master/BGM/keysound volume controls where the active backend exposes them.
- Display mode, resolution, VSync, and frame-cap controls where supported.
- Safe audio and display preview, rollback, and disconnected-device behavior.
- Versioned application settings with explicit migrations.
- A stable ruleset descriptor and immutable per-attempt score provenance.
- Multiple profiles with isolated settings, scores, and replays while keeping the chart/library database shared.
- Versioned profile export and import without chart assets or credentials.
- Lossless migration of current single-profile data into the default profile.
- Registration of every project test executable with CTest so one canonical command runs the complete baseline and all new foundation tests.

The milestone does not include:

- IR accounts, leaderboards, rivals, score upload, server code, or credentials.
- Practice loops, practice rate, timing analysis, or result-to-practice navigation.
- BMSON, PMS, 9-key, or 24-key parsing and gameplay.
- User-created courses, LR2/beatoraja score import, or external skin loading.
- Perfect/MAX lamps, Hazard/Light Assist, or other new gameplay rules. The schema can represent them later without another provenance redesign.

## Branch and Worktree Model

The original checkout stays on `develop`. The integration worktree is `.worktrees/player-foundations` on `feature/player-foundations`.

After the shared types and migrations are committed, task branches are created from the integration branch and checked out in separate worktrees:

- `feature/foundation-contracts`
- `feature/foundation-input`
- `feature/foundation-audio-video`
- `feature/foundation-score-profile`
- `feature/foundation-settings-ui`

Agents edit only their assigned worktree. Task branches merge into `feature/player-foundations`, never directly into `develop`. A task merge requires its focused tests and the existing desktop test suite to pass. The integration branch receives the final desktop, iOS, and Android build verification.

## Architecture

### Shared contracts

Four contracts are defined before device, UI, or persistence work proceeds.

#### `InputProfile`

`InputProfile` maps physical bindings to logical actions. A binding is scoped by player number and BMS key mode and has a stable device identifier, device class, control kind, control index, direction, activation threshold, release threshold, and optional inversion. Logical actions include gameplay lanes, scratch directions, Start, Select, pause, retry, and lane-cover controls.

Bindings never leak physical device identifiers into replays. The existing replay stream continues to record logical lane, touch, and control events so a replay remains portable to different hardware.

The default profile is generated from the current hard-coded keyboard layouts for 4/5/6/7/8/10/14-key modes. Touch remains a first-class logical input adapter and preserves the current mobile layout and flick behavior.

#### `AudioVideoSettings`

Audio settings contain a stable output-device ID, requested sample rate, requested buffer frames, master volume, BGM volume, and keysound volume. Video settings contain display mode, display index, resolution, VSync state, and frame cap.

The persisted values express user intent. Runtime capability objects report which values the current platform/backend can enumerate or apply. Unsupported settings remain preserved during cross-platform profile transfer and are not silently replaced.

#### `RulesetDescriptor` and `ScoreProvenance`

`RulesetDescriptor` is an immutable, integer-versioned description of scoring and judging behavior. Version `1` represents the gameplay behavior shipped immediately before this milestone.

`ScoreProvenance` snapshots the active ruleset version, chart hashes, LN mode, gauge type and auto-shift state, play options and random seeds for both players, assist option, judge-rank source and effective judge windows, input-device categories, autoplay/practice state, and an eligibility state.

Eligibility states are `verified`, `modified`, and `legacy-unverified`. New standard plays are `verified`; autoplay, practice, or rule overrides are `modified`; migrated records that cannot reconstruct complete provenance are `legacy-unverified`. All three remain visible locally.

The play scene creates the descriptor once when the session starts. The same immutable value is saved with the score and replay, preventing settings changed during play from altering the recorded attempt.

#### `PlayerProfile`

A profile has a generated UUID, display name, creation timestamp, last-used timestamp, schema version, and paths to its settings, score database, replay database, and input profile. The active profile ID is the only profile-specific value kept in the global application bootstrap file.

The chart/library database, difficulty-table catalog, chart roots, downloaded archives, and chart assets remain shared across profiles.

## Components and Data Flow

### Input path

Physical events flow through:

`InputDeviceRegistry` → `InputBindingResolver` → existing logical gameplay input handling.

`InputDeviceRegistry` owns enumeration, hot-plug state, stable-ID normalization, and device capabilities. SDL supplies keyboard, button, game-controller, joystick-axis, and hat events. Platform MIDI adapters normalize MIDI note/control messages into the same physical-event type. A platform without a usable MIDI API reports MIDI as unsupported while retaining imported MIDI bindings.

The milestone supplies MIDI adapters for CoreMIDI on macOS/iOS, the Windows Multimedia MIDI API on Windows, and Android's `MidiManager` through the existing SDL activity/JNI boundary. Android devices that do not expose the MIDI service report the capability as unavailable. Linux remains build-compatible at the interface level but is not a release-verification target for this milestone.

`InputBindingResolver` applies the active profile, player, and key-mode scope; performs threshold/hysteresis processing for axes; detects transitions; and emits logical press/release actions. The resolver is independent of SDL so deterministic tests can feed synthetic events.

The settings UI uses the same registry and resolver in capture mode. Capture mode shows raw and normalized state, ignores duplicate noise, detects binding conflicts, and requires an explicit confirmation before replacing a conflicting binding.

Disconnected devices do not delete bindings. A missing binding is shown with its saved stable ID and becomes active again if the device returns. Keyboard defaults remain available as an explicit fallback.

### Audio and display path

`AudioDeviceManager` exposes backend-neutral device/capability snapshots and owns transactional restart. Applying audio settings performs these steps:

1. Validate requested settings against the current capability snapshot.
2. Preserve the last working settings and playback state.
3. Pause and drain active chart playback.
4. Attempt backend restart with the requested device and format.
5. Resume on success or restore the last working configuration on failure.

Volume changes that do not require restart apply immediately. Audio offsets remain separate from device/buffer settings.

`DisplaySettingsManager` enumerates display modes and applies window mode, display, resolution, VSync, and frame cap. Risky changes use a confirmation countdown; timeout, loss of focus, or renderer failure restores the previous working state. Mobile exposes fixed display mode/resolution and only reports controls that its renderer can safely apply.

Desktop PortAudio builds expose output device, sample rate, and buffer frames. Mobile builds expose app-owned volume groups and any safe runtime audio controls supplied by their current backend; unavailable device or format controls are visibly disabled rather than simulated. Desktop SDL builds expose the full display group, while iOS and Android report fixed system-managed display capabilities.

### Profile and persistence path

Profile-independent bootstrap data resolves the active UUID. `PlayerProfileManager` then supplies profile-scoped settings and database paths to the existing application database initializer and scene construction.

Each profile uses this layout under the existing application data root:

```text
profiles/
  <uuid>/
    profile.json
    settings.json
    input.json
    scores.db
    replays.db
active-profile.json
```

The current global chart database and library settings stay in their existing locations.

On first launch after upgrade, migration creates a default profile in a staging directory, copies the current settings/score/replay data, upgrades the copies, validates row counts and SQLite integrity, atomically renames the staged profile, and finally writes `active-profile.json`. Existing source files are retained as recovery data for the milestone release and are never used after a successful migration.

Profile switching is allowed only outside an active play, replay export, scan, or database write. Switching closes profile databases, loads and sanitizes the new settings/input data, reconnects helpers using the new paths, refreshes score-backed caches, and updates the UI.

### Export and import

The portable format is a ZIP archive with this structure:

```text
manifest.json
settings.json
input.json
scores.db
replays.db
checksums.sha256
```

`manifest.json` contains format version `1`, source application version, profile UUID, profile display name, creation timestamp, and per-component schema versions. It contains no machine-specific database paths.

Export takes consistent SQLite snapshots, writes all files to a temporary archive, verifies the SHA-256 manifest, and atomically moves the completed archive to the requested destination.

Import rejects path traversal, duplicate archive members, checksum mismatch, unsupported future versions, oversized metadata, invalid UTF-8 profile names, and failed SQLite integrity checks. A colliding UUID is replaced with a newly generated UUID unless the user explicitly selected overwrite. Overwrite is implemented as an atomic staged replacement with backup. Chart assets, downloaded archives, account credentials, and IR data are never exported.

## Settings and Migration Policy

Every JSON root contains an integer `schemaVersion`. Parsers reject versions newer than the application understands. Older versions pass through sequential, idempotent migrations before domain objects are constructed. Invalid individual values fall back through the existing `sanitize()` policy and generate a diagnostic; invalid document structure leaves the last working file untouched and loads defaults.

SQLite databases continue using `PRAGMA user_version`. Schema upgrades run inside transactions and are safe to call more than once. Score and replay migrations add provenance without rewriting historical outcome values. Existing records receive `legacy-unverified` eligibility and ruleset version `0`.

Atomic JSON writes use a sibling temporary file, flush, and rename. The last working settings file is retained as a single backup generation.

## User Experience

The existing settings scene gains these groups:

- **Profile:** active profile, create, rename, duplicate, delete, export, and import.
- **Input:** player, key mode, device filter, binding grid, press-to-bind, input monitor, conflict warnings, dead-zone, thresholds, inversion, and reset.
- **Audio:** output device, sample rate, buffer frames, master/BGM/keysound volumes, test sound, effective latency, and rollback errors.
- **Display:** mode, display, resolution, VSync, frame cap, apply/confirm, and capability explanations.

Profile deletion requires another profile to exist and cannot delete the active profile until the user switches. Imported settings show unsupported device references without discarding them. Device and display lists show friendly names while persistence uses stable IDs.

## Failure Handling

- Unknown future settings, profile, export, score, or replay schema versions fail closed without modifying user data.
- Failed migrations retain the old data, remove incomplete staging data, log the failing phase, and continue with the legacy default profile when safe.
- Failed audio restarts restore the last working device/format and leave playback stopped if resumption is unsafe.
- Failed display previews restore the last working window and renderer configuration automatically.
- Disconnected input devices preserve their bindings and expose a clear missing-device state.
- Corrupt imports do not create or modify profiles.
- Failed profile switches restore the previous active profile and refresh its database connections.
- Legacy scores remain available locally and are never deleted merely because their provenance is incomplete.

## Verification

Focused test executables cover:

- Binding serialization, defaults, scoping, conflict detection, hot-plug identity, axis hysteresis, dead zones, inversion, and analog scratch direction.
- Synthetic logical-event output independent of SDL.
- Audio capability filtering, restart success, restart failure, rollback, and volume-only updates through a fake backend.
- Display preview confirmation, timeout rollback, and unsupported capability behavior through a fake backend.
- JSON migration fixtures for the current unversioned settings file and all newly versioned documents.
- Score/replay schema migrations and equality of the saved provenance descriptor.
- Default-profile migration, row-count validation, idempotency, failed-staging recovery, profile isolation, and cache refresh on switch.
- Export/import round trip, UUID collision, overwrite rollback, traversal rejection, checksum rejection, future-version rejection, and SQLite corruption rejection.

The root build enables CTest and registers each existing lightweight test executable plus every new foundation test. Test registration is conditional on `ASOBMASHOW_BUILD_TESTS`; configuration with tests disabled does not build or register them. The canonical local command is `ctest --test-dir cmake-build-debug --output-on-failure -j 6`.

Integration verification includes:

- The complete CTest suite, including the migrated existing executables and Yoga tests.
- `cmake --build cmake-build-debug --target main -j 6` from the primary checkout after integration.
- Manual desktop smoke tests for keyboard defaults, controller hot-plug, binding capture, audio rollback, display rollback, profile switching, and export/import.
- `scripts/ios_firebase_deploy.sh --build-only` for iOS without upload.
- Android build-only verification when the required private signing environment is present; otherwise the exact missing environment is reported and the Android-native C++ targets are compiled through the available local configuration.

## Acceptance Criteria

- Existing keyboard and touch behavior is unchanged when the user accepts migrated defaults.
- A controller can be bound without editing files, including an analog scratch axis with hysteresis and direction control.
- Supported audio and display settings can be previewed, confirmed, persisted, and rolled back safely.
- Every new score and replay contains the same immutable ruleset/provenance snapshot.
- Existing scores and replays appear under the default profile with unchanged outcome values.
- Two profiles have isolated settings, input mappings, scores, and replays while sharing the same chart library.
- A profile export imports into a clean installation with matching settings, scores, replays, and checksums.
- Corrupt or future-version data never overwrites working local data.
- The original `develop` checkout remains free of feature edits throughout parallel implementation.
