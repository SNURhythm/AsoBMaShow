# Orientation-specific presentation settings

Status: approved for implementation planning, including the subsequent song-selection layout request. Product code has not changed.

Confirmed preference: portrait built-in lanes default to a flat 0-degree angle.

## Song-selection layout

In portrait, show Library and Songs side by side in an upper row, with a taller full-width Details pane below. Give Library approximately 30% of the upper row's width and Songs the remainder. Give Details approximately 40% of the usable content height instead of its current fixed 340-unit height. Account for safe areas and keep the primary actions reachable while detail content scrolls. Preserve selection, list scroll positions, and preview playback when resizing. Landscape retains its existing three-column arrangement.

## Intended behavior

Each player profile owns independent landscape and portrait presentation settings. Rotating in menus selects the matching presentation. Gameplay captures that selection at entry and keeps it through pause and direct retry, alongside the existing orientation lock. Auto is an orientation policy, not a third presentation profile.

Separate by orientation:

- Built-in lane angle, length, width per key mode, beam length, note-start/lane-cover settings, lift/hidden geometry, judgement placement and display, counter placement, and gauge placement.
- Selected skins for each supported screen/key-mode target, plus each skin's options, files, offsets, and viewport configuration. Existing per-target separation remains intact.

Keep calibration, green number/hispeed preferences, input mappings, audio, rulesets, records, installed skin packages, and skin safety policy shared.

## Approach

Use two presentation blocks within each player profile. This separates the requested visual settings while retaining shared play preferences and records. Duplicating entire player profiles would also separate unrelated settings and records. Keeping one settings block with orientation-specific defaults would not preserve edits independently, so it would not meet the request.

## Defaults and accepted ranges

Introduce one orientation-aware geometry policy used by settings inputs, reset buttons, persistence validation, and renderer validation. Updating only the settings UI would leave valid portrait values silently clamped elsewhere.

The portrait angle is confirmed. The length and width numbers below are proposed starting values; projection verification must establish that the defaults and accepted range endpoints are usable before shipping:

| Setting | Landscape default / range | Proposed portrait default / range |
| --- | --- | --- |
| Lane angle | 13.4 degrees / 0–28 | 0 degrees / 0–28 |
| Lane length | 8 / 5–12 | 16 / 4–32 |
| Play-area width per key mode | 8 / 4–12 | 8 / 2–16 |

Normalized positions stay within 0–1 and percentages within 0–100. Skin-authored constraints remain authoritative; orientation separation does not override a skin's own option definitions.

Portrait camera framing must account for lane length, width, angle, viewport aspect, and safe area. Merely increasing the lane-length maximum with the current camera would permit clipping. Keep the judgement line and all playable lanes reachable, with matching projected touch regions. Preserve existing landscape projection behavior.

## Storage and migration

Use named landscape/portrait presentation blocks inside the existing player settings document. Do not duplicate whole player profiles or their records. Keep orientation policy separate from the orientation currently being displayed.

Preserve legacy built-in settings in landscape. Initialize portrait with its own geometry defaults. Seed portrait skin selections/configurations from the existing settings once, by value; subsequent changes are independent. Existing values remain recoverable in landscape. Loading and saving must preserve both blocks, including when the other orientation is inactive. Profile copy, import, and export carry both blocks.

## Activation and asynchronous saves

Orientation selection must happen before scene updates and input handling for the new viewport. Settings show which orientation is being edited; reset affects that orientation only. Finish edits against the orientation where editing began before replacing controls after rotation.

Skin activation, configuration writes, and persistence jobs capture their originating player profile, orientation, target, and generation. A late completion may update its original orientation's stored settings, but must not replace the currently active orientation's skin or settings. Switching orientation must use the existing skin activation and lease lifecycle, not directly swap mutable settings beneath a live skin session.

Preserve existing configuration-digest semantics. Orientation is part of the owning context; identical configurations may retain identical content digests.

## Verification

- Legacy migration, independent round trips, reset, invalid values, and profile copy/import/export.
- Rotation away and back restores each orientation's values and skin selection.
- Delayed saves and activation completions cannot overwrite another orientation or player profile.
- Gameplay entry/pause/retry/exit retain and release the correct orientation profile.
- UI, load/save, and renderer agree on accepted ranges and defaults.
- Portrait projection and touch geometry remain usable at representative phone/tablet aspect ratios and range endpoints; landscape behavior remains unchanged.
- Focused desktop tests/build first. Run platform builds sequentially; iOS device testing remains with the user as requested.
