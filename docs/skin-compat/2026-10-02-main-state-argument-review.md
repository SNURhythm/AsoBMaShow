# Main-state argument review — 2026-10-02

Baseline: AsoBMaShow `9b04e93d`. Reference: local Beatoraja
`c2ed5db1a46145ed10790c3872f717e95b59db9d`,
`skin/lua/MainStatePropertyLuaApiExporter.java`, and its bundled
`luaj-jse-3.0.2-custom.jar`.

## Findings and changes

| Boundary | Reproduction | Change |
| --- | --- | --- |
| Property IDs | `number(4294967386)` clamped to 2147483647 and failed; LuaJ selects ID 90. | Apply LuaJ long conversion and low-32-bit narrowing to option, number, float_number, text, offset, timer, event_index, and judge arguments. Preserve each property domain. |
| Event dispatch | Wrapped IDs targeted the wrong event; nil and boolean arguments raised type errors. | Apply the same conversion to the event ID and its zero to two arguments before existing dispatch. Nonnumeric values become zero. |
| Key lookup | `key_pressed("29")` did not recognize the numeric string. | Preserve the parser's numeric recognition result so valid numeric strings select key codes and other strings select key names. ASCII spaces are accepted; tabs and newlines are not. |
| Volume arguments | A tab-prefixed decimal was accepted by LuaJIT, and `"0x-1"` became zero. | Use LuaJ string conversion before float narrowing and forwarding to the bound state. |

The parser now exposes an optional number while retaining the existing
conversion-to-zero wrapper for other callers. Numeric zero remains distinct
from an invalid string. The pinned jar confirms that `"-"` and `"0x-"` are
recognized as numeric zero, while `"+"`, empty strings, and tab/newline-prefixed
or suffixed strings are nonnumeric.

Independent review found one additional scanner mismatch: hex punctuation
`[`, backslash, `]`, `^`, `_`, and backtick are digit aliases 4 through 9 in
the bundled LuaJ scanner. A jar probe confirmed all six values; a new host test
failed before correcting the digit mapping and passed afterward. The correction
also applies to existing callers of the shared scanner.

## Scope and limits

This round covers argument conversion at the host API boundary. Existing
unsupported-property failures, event executor errors, and writable-state checks
remain in place. It does not change backend audio value clamping or persistence.

The existing safe zero fallback for bare `"0x"` is retained; numeric-key lookup
treats it as nonnumeric. The pinned jar throws an array-bound exception for that
input. The existing event argument-count guard is also retained rather than
reproducing the upstream VarArgFunction recursion for unsupported arities.

## Verification

- Host-boundary regression run before production changes: 12 failures, covering
  all eight property/index wrappers, event dispatch, key lookup, and volume input.
- Host-module suite after changes: passed.
- All-target desktop build: passed.
- Initial full parallel CTest: 409/409 passed before the hex scanner correction.
- Independent review: identified the hex scanner gap above, then reviewed its
  correction with no remaining actionable findings.
- Final all-target build after that correction: passed.
- Final full parallel CTest: 409/409 passed (63.72 seconds).
- `git diff --check`: passed.

External skins, Lua file persistence, and LITONE-specific tests were unchanged.
No mobile deployment was performed.
