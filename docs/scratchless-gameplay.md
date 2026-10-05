# Scratchless 5K and 7K

Skin settings distinguish **5K / 7K** from the original **5K1S / 7K1S** modes
(`S` means scratch). A single-play 5- or 7-key chart with no scratch content
uses the corresponding scratchless skin selection. Normal scratches, long
scratches, invisible scratch keysounds, and scratch mines prevent this switch.
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
modes remain canonical. Canonical chart-mode labels explicitly include `1S`;
the separate settings targets are labeled `5K` and `7K`.
