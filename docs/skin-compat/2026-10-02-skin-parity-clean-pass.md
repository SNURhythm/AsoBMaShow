# Skin parity clean pass — 2026-10-02

AsoBMaShow baseline: `3af57ca4`. The reference checkout was updated from
`c2ed5db1` to upstream `ad42f56c4658e968f93b24bf23440fe51cb9878e` during this
review. Final behavior follows the updated source. Earlier audit reports retain
their original reference versions.

## Changes

| Area | Confirmed difference and correction |
| --- | --- |
| Host arguments | Audio volumes and both direct/legacy HTTP timeout calls use LuaJ numeric parsing and Java integer conversion before their existing bounds. |
| Audio state | Direct volume setters work in gameplay, selection, result, and course result. Configured writes publish with the first submitted frame; callbacks read staged values, including named slider writers. Audio playback uses the current frame’s system volume. Photo/video output keeps volume actions isolated. |
| Hidden text | Runtime-hidden text still evaluates its value callback; constructor-pruned objects remain removed. Nested source timers retain the source prepare order. |
| Song-list numbers | Synthetic nested number objects retain the numeric constructor reference independently of a standalone value callback. |
| Font blend | Scalable/bitmap text inherits the preceding drawn object’s retained blend; image fonts use ordinary image blend. Empty note lanes, transparent gauge nodes, and zero-rate graphs retain their source draw-state behavior. Session state publishes only after successful submission; cancelled preparation, rejected backend preflight, and photo export preserve the live blend. |
| Pointer visibility | `mouseRect` now uses retained passive pointer position, authored coordinates, runtime offsets, inclusive edges, and the source’s distinct nested prepare/draw offsets. A shared input snapshot preserves a stationary pointer across scene/session replacement and applies current viewport transforms on each render. Pointer release preserves the preceding position, matching the source. |
| Image filtering | Scaled filtered images use Beatoraja’s alpha-preserving bilinear shader. Actual Metal readback checks transparent edges, fractional alpha, tint, and both interpolation axes. BGA explicitly restores hardware sampling when it shares the shader program. |
| Destination resolution | Compatibility geometry scales into the selected logical destination canvas before stretch and rotation. Custom transforms remain afterward. Flipped regions use positive intrinsic dimensions for unity-filter decisions. Standard geometry retains its existing behavior. |
| Font filtering | Scalable, bitmap, distance-field/fallback, and LR2 image fonts keep their separate filtering rules. Mixed image-font glyphs split adjacent runs when sampling differs. |
| Current score rates | Best/rival progress uses the authoritative stage passed-note count. Best ghosts remain valid; rival progress follows BMSPlayer’s no-ghost contract. The updated reference now derives rates from integer-projected scores, so the old fractional-rate proposal was discarded. |
| Updated Lua API | Direct properties accept factory names and return typed defaults for missing factories; `float_number` uses the full float domain. Added variadic `numbers`, timer state/elapsed helpers, and screen dimensions. Internal bridge-only names remain private. |
| Result input snapshots | Result/course headers, configured Lua, and rendered frames receive current input dimensions. Video export supplies its own dimensions and empty input state. Actual export-caller tests cover the wiring. |
| Historical reference checks | Existing versioned oracle traces keep their original source pin. Their tools now materialize that exact commit from Git history into temporary snapshots, so updating the sibling checkout does not invalidate historical evidence or require changing its HEAD. |

## Review and verification

Regression checks reproduce each confirmed difference without its correction.
Focused tests cover actual Lua host calls, session configuration/rendering,
command lowering, backend batching, and real Metal pixels. Independent reviews
checked the host/state work, shader/destination math, and renderer lifecycle.
The final full build passed with `cmake --build cmake-build-debug -j 6`.
`ctest --test-dir cmake-build-debug --output-on-failure -j 6` passed all 410
tests in 66.25 seconds, including actual Metal readback and BGA sampling checks.
The photo-export regression also failed for both result types when its blend
restoration was temporarily removed, then passed with the production code
restored.

[Windows shader compilation](https://github.com/SNURhythm/AsoBMaShow/actions/runs/36994706846)
passed and produced the committed Metal, SPIR-V, ESSL, and DirectX artifacts.
The manifest verifier passed for all four backends after adopting those outputs.
The final independent source reviews found no further actionable issue in the
reviewed Lua/state/audio, renderer/filtering/lifecycle, pointer, and reference
tooling paths. `git diff --check` passed. Platform device builds were not part
of this desktop verification pass.

## Deliberate distinctions

The updated upstream still changes blend equations while flushing previously
queued sprites and mutates filters on shared textures. AsoBMaShow keeps its
existing documented corrected subtractive blend and explicit sampler state.
Reproducing those side effects would require Java SpriteBatch queue and
per-texture history emulation. The new bilinear shader and font rules match
the intended draw path without adding that history model.

Fit/custom viewport settings are application features. Their logical canvas
and final transform stages are explicit; physical pixel density does not
silently alter authored stretch rules. Standard safety policy, unavailable
application features, and external-skin-specific coverage remain outside this
bounded compatibility pass. A clean review is not proof of equivalence for
all possible third-party skins.
