# Gameplay Skin Trait Settings Design

## Goal

Replace the flat Gameplay Skins entry list with gameplay-key-mode tabs. Each
tab selects one skin for its matching play trait and exposes only that skin's
configuration controls. Starting the app or opening Settings must not scan the
user's Skins directory.

## Scope

This product currently executes only gameplay skins. The Settings tab therefore
shows gameplay traits only, not Beatoraja's Music Select, Decide, Result,
Key Config, Skin Select, Sound Set, or Theme types.

The trait mapping is pinned to Beatoraja `SkinType.java` at
`c2ed5db1a46145ed10790c3872f717e95b59db9d`:

| Lua header type | Trait label | Chart key mode |
| --- | --- | --- |
| 1 | 5K | 5 |
| 0 | 7K | 7 |
| 4 | 9K | 9 |
| 3 | 10K | 10 |
| 2 | 14K | 14 |
| 16 | 24K | 24 |
| 17 | 24K Double | 48 |

Battle types are excluded because the app does not expose a battle gameplay
mode. Unknown and non-gameplay header types remain catalogued with their
diagnostics but cannot appear in a selectable trait panel.

## UI

The existing Gameplay Skins Settings tab keeps its install controls and explicit
Rescan action. Beneath them it becomes a two-column surface:

- A fixed vertical tab column lists `5K`, `7K`, `9K`, `10K`, `14K`, `24K`, and
  `24K Double`; empty traits remain visible so users know where a later import
  belongs.
- The selected tab's right panel begins with a dropdown. It offers Built-in
  presentation plus every validated catalog entry whose header type matches
  the selected trait.
- Selecting Built-in clears only that trait's external selection. Selecting a
  skin validates and persists only that trait's selection, then the body shows
  that skin's categories, options, files, offsets, and layout controls.
- A skin's configuration controls are never shown in another trait's panel.
  Invalid and unsupported catalog entries remain visible only in diagnostics;
  they never become dropdown choices.
- Diagnostics and package-management actions remain available below the trait
  panel. Revalidate and Remove continue to operate on the selected entry.

The viewport Fit, Stretch, and Custom controls remain per selected entry. No
control is renamed or given a new behavior merely to fit the new layout.

## Data and Runtime

`SkinProfileSettings` gains an authoritative mapping from canonical gameplay
header type to selected entry. Entry-specific configuration continues to use
the existing `entries` map. The old single-7K fields remain derived
in-memory compatibility aliases while package recovery and activation migrate;
the persisted legacy `selected7KeyEntry` is read as the 7K mapping during
migration and is no longer emitted by new saves.

Validation becomes gameplay-type aware: a Lua entry is selectable when its
decoded header has one of the supported gameplay types above and the existing
configuration reconciliation succeeds. Package reconciliation, removal, and
activation must validate every selected trait rather than treating 7K as a
special case.

The chart handoff receives the chart key mode. It resolves the mapped trait,
uses its selected entry if present, and otherwise leaves the built-in
presentation active. The previous `chart->Meta.KeyMode != 7` gate is removed.

## Scan Policy

Successful catalog recovery is the startup source of truth. The lifecycle marks
that recovered catalog usable without a scan, then validates a selected entry
only when an explicit lifecycle action requires it. It never schedules a
startup scan. Entering the Gameplay Skins tab only refreshes the controller
projection; it never schedules a scan. The explicit Rescan button and a
successful install/removal remain the only catalog-refresh paths.

## Error Handling

- A trait with no valid external candidate renders `Built-in presentation` and
  no configuration body.
- A selection whose validation fails leaves the prior durable selection
  unchanged and shows the existing typed diagnostic.
- Unknown, stale, removed, or wrong-type profile mappings are removed during
  settings sanitization and fall back to the built-in presentation.
- Existing catalog recovery failure handling remains unchanged; this design does
  not hide service-unavailable errors behind a scan attempt.

## Tests

- Unit-test Beatoraja-pinned header-type to trait/key-mode mapping, tab order,
  and dropdown candidate filtering.
- Add profile persistence and migration tests for independent 5K/7K/10K/14K
  selections and legacy 7K input.
- Extend controller tests so selecting one trait cannot overwrite another,
  Built-in clearing affects only the active trait, and settings edits remain
  entry-local.
- Extend lifecycle and gameplay-start tests to prove no startup/Settings-entry
  rescan is requested, recovered catalog acquisition works without a scan, and
  a chart requests the matching trait for each supported key mode.
- Keep the fast desktop build as the primary verification loop. Run the
  unsigned iOS release verifier after the native lifecycle/context handoff
  changes; do not deploy.
