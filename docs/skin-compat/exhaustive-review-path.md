# Exhaustive skin and replay review path

This is the mandatory local review path for changes that affect installed
Beatoraja skins, skin settings, gameplay presentation, replay watch, or replay
video export. Its purpose is to catch the same classes of regression before a
PR reviewer has to identify them.

## Source of truth

Compatibility claims use only the pinned local Beatoraja checkout:

```sh
git -C /Users/xf/workspace/SNURhythm/beatoraja rev-parse HEAD
# Required: c2ed5db1a46145ed10790c3872f717e95b59db9d
```

Read the complete relevant upstream method before changing its counterpart.
Record its class, method, and SHA in the slice’s commit or review note. Do not
invent a validation, quota, denial, fallback, or state transition that does
not exist upstream. In particular, `SkinLuaAccessor.RestrictedIoLib.openFile`
directly uses the live selected skin directory; live reads/writes and
mid-session edits are intentional compatibility behavior.

Project-owned behavior (mobile storage plumbing, replay export, iOS renderer
ownership, and virtual controls) has no Beatoraja equivalent. It must instead
be compared across every AsoBMaShow path that presents the same state.

## Required review passes

| Pass | Trigger | Required comparison | Evidence to retain |
| --- | --- | --- | --- |
| Lua host and model | `src/skin/beatoraja/**`, package/catalog or Lua configuration changes | `SkinLuaAccessor`, `LuaSkinLoader`, `SkinLoader`, affected property/event factory, then the corresponding C++ host/parser/model code | Exact upstream class/method; focused `lua_skin_*`, `beatoraja_skin_model_tests`, `gameplay_skin_validator_tests` results |
| Gameplay projection | play state, note, BGA, timing, cover, input, or skin renderer changes | `BMSPlayer`, `LaneRenderer`, `SkinNote`, `PlaySkin`, and BGA classes against live `GamePlayScene`, `BMSRenderer`, `PlaySkinStateBridge`, and `PlaySkinSession` | Built-in plus selected-skin focused tests; no synthetic fallback or unsupported-state failure without upstream evidence |
| Replay propagation | replay watch or video export changes | For each changed authority field, compare live gameplay, replay watch, normal export, and every course stage | `replay_playfield_presentation_tests`, `playfield_presentation_coordinator_tests`, and a field-by-field propagation record |
| Package and Files integration | import, scan, storage, or file-system changes | Direct selected-root behavior in `SkinLuaAccessor` and current mobile Documents constraints | `skin_package_*`, `skin_tree_snapshotter_tests`, `lua_skin_file_system_tests`, and platform checks |
| Resource and platform lifecycle | bgfx, BGA, thread, texture, or iOS-sensitive changes | Create/submit/finalize/destroy ownership on successful and failing paths | focused BGA/resource tests, desktop full CTest, and unsigned iOS release verification |

## Compatibility matrix

For every touched Lua API, model field, renderer object, property type, timer,
event, or file operation, answer each cell before considering the slice ready:

| Question | Required result |
| --- | --- |
| Does upstream accept it? | Accept it here, or record the exact deliberate product divergence. Never reject it merely because it was not in a small test skin. |
| Does upstream reject it? | Reject it with the equivalent caller-visible Lua/model result, not an unrelated fallback. |
| Does it mutate data? | Preserve upstream live-directory semantics. Harden arithmetic, allocation sizing, and platform errors only when that does not change valid observable behavior. |
| Does it affect an on-frame value? | Verify the value reaches the active skin session before its frame is prepared. |
| Does it have a replay representation? | Verify capture, codec, replay watch, normal export, and course export; add a regression for every lost transition. |
| Does it affect rendering resources? | Verify the failure path releases prepared BGA/texture resources and that no selected-skin error silently draws built-in gameplay. |

## Replay authority propagation checklist

Whenever `PlayfieldAuthorityState` changes, review all four producers:

1. Live gameplay: `GamePlayScene`.
2. Replay watch: `ReplayPlayfieldPresentation` plus its coordinator.
3. Normal video export: `renderReplayVideoToMp4`.
4. Course video export: `renderCourseReplayVideoToMp4`, once per stage and
   with explicit cross-stage fields preserved.

At minimum compare: score, combo, maximum combo, combo breaks, judgement and
FAST/SLOW counters, gauge/gauge rules, BPM/scroll, lane-cover transitions,
best-score target, pacemaker target/status, timers/start clocks, replay
ghosts, BGA miss state, lane indicators, and full-combo/clear completion
timers. A field may be intentionally stage-local only when the source and
consumer agree; record that decision.

## Machine-runnable gate

Use the smallest focused target while iterating, then run this gate before a
skin/replay commit:

```sh
git diff --check
cmake --build cmake-build-debug --target main -j 12
# Renderer tests currently share a process-global bgfx device. Keep the full
# gate serialized until that unrelated fixture isolation issue is fixed.
ctest --test-dir cmake-build-debug --output-on-failure -j 1
scripts/ios_release_verify.sh
```

For a Lua-host change, the focused command is:

```sh
cmake --build cmake-build-debug --target lua_skin_host_modules_tests -j 12
./cmake-build-debug/lua_skin_host_modules_tests
```

For a presentation/replay change, run both:

```sh
cmake --build cmake-build-debug \
  --target replay_playfield_presentation_tests \
           playfield_presentation_coordinator_tests -j 12
./cmake-build-debug/replay_playfield_presentation_tests
./cmake-build-debug/playfield_presentation_coordinator_tests
```

No deployment/upload command belongs to this gate.

## Review-thread disposition rule

Classify every review suggestion before code changes:

- **Implement:** it reproduces in the local code and is compatible with the
  relevant upstream source or is an AsoBMaShow-owned correctness defect.
- **Reject as incompatible:** it adds a restriction that upstream does not
  impose, such as prohibiting live selected-root I/O, freezing edits at render
  start, or replacing live skin behavior with a synthetic fallback.
- **Defer with a concrete gap:** it is valid but requires a broader product
  decision or another repository; add it to `docs/todo.md` with source and
  affected paths instead of silently dropping it.

The current 2026-08-13 batch is classified as follows:

| Review theme | Disposition | Evidence |
| --- | --- | --- |
| Huge numeric or all-file Lua reads allocate from user input | Implemented by sizing the allocation from the unread tail, matching `SkinLuaAccessor.SandboxFile.read` behavior without a made-up cap | `lua_skin_host_modules_tests` large-read fixture |
| Lua `file:seek` signed overflow | Implemented by checked stream arithmetic; valid negative positions still clamp to zero | `LuaSkinFileIo.h` boundary tests |
| Validation read-only/profile overlay policy | Rejected as incompatible | `SkinLuaAccessor.RestrictedIoLib.openFile` directly creates/truncates the selected-root file and user requirement allows mid-session edits |

## Slice record

Each skin/replay commit must state:

1. pinned Beatoraja SHA and methods compared;
2. all producer paths checked for changed presentation state;
3. focused test(s), full desktop CTest, and iOS gate result; and
4. any review suggestion rejected as upstream-incompatible, with its evidence.

This turns reviewer feedback into a permanent local check rather than a
recurring manual patch loop.
