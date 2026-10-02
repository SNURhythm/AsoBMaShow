# Beatoraja skin parity audit — 2026-10-02

Baseline: AsoBMaShow `303d8277`; local beatoraja
`c2ed5db1a46145ed10790c3872f717e95b59db9d`. These nine P2 findings were
confirmed by source comparison in BeatorajaCompatibility mode. The implementation and validation record are tracked below.

External skin files and Lua file persistence are outside this change.

| ID | Mismatch and observable effect | Local implementation | Upstream reference | Status |
| --- | --- | --- | --- | --- |
| L1 | `timer_util` and `event_util` are empty; calling their helpers fails. Implement all six timer and five event exports with upstream closure behavior. | `LuaSkinHostModules.cpp`, module installation | `skin/lua/SkinLuaAccessor.java:900`, `TimerUtility.java`, `EventUtility.java` | Fixed |
| L2 | Identical timer-factory strings construct one shared timer instead of separate closures. Two calls to a serial-number factory should produce 1000 and 2000, not 1000 twice. | `LuaSkinBindingDecoder.cpp:244` | `skin/lua/LuaSkinLoader.java:118`, `SkinLuaAccessor.java:712` | Fixed |
| C1 | Every course-stage clear lamp becomes NO PLAY, including Full Combo, Perfect, and Max. Upstream only changes Failed to No Play. | `scene/ResultScene.cpp:621`; corresponding video export path | `play/BMSPlayer.java:863`, `result/MusicResult.java:368` | Fixed |
| R1 | Duplicate offset IDs apply twice. With x=100 and offset x=10 listed twice, actual x=120 instead of 110. | `Skin2DRenderer.cpp:4650`; destination decoders | `skin/SkinObject.java:832` | Fixed |
| R2 | Step animations apply alpha offsets between changing-color keyframes. At t=500 between alpha 128 and 255, acceleration 3 and offset alpha −200 yield 0 instead of 128/255. | `SkinDestinationEvaluator.cpp:376` | `skin/SkinObject.java:496` | Fixed |
| R3 | Sorting frames loses authored acceleration precedence. Authored frames (t=1000, x=100, acc=1), (t=0, x=0, acc=2) yield x=75 at t=500 instead of 25. | `SkinDestinationEvaluator.cpp:42`; JSON/Lua/LR2 destination sorting | `skin/SkinObject.java:218` | Fixed |
| S1 | Selected-score integer/float judgment percentages 85–89 are absent. 250 PGREAT of 500 notes should return 50 / 0.5. | `music_select/MusicSelectPropertyProjection.cpp:380` | `skin/property/IntegerPropertyFactory.java:505`, `FloatPropertyFactory.java:363` | Fixed |
| S2 | Course bars skip graph rates 140–145 and 147. For 1000 notes, PG=500, combo=612, EX=1450, expected rates include 0.5, 0.612, 1.45. Upstream course EX rate intentionally has no division by 2. | `music_select/MusicSelectPropertyProjection.cpp:526` | `skin/property/FloatPropertyFactory.java:550,165` | Fixed |
| S3 | Selected-score integer aliases 115/116 and 155/156 return unavailable although equivalent score rates exist. EX=800 / maximum=1000 should give 80 for integer 155. | `music_select/MusicSelectPropertyProjection.cpp:396` | `skin/property/IntegerPropertyFactory.java:187,200,691` | Fixed |

Local skin filenames without a directory above are under `src/skin/beatoraja/`.
Upstream paths are relative to `src/bms/player/beatoraja/` in the pinned checkout.

## Implementation details

- Lua utilities now provide six timer helpers and five event helpers. Each
  helper owns its closure state; event adapters discard invocation arguments
  exactly as upstream does. Tests cover the microsecond clock, initial OFF
  edges, repeated calls, reset behavior, minimum-interval boundaries, Java
  integer wrapping, and LuaJ string coercion. The selection bridge now exposes
  its frame clock to these helpers.
- Scripted timer factories execute for every authored property. Numeric and
  ordinary function bindings retain their existing handling.
- Destination evaluation deduplicates offsets in both main and nested object
  paths. Decoders preserve the first nonzero authored acceleration before
  sorting timestamps. Step animations now skip alpha offsets between changing
  color keyframes while retaining endpoint and fixed-color behavior.
- Selection projections populate judgment percentages, integer score aliases,
  and course graph rates. Integer score fractions use Java float rounding
  (EX 813 / maximum 1000 produces 81 and 30). Scored zero-note course graph
  rates preserve upstream IEEE infinity/NaN; absent scores return zero.
- Interactive course results and video exports share stage-clear projection.
  Stage-local judgments determine combo achievements even when maximum combo
  carries from a previous stage. Skins receive clear IDs 8/9/10 for Full Combo,
  Perfect, and Max; ordinary and failed stages retain No Play. Gauge survival
  still determines the separate clear/fail animation. Assisted or modified
  playback cannot promote a stage to Full Combo.

## Verification record

Each original finding was reproduced by a failing regression before its fix.
Focused renderer/decoder, selection, Lua binding, and course-result suites have
passed. Additional coercion regressions reproduced the independently reviewed
LuaJ string discrepancy and passed after the parser correction. The original
runtime fixture's empty-module assertions were updated to exercise the supported
helpers while retaining all capability checks.

Final validation on 2026-10-02:

- `cmake --build cmake-build-debug --target all -j 6`: passed, including `main`
  and all test executables.
- `ctest --test-dir cmake-build-debug --output-on-failure -j 6`: **409/409 passed**.
- `git diff --check`: passed.
- Independent source reviews covered Lua semantics and the course/selection
  changes; both identified boundary cases are included in the passing regressions.

Regression files:

- `tests/lua_skin_host_modules_tests.cpp`
- `tests/lua_skin_binding_decoder_tests.cpp`
- `tests/fixtures/beatoraja_skin/packages/runtime_contract/skin/forbidden_capabilities.luaskin`
- `tests/skin_draw_command_tests.cpp`
- `tests/skin_destination_evaluator_tests.cpp`
- `tests/beatoraja_gameplay_cross_format_tests.cpp` and authored-acceleration fixtures
- `tests/music_select_property_projection_tests.cpp`
- `tests/music_select_skin_state_bridge_tests.cpp`
- `tests/play_skin_session_tests.cpp` (generic course-stage regression)

No external skins, Lua file persistence, or LITONE-specific tests were changed.

## Scope limits

Deduplication preserves the existing order of distinct offset IDs. It does not
reproduce LibGDX's nondeterministic cuckoo-hash iteration order.

The existing configured-loader binding lifetime is unchanged. Ordinary
configured skin execution and frame callbacks have a state clock; a string
factory that actively reads that clock during the later decoder trial can
still lack a bound state. This is a separate lifecycle issue from the duplicate
factory-instance finding L2.
