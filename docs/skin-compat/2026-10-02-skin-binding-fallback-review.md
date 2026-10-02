# Skin binding fallback review — 2026-10-02

Baseline: AsoBMaShow `d94f9a82`. Reference: local beatoraja
`c2ed5db1a46145ed10790c3872f717e95b59db9d`.
This continues the requested source comparison and regression-fix loop.

## Findings and fixes

| ID | Reproduction | Change |
| --- | --- | --- |
| B1 | Unknown numeric properties, invalid value types, or broken script strings could suppress a valid numeric/text `ref`. | Resolve the authored property first, then use the loader's `ref` fallback when resolution returns null. Lua resource failures retain their fatal status. |
| B2 | A float display's explicit `value` used the broader FloatValue factory. For example, value 1107 resolved locally although the serializer's Rate factory returns null. | Explicit values use Rate; the numeric `ref` overload uses FloatValue. A null value 1107 can therefore fall back to ref 17, while ref 1107 remains supported. |
| B3 | Invalid text `event` fields suppressed the implicit ref writer and editability. JSON also accepted numeric StringWriter selectors despite lacking that serializer factory. | Fall back after writer resolution; enable implicit editability when a ref writer exists. Valid explicit writers preserve precedence. Reject authored numeric JSON StringWriters. |
| B4 | JSON slider/graph constructors were selected by raw value presence. Unknown value 999999 prevented type 17 fallback; implicit sliders incorrectly used authored events. | Select the constructor from the resolved value. Null values use the rate or integer-range type overload. Implicit sliders derive their writer from type. |
| B5 | Explicit slider value 17/event 17 with changeable false retained a writer but blocked interaction locally. | Match the explicit-value constructor, whose supplied writer is independent of the changeable flag. |
| B6 | Image-set value 90 and ref 90 both used ImageIndex, conflating maximum BPM with the favorite-image index. | Explicit image-set values use IntegerValue; ref uses ImageIndex. Unknown explicit values fall back to ref in its own domain. |

Reference paths under `src/bms/player/beatoraja/`:

- `skin/lua/LuaSkinLoader.java`: serializer map and `serializeLuaScript`.
- `skin/json/JsonSkinSerializer.java`: property serializers and `LuaScriptSerializer`.
- `skin/json/JsonSkinObjectLoader.java`: image-set, number, float, text,
  slider, and graph constructor selection.
- `skin/SkinImage.java`, `skin/SkinFloat.java`, and `skin/SkinSlider.java`:
  numeric-ref factories and slider input handling.

## Review and verification

- Added generic Lua/JSON decoder fixtures for all six findings. Tests observed
  failures before each production fix, including separate slider and image-set
  follow-up cases discovered during review.
- Each fixture checks all 20 destination objects are retained, plus resolved
  selectors, property domains, writer precedence, editability, constructor
  selection, and slider interaction eligibility.
- Lua cases use the real Lua runtime with a small explicit builtin catalog.
  JSON unit cases use the production builtin catalog and a script compiler seam;
  the full suite also includes real scripted JSON sessions and rendering.
- A focused session test caught an overly broad diagnostic downgrade during
  development. Narrowed it to optional FloatProperty/StringWriter resolution;
  existing timer-factory failure admission checks pass.
- `cmake --build cmake-build-debug --target all -j 6`: passed.
- `ctest --test-dir cmake-build-debug --output-on-failure -j 6`:
  **409/409 passed** (66.70 seconds).
- After adding the final destination-count assertions, both decoder targets
  rebuilt and their two CTest entries passed again. `git diff --check` passed.

The final local review compared serializer factories, resolved-null fallbacks,
property domains, and constructor-controlled interaction. No further confirmed
gap remained in those reviewed paths. This was a local review, not an independent
review or an exhaustive proof of compatibility across external skins.
