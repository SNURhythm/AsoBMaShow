# Replay-Only Scratch Handoff Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve validated logical-only scratch ownership handoffs through AsoBMaShow replay files and playback without changing Beatoraja stock keyinput or replaying physical lane edges.

**Architecture:** Add a `replayOnly` marker to canonical input transitions and validate the marker as an adjacent same-timestamp release-old/press-opposite scratch pair whose player scratch is already held. The existing AsoBMaShow extension carries the optional marker while stock projection strips it. Playback honors only structurally and statefully valid marked pairs; malformed markers fall back to normal physical processing and replay-file encode/decode rejects them.

**Tech Stack:** C++23, nlohmann JSON, CMake test executables.

## Global Constraints

- Do not change Beatoraja stock keyinput bytes for an otherwise identical transition stream.
- Missing extension marker fields decode as `false`.
- A marker must never suppress lane, command, unmatched scratch, or malformed scratch input.
- Do not commit, push, deploy, or modify GitHub state.

---

### Task 1: Canonical marker and capture propagation

**Files:**
- Modify: `src/replay/ReplayPlaybackData.h`
- Modify: `src/scene/play/RealtimeGameplayWorker.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/replay/ReplayInputRecorder.cpp`
- Modify: `src/replay/ReplayInputRecorder.h`
- Test: `tests/realtime_gameplay_worker_tests.cpp`
- Test: `tests/replay_input_recorder_tests.cpp`

**Interfaces:**
- Produces: `replay::InputTransition::replayOnly` and recorder APIs with a defaulted `bool replayOnly = false` argument.
- Consumes: `gameplay::RealtimeGameplayInput::replayOnly`.

- [ ] Add failing worker and recorder assertions showing the marker is absent or dropped.
- [ ] Run the two focused test targets and confirm RED.
- [ ] Propagate the marker through worker transfer, scene capture, and finish-time recorder materialization.
- [ ] Add strict whole-stream handoff validation shared by recorder and codec.
- [ ] Run the two focused test targets and confirm GREEN.

### Task 2: Extension encoding, compatibility, and strict validation

**Files:**
- Modify: `src/replay/BeatorajaReplayCodec.cpp`
- Test: `tests/beatoraja_replay_codec_tests.cpp`

**Interfaces:**
- Consumes: `replay::InputTransition::replayOnly` and shared handoff validation.
- Produces: optional `asobmashow.input[].replayOnly` JSON field.

- [ ] Add failing round-trip, absent-default, unchanged-stock, wrong-type, arbitrary-lane, unmatched, and malformed-pair tests.
- [ ] Run `beatoraja_replay_codec_tests` and confirm RED.
- [ ] Encode only true markers, decode missing markers as false, reject wrong types, and validate the complete marker grammar.
- [ ] Compare stock projections by timestamp/control/pressed semantics only.
- [ ] Run `beatoraja_replay_codec_tests` and confirm GREEN.

### Task 3: Playback without physical handoff edges

**Files:**
- Modify: `src/replay/ReplayPlaybackDriver.cpp`
- Modify: `src/replay/ReplayPlaybackDriver.h`
- Test: `tests/replay_playback_driver_tests.cpp`

**Interfaces:**
- Consumes: validated `replay::InputTransition::replayOnly` pairs.
- Produces: logical scratch-direction state changes without `IRhythmControl` release/press calls.

- [ ] Add a failing driver test proving a marked held-scratch handoff emits no physical reversal.
- [ ] Add fail-safe coverage proving malformed marked lane input still emits normal physical edges.
- [ ] Run `replay_playback_driver_tests` and confirm RED.
- [ ] Recognize only a valid next-edge pair against current held scratch state; update logical direction and retain physical hold.
- [ ] Run driver and materializer tests and confirm GREEN.

### Task 4: Verification

**Files:**
- Verify all modified files and the aggregate desktop target.

- [ ] Build and run worker, recorder, codec, driver, logical-input, and playback-startup suites.
- [ ] Run `cmake --build cmake-build-debug --target main -j 6`.
- [ ] Run `git diff --check` and inspect the final scoped diff.
