# Dropdown width regression audit

Commit `078509668` changed `DropdownView` from a fixed intrinsic width to a
minimum width, preserving caller-supplied fixed, percentage, and flex sizing.
The inner trigger still used `width: 100%`. In a start-aligned column, Yoga
resolved that percentage before the naturally sized parent had a definite
width: the dropdown was 160 units wide but its trigger was 0 units wide.
This was a regression introduced by this branch.

Commit `745dcecc1` changes the trigger to automatic width. The dropdown's
existing stretch alignment then fills the resolved container width without
the percentage dependency. This is a shared component fix, not a Language
settings workaround.

## Call-site review

All 33 `new DropdownView` sites under `src` were reviewed, including the
helpers that assign sizes after construction.

| Layout | Consumers | Result |
| --- | --- | --- |
| Natural width in a start-aligned column | General settings: Language | Collapse reproduced with the previous percentage trigger in normal and compact density; fixed by stretch sizing. |
| Natural width in rows, including wrapping label rows | Skin selection, skin options/files, skin safety; desktop chart filters | Representative row layouts did not reproduce the collapse with the previous trigger. They pass with the shared fix. Actual chart-filter trigger bounds also pass across density changes. |
| Explicit width or `setTriggerWidth` | Audio/display settings, input selectors, calibration, skin preview selectors, storage policies, music-player playback mode; compact chart filters | Existing allocation tests and component interaction tests pass with the fix. |
| Full-row percentage width | Settings section navigation, compact fixed hi-speed, built-in skin mode, scratchless mode, keybomb judgement, built-in note editor, Play Options playback mode, practice controls | Reviewed parent width assignments; percentage allocation coverage passes with the fix. |

## Verification

`play_options_panel_view_tests` now checks the inner trigger as well as the
outer dropdown. The natural-width regression covers start-aligned and
stretched columns, plain and wrapping rows, normal and compact density,
portal and inline menus, opening from the trigger, and selecting an option.
The actual chart-filter test checks that each trigger fills its control
through retained-panel density changes.

For comparison, temporarily restoring only the old percentage trigger
reproduced `outer=160 trigger=0` for both start-aligned-column density cases.
The other parent layouts in that comparison did not collapse. The fixed
source was restored before the final test run.

The earlier regression test covered caller-assigned widths and outer menu
geometry, but missed naturally sized, closed trigger buttons. That coverage
gap allowed the Language regression through.

The initial grouped run also caught four obsolete Log/Tasks translations and
a rotation-test fixture that still modeled the toolbar before its icon-only
buttons. The unused translations were removed and the fixture was updated to
model the current toolbar and assert square icon buttons across rotation.

All 37 tests in `area_ui` and `area_settings` now pass, and the desktop app
build passes. This audit used source
inspection and native layout/interaction tests; it does not claim a fresh
visual check of every settings page or physical-device testing.
