# Skin parity review loop — 2026-10-02

Baseline: AsoBMaShow `011d1428`. Reference: local beatoraja
`c2ed5db1a46145ed10790c3872f717e95b59db9d` and its bundled LuaJ behavior.
This follows the earlier nine-finding audit and factory-clock fix.

## Findings and fixes

| ID | Reproduction | Change |
| --- | --- | --- |
| L3 | A numeric callback returning `"12oops"` was read as 12; LuaJ returns 0. Tabs, signed hex, and overflowing integers also need LuaJ scanning rules. | Property conversion and utility arguments share the existing pinned string-number scanner. |
| L4 | Numeric string-property callbacks used C++ formatting; e.g. 1.23456789 should display `1.2345679`, and 0.0001 should display `1.0E-4`. | Numeric text uses LuaJ's integral-double handling and Java float formatting. The runtime keeps the upper signed-64-bit boundary as a double to avoid an out-of-range cast. |
| C2 | Final-course metadata reset BPM and difficulty. A last chart with BPM 180, range 150–210, difficulty 4 exposed zeros. | Interactive, replay-video, and image-export metadata retain those last-chart fields while preserving aggregate course title, length, note count, and chart count. |
| J1 | Scripted JSON values, draw conditions, timers, writers, and events were discarded as unsupported. | Script-bearing gameplay/result JSON sessions retain a configured Lua runtime and compile the shared typed bindings. State, timer utilities, and event utilities are exported globally as in JSONSkinLoader. |
| C3 | With a completed stage lacking sampled graph data, unplayed-stage zero padding could hide its durable gauge history. | Use the complete aggregate fallback when a played stage needs it; omit sample-based section markers and preserve explicit graph omission limits. |

Reference locations under beatoraja's `src/bms/player/beatoraja/`:
`skin/lua/SkinLuaAccessor.java` property loaders and global exports;
`skin/json/JsonSkinSerializer.java` LuaScriptSerializer;
`skin/json/JSONSkinLoader.java` runtime initialization;
`result/CourseResult.java` retained SongData and per-stage gauge padding;
`skin/property/IntegerPropertyFactory.java` BPM properties.

## Review passes

1. Compared the changed numeric conversions, JSON serializer dispatch, callback
   creation, and result metadata with pinned source. JSON writer/timer/event
   strings have no name-factory lookup upstream; corrected that distinction and
   changed the all-fields fixture's built-in volume writer to numeric ID 17.
   Identical timer strings construct independent closures.
2. Traced configured-session ownership, initial state binding, later frame
   evaluation, static inspection, cancellation, digest validation, and callback
   lifetime. Inspected adjacent course graph aggregation, zero padding, and
   omission limits; reproduced C3 with a generic graph fixture. Rechecked the
   existing destination offset/acceleration and score-property boundaries.

Reviews in this round were performed locally. The conversation's agent-thread
limit prevented another independent reviewer. This is bounded coverage of the
supported paths, not proof that all external skins behave identically.

## Verification

- Numeric parsing/text, course metadata, JSON sessions, and the additional
  course-graph case each reproduced an observable regression before its fix.
- JSON regressions cover real gameplay rendering, global utilities, timer
  factories reading initial state, updated frame state, independent result
  timers, scripted events, and automatic event intervals. Existing static JSON
  sessions still have no Lua VM; catalog parsing defers scripts.
- Java 17 float comparison: 11,538 boundary/random cases and a second 200,000
  random bit-pattern sample matched. This samples binary32; it is not exhaustive.
- `cmake --build cmake-build-debug --target all -j 6`: passed, including
  the final graph correction.
- Full parallel CTest run: 408/409 passed initially. The sole failure was the
  pinned oracle fixture checksum after correcting its JSON writer selector.
  Regenerated the trace using `generate_beatoraja_gameplay_skin_oracle.py`
  against the pinned checkout; only that checksum changed. `ctest --rerun-failed --output-on-failure -j 6` then passed
  (1/1), leaving all 409 tests verified.
- `git diff --check`: passed.

External skins, Lua file persistence, and LITONE-specific tests were unchanged.
No mobile deployment was performed.
