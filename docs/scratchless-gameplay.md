# Scratchless 5K and 7K

Skin settings distinguish **5K / 7K** from the original **5K1S / 7K1S** modes
(`S` means scratch). Settings → Skins → **5K** or **7K** includes a
**Scratchless mode** preference for that mode. Each independently controls
when a chart without scratch content uses its skin and input settings:

- **Disabled**: always use 5K1S / 7K1S, including charts with no scratches.
- **Enabled**: use 5K / 7K for every eligible chart.
- **Enabled for selected difficulty tables** (default): select any number of
  installed tables using independent checkboxes. A chart qualifies when its
  MD5 or SHA-256 belongs to any selected table, regardless of which folder it
  was launched from. Both **5KEYS AERY** and **7KEYS AERY** are selected by
  default for their respective modes: 5K selects only Aery 5K, and 7K selects
  only Aery 7K. Selecting no tables disables automatic scratchless selection
  for that mode.

Each mode’s policy and table selections are saved independently per player.
The controls are available with Built-in, Follow, and imported skins. Live play, retries,
replay viewing, and replay video exports use the same policy. Layout previews
still allow editing either mode directly.

For all enabled modes, normal scratches, long scratches, invisible scratch
keysounds, and scratch mines prevent this switch.
Play modifiers are checked against the resulting chart, too.

Each scratchless skin dropdown offers:

- **Built-in**: independent play-area width and a **Hide empty scratch lane**
  option, enabled by default. Hiding also removes the built-in scratch touch
  lane and virtual platter.
- **Follow 5K1S / Follow 7K1S**: uses the original mode's current skin and
  settings. If that selection is built-in, the scratchless mode keeps its own
  hide option while following the original width. Switching away from Follow
  restores the scratchless mode's saved width and skin configuration.
- **An imported skin**: accepts the same Beatoraja skin file type as the
  original mode, with independent options, files, offsets, and viewport.
  Choose a skin authored for scratchless play; the app does not crop or
  rearrange its lanes. The built-in hide option is not shown for imported
  skins, including when following an imported skin.

These settings are saved per player and presentation orientation. New
scratchless selections default to Built-in. Existing 5K1S/7K1S skin selections
and widths are retained.

Input settings also offer **5K / 7K**, with independent bindings and resets
from **5K1S / 7K1S**. Scratchless entries hide scratch rows and start with
standard keyboard defaults. Existing profiles gain these new defaults once;
custom bindings for the original modes are preserved. Scratchless gameplay
uses these bindings regardless of the selected skin or hidden-lane setting.

Chart identities, judgements, scores, replay formats, and IR protocol key
modes remain canonical. The chart list labels single-play charts with no counted
scratch or backspin notes `5K` / `7K`; charts with either are labeled `5K1S` / `7K1S`. This
metadata-only label does not change gameplay skin selection, which also checks
invisible scratch keysounds and scratch mines in the loaded chart.
