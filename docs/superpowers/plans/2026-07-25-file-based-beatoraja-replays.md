# File-Based Beatoraja Replays Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Store every eligible chart and course replay as a durable,
Beatoraja-compatible `.brd` file while keeping result history, provenance,
and postponed IR uploads independent from replay bytes.

**Architecture:** `ReplayPlaybackData` and `CourseReplayPlaybackData` are
encoded by `BeatorajaReplayCodec` and finalized beneath the active profile's
`replay/` directory by `ReplayFileStore`. SQLite schema v11 stores compact
result rows, provider-neutral IR snapshots, immutable replay-file references,
and idempotent filename reservations. A one-way all-files-first migration
converts schema-v10 row replays before one atomic database cutover; normal
result recall and IR code never reconstruct facts from replay data.

**Tech Stack:** C++23, SQLite3 WAL transactions, bundled miniz gzip,
nlohmann/json, SHA-256, SDL2, CMake/Ninja, CTest, Dear ImGui, filesystem-safe
atomic rename/sync utilities.

**Design:**
`docs/superpowers/specs/2026-07-25-file-based-beatoraja-replays-design.md`

## Global Constraints

- Pin interoperability behavior to Beatoraja commit
  `5f46fe198e88abbefe9215ca2de397aef8f54bd8`; do not infer mappings from a
  newer checkout without updating golden fixtures and the design baseline.
- Store replay files only beneath `<profile>/replay/`. Persist normalized
  relative paths beginning with `replay/`; reject absolute paths, `..`,
  symlinks, and aliases that escape the active profile.
- Match Beatoraja's chart/course stem, undefined-LN prefix, and numeric index
  suffix exactly. Allocate indexes monotonically beyond Beatoraja's visible
  slots 0..3 and never reuse a deleted index.
- `.brd` stock fields and `keyinput` must remain loadable by stock Beatoraja.
  Put touch samples, timed lane-cover changes, course rest timing, and the
  legacy playback track only in the versioned `asobmashow` extension.
- Replay playback models must not include final result facts,
  `ScoreProvenance`, IR eligibility/proof/payloads, database IDs, delivery
  state, or credentials. The migration-only legacy track may retain untrusted
  per-event annotations for faithful playback, but no result or IR API may
  consume them.
- Result and IR fingerprints must not include replay paths, hashes, bytes, or
  event data. Missing/deleted replay files must not affect result recall,
  pending score projection, IR outbox processing, receipts, or manual upload
  from a valid stored snapshot.
- Capture new replays from accepted logical input transitions before
  judgement derivation. Preserve scratch direction on keyboard/controller and
  touch/realtime paths. Never synthesize new stock input from judgement rows.
- Keep practice, Auto Play, replay playback, and existing eligibility policies
  unchanged. Imported stock Beatoraja files are replay-only and never become
  trusted local results or IR candidates.
- For new attempts, finalize and validate the file before committing the
  result/reference transaction. Repeating the same attempt ID must reuse its
  reservation and accept existing data only after exact fingerprint/hash and
  identity validation.
- Migration is all-files-first and one-way. No schema-v10 row changes occur
  before every deterministic `.brd` validates; one SQLite transaction then
  copies results/references, relinks durable work, drops legacy tables, and
  advances `user_version` to 11.
- Existing outbox payloads and receipts migrate unchanged. Do not manufacture
  an IR snapshot for a legacy record that lacks independently stored facts.
- Add every new production source to `src/CMakeLists.txt`, every focused test
  target to root `CMakeLists.txt`, and rely on the iOS synchronized source
  group for normal new files; do not add ordinary sources to
  `membershipExceptions`.
- Never upload a mobile build. Final verification uses desktop `main` and
  configured CTest; mobile build-only checks are optional only when the local
  signing environment is already configured.
- At each task, record the RED and GREEN commands/results in
  `.superpowers/sdd/file-based-replays-task-<N>-report.md`.

---

### Task 1: Define replay-only domain types and Beatoraja path identities

**Files:**

- Create: `src/replay/ReplayPlaybackData.h`
- Create: `src/replay/BeatorajaReplayPath.h`
- Create: `src/replay/BeatorajaReplayPath.cpp`
- Create: `tests/beatoraja_replay_path_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
namespace replay {
enum class LogicalControlKind : std::uint8_t {
  Lane,
  ScratchClockwise,
  ScratchCounterClockwise,
  Start,
  Select,
};

struct LogicalControl {
  LogicalControlKind kind = LogicalControlKind::Lane;
  int player = 1;
  int lane = -1;
  bool operator==(const LogicalControl &) const = default;
};

struct InputTransition {
  std::int64_t songTimeMicros = 0;
  LogicalControl control;
  bool pressed = false;
  bool operator==(const InputTransition &) const = default;
};

struct ChartPlaybackSetup {
  std::string chartMd5;
  std::string chartSha256;
  int keyMode = 0;
  int longNoteMode = 0;
  bool hasUndefinedLongNotes = false;
  std::optional<unsigned int> randomSeed;
  std::optional<std::string> randomPrng;
  std::vector<int> randomValues;
  std::optional<std::string> playOption;
  std::optional<std::int64_t> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<std::int64_t> playOption2Seed;
  std::string assistOption;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  std::string playbackRulesetId;
  int playbackRulesetRevision = 0;
  int playbackRatePercent = 100;
  int judgeWindowScalePercent = 100;
  float startingGaugePercent = 20.0F;
  bool clubMode = false;
  int initialLaneCoverPercent = 0;
  bool laneCoverEnabled = false;
  bool operator==(const ChartPlaybackSetup &) const = default;
};

enum class LegacyPlaybackAction : std::uint8_t {
  Press,
  Release,
  Miss,
  Mine,
  Gauge,
  MultiBad,
};

struct LegacyPlaybackEvent {
  LegacyPlaybackAction action = LegacyPlaybackAction::Press;
  int lane = -1;
  std::int64_t noteTimeMicros = -1;
  std::int64_t songTimeMicros = 0;
  std::int64_t judgeTimeMicros = 0;
  Judgement judgement = None;
  std::int64_t diffMicros = 0;
  float gauge = 0.0F;
  GaugeType gaugeType = GaugeType::Normal;
  int combo = 0;
  int score = 0;
  bool operator==(const LegacyPlaybackEvent &) const = default;
};

struct LegacyPlaybackTrack {
  std::vector<LegacyPlaybackEvent> events;
  bool stockScratchDirectionBestEffort = false;
  bool operator==(const LegacyPlaybackTrack &) const = default;
};

struct ReplayPlaybackData {
  ChartPlaybackSetup setup;
  std::vector<InputTransition> input;
  std::vector<ReplayTouchSample> touchSamples;
  std::vector<ReplayLaneCoverEvent> laneCoverEvents;
  std::optional<LegacyPlaybackTrack> legacy;
  bool operator==(const ReplayPlaybackData &) const = default;
};

struct CourseReplayPlaybackData {
  std::vector<ReplayPlaybackData> stages;
  std::vector<std::int64_t> restMicrosAfterStage;
  bool operator==(const CourseReplayPlaybackData &) const = default;
};

struct CoursePathInput {
  std::vector<std::string> stageSha256;
  int longNoteMode = 0;
  bool hasUndefinedLongNotes = false;
  std::vector<int> beatorajaConstraintIds;
};

struct ReplayPathIdentity {
  std::string stem;
  std::int64_t historyIndex = 0;
  std::filesystem::path relativePath;
  bool operator==(const ReplayPathIdentity &) const = default;
};

std::optional<std::string> chartStem(std::string_view lowerSha256,
                                     int longNoteMode,
                                     bool hasUndefinedLongNotes,
                                     std::string &diagnostic);
std::optional<std::string> courseStem(const CoursePathInput &,
                                      std::string &diagnostic);
std::optional<ReplayPathIdentity> pathForStem(std::string_view stem,
                                              std::int64_t historyIndex,
                                              std::string &diagnostic);
} // namespace replay
```

`ReplayPlaybackData.h` owns the explicitly untrusted `LegacyPlaybackEvent`
solely so migration can preserve old playback annotations. It must not include
`ScoreProvenance.h`, `IrSubmission.h`, or repository headers. Move
`ReplayTouchSample` and `ReplayLaneCoverEvent` into this header now and provide
temporary aliases for those types and `LegacyPlaybackEvent` from
`ReplayData.h` until Task 13 removes the old model.

- [ ] **Step 1: Establish the path/model baseline**

Run:

```sh
cmake --build cmake-build-debug --target \
  replay_repository_tests logical_gameplay_input_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(replay_repository_tests|foundation_input_gameplay)$'
```

Expected: both focused suites pass and `main` builds.

- [ ] **Step 2: Add path tests and target before implementation**

Cover exact fixtures:

```cpp
constexpr std::string_view sha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
expectPath(chart(sha, 0, false), 0,
           "replay/0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef.brd");
expectPath(chart(sha, 1, true), 1,
           "replay/C0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef_1.brd");
expectPath(chart(sha, 2, true), 27,
           "replay/H0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef_27.brd");
expectPath(course({shaA, shaB}, 1, true, {1, 4, 7, 13, 2, 3}), 0,
           "replay/C" + shaA.substr(0, 10) + shaB.substr(0, 10) +
               "_040713.brd");
```

Also reject uppercase/non-64-character/non-hex chart hashes, negative or
overflow indexes, empty courses, short stage hashes, more than 256 stages,
unknown constraint IDs, private Aso constraint IDs, and stems containing path
separators. Verify slot 0 has no suffix and index 4 uses `_4` without special
handling.

- [ ] **Step 3: Run the target and capture RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target beatoraja_replay_path_tests -j 6
```

Expected: compile fails because `replay/BeatorajaReplayPath.h` is absent.

- [ ] **Step 4: Implement domain and path mapping**

Use a table for Beatoraja's undefined-LN prefixes `{0: "", 1: "C", 2:
"H"}` and reproduce its course constraint filter/order. Produce only
`replay/<stem>[_<positive-index>].brd`; use `std::from_chars`/checked integer
operations and do not concatenate caller-supplied path fragments.

- [ ] **Step 5: Run GREEN and commit**

Run:

```sh
cmake --build cmake-build-debug --target \
  beatoraja_replay_path_tests replay_repository_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(beatoraja_replay_path_tests|replay_repository_tests)$'
git diff --check
```

Expected: all commands pass.

Commit:

```sh
git add src/replay/ReplayPlaybackData.h \
  src/replay/BeatorajaReplayPath.h src/replay/BeatorajaReplayPath.cpp \
  src/ReplayData.h src/CMakeLists.txt CMakeLists.txt \
  tests/beatoraja_replay_path_tests.cpp
git commit -m "feat: define beatoraja replay paths"
```

---

### Task 2: Implement bounded gzip, Base64URL, and the `.brd` codec

**Files:**

- Create: `src/replay/GzipCodec.h`
- Create: `src/replay/GzipCodec.cpp`
- Create: `src/replay/Base64Url.h`
- Create: `src/replay/Base64Url.cpp`
- Create: `src/replay/BeatorajaReplayCodec.h`
- Create: `src/replay/BeatorajaReplayCodec.cpp`
- Create: `tests/beatoraja_replay_codec_tests.cpp`
- Create: `tests/fixtures/replay/beatoraja-chart.brd`
- Create: `tests/fixtures/replay/beatoraja-course.brd`
- Create: `tests/fixtures/replay/beatoraja-keyinput.bin`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
namespace replay {
struct ReplayCodecLimits {
  std::size_t maxCompressedBytes = 64U * 1024U * 1024U;
  std::size_t maxJsonBytes = 256U * 1024U * 1024U;
  std::size_t maxKeyInputBytes = 9U * 1'000'000U;
  std::size_t maxInputTransitions = 1'000'000U;
  std::size_t maxTouchSamples = 1'000'000U;
  std::size_t maxLaneCoverEvents = 100'000U;
  std::size_t maxJsonDepth = 64U;
};

struct ReplayDecodeOutcome {
  std::optional<ReplayPlaybackData> chart;
  std::optional<CourseReplayPlaybackData> course;
  bool stockOnly = false;
  bool unsupportedAsoExtension = false;
  std::string diagnostic;
};

class BeatorajaReplayCodec {
public:
  explicit BeatorajaReplayCodec(ReplayCodecLimits limits = {});
  std::optional<std::vector<std::byte>>
  encodeChart(const ReplayPlaybackData &, std::int64_t playedAtUnixMillis,
              std::string &diagnostic) const;
  std::optional<std::vector<std::byte>>
  encodeCourse(const CourseReplayPlaybackData &,
               std::int64_t playedAtUnixMillis,
               std::string &diagnostic) const;
  ReplayDecodeOutcome decode(std::span<const std::byte>) const;

  static std::optional<int> beatorajaKeyCode(const LogicalControl &,
                                             int keyMode) noexcept;
  static std::optional<LogicalControl> logicalControl(int keyCode,
                                                      int keyMode) noexcept;
};

std::optional<std::vector<std::byte>> gzipCompress(
    std::span<const std::byte>, std::string &diagnostic);
std::optional<std::vector<std::byte>> gzipDecompressBounded(
    std::span<const std::byte>, std::size_t maximumOutputBytes,
    std::string &diagnostic);
std::string base64UrlEncode(std::span<const std::byte>);
std::optional<std::vector<std::byte>> base64UrlDecodeBounded(
    std::string_view, std::size_t maximumOutputBytes,
    std::string &diagnostic);
} // namespace replay
```

The outer stream is gzip JSON. `keyinput` is URL-safe Base64 of an inner gzip
stream. Each decoded transition is exactly one signed key byte followed by a
little-endian signed 64-bit timestamp. Press is `keyCode + 1`; release is its
negative. Reject zero, `INT8_MIN`, trailing partial records, invalid mode/key
pairs, decreasing timestamps, and invalid state transitions. Decoding remains
tolerant of redundant stock press/release records; the Aso recorder and
encoder emit only effective transitions.

- [ ] **Step 1: Generate and commit independent golden fixtures**

Use the pinned Beatoraja Java implementation in a temporary checkout or a
small Java fixture generator that imports the pinned `ReplayData` behavior.
Store one chart fixture containing lane, both scratch directions, Start and
Select transitions, initial lane cover, random options, and one two-stage
course fixture. Store the already inner-decompressed `keyinput` bytes as
`beatoraja-keyinput.bin` so byte order/sign can be asserted independently.
Record the pinned source paths and SHA-256 of each fixture in the test file.

- [ ] **Step 2: Write codec tests before production files**

Tests must:

- decode both Beatoraja fixtures and compare every supported stock field;
- compare encoded key records byte-for-byte with
  `beatoraja-keyinput.bin`;
- round-trip 5-key, 7-key, 9-key, 10-key, and 14-key controls including both
  scratch directions;
- prove the initial cover maps to `config` while timed cover/touch/rest data
  round-trips only through `asobmashow.schemaVersion == 1`;
- prove a stock-compatible reference JSON reader ignores the extension;
- reject malformed outer/inner gzip, invalid Base64URL, partial nine-byte
  records, decompression expansion beyond each limit, excessive JSON depth,
  overlong strings/arrays, non-finite touch coordinates, out-of-range cover,
  and chart/course envelope mismatch; an unknown extension version must be
  ignored and flagged while retaining safe stock playback;
- accept unknown stock JSON fields.

- [ ] **Step 3: Run RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target beatoraja_replay_codec_tests -j 6
```

Expected: compile fails on the missing codec headers.

- [ ] **Step 4: Implement the bounded primitives**

Wrap bundled miniz by passing `15 + 16` as `windowBits` to
`mz_deflateInit2` and `mz_inflateInit2` for gzip framing. Grow output in checked
chunks and abort before the configured maximum. Implement Base64URL with
`-`/`_`, accept Beatoraja's padded and unpadded forms, and reject whitespace,
non-canonical tail bits, or output beyond the bound.

- [ ] **Step 5: Implement stock mappings and the Aso extension**

Use explicit tables for key modes, gauge/options, lane order, scratch
direction, and Beatoraja JSON field names. Serialize JSON deterministically
for idempotent retries. Validate all fields before allocation. When decoding
an unsupported Aso extension, retain safe stock playback in the outcome and
set `unsupportedAsoExtension`; do not interpret extension fields.

- [ ] **Step 6: Run GREEN and commit**

Run:

```sh
cmake --build cmake-build-debug --target \
  beatoraja_replay_codec_tests beatoraja_replay_path_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(beatoraja_replay_codec_tests|beatoraja_replay_path_tests)$'
git diff --check
```

Expected: all commands pass and sanitizer/limit fixtures terminate without
unbounded allocation.

Commit:

```sh
git add src/replay/GzipCodec.h src/replay/GzipCodec.cpp \
  src/replay/Base64Url.h src/replay/Base64Url.cpp \
  src/replay/BeatorajaReplayCodec.h src/replay/BeatorajaReplayCodec.cpp \
  tests/fixtures/replay \
  tests/beatoraja_replay_codec_tests.cpp src/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add beatoraja replay codec"
```

---

### Task 3: Add the contained, crash-safe replay file store

**Files:**

- Create: `src/replay/ReplayFileStore.h`
- Create: `src/replay/ReplayFileStore.cpp`
- Create: `tests/replay_file_store_tests.cpp`
- Create: `tests/atomic_file_tests.cpp`
- Modify: `src/AtomicFile.h`
- Modify: `src/AtomicFile.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
namespace replay {
enum class ReplayFileState { Available, Missing, Corrupt, Unsafe, IoFailure };

struct ReplayFileMetadata {
  std::filesystem::path relativePath;
  std::string sha256;
  std::uint64_t compressedSize = 0;
  int codecVersion = 1;
  bool operator==(const ReplayFileMetadata &) const = default;
};

struct FinalizeOutcome {
  std::optional<ReplayFileMetadata> metadata;
  bool existingIdenticalFile = false;
  std::string diagnostic;
};

struct ReplayFileInspection {
  ReplayFileState state = ReplayFileState::IoFailure;
  std::optional<ReplayFileMetadata> metadata;
  std::string diagnostic;
};

struct ExpectedReplayIdentity {
  std::vector<std::string> stageSha256;
  bool course = false;
};

struct ReplayFileStoreFaults {
  std::function<bool(std::string_view)> failAt;
};

class ReplayFileStore {
public:
  ReplayFileStore(std::filesystem::path profileRoot,
                  ReplayFileStoreFaults faults = {});
  FinalizeOutcome finalize(const ReplayPathIdentity &,
                           std::span<const std::byte> encoded,
                           const BeatorajaReplayCodec &,
                           const ExpectedReplayIdentity &,
                           std::string_view attemptToken);
  ReplayFileInspection inspect(const ReplayFileMetadata &) const;
  ReplayDecodeOutcome load(const ReplayFileMetadata &,
                           const BeatorajaReplayCodec &) const;
  bool remove(const ReplayFileMetadata &, std::string &diagnostic);
  bool copyToBeatorajaSlot(const ReplayFileMetadata &source,
                           std::string_view stem, int slot,
                           std::string &diagnostic);
  void removeStaleTemporaryFiles(std::chrono::system_clock::time_point cutoff);
};
} // namespace replay
```

`finalize` writes a private sibling `.<filename>.<attempt-token>.tmp` with
`atomic_file::writeWithoutBackup` and private operations, then calls a new
`atomic_file::renameNoReplaceDurably`, syncs the directory, reads the final
file, decodes it, checks the expected identity, and hashes it with
`FileChecksum`. Implement no-replace with `renameat2(RENAME_NOREPLACE)` where
available, `link` plus `unlink` on other POSIX systems, and
`MoveFileExW` without `MOVEFILE_REPLACE_EXISTING` on Windows. Existing final
files are accepted only if the bytes/hash and decoded identity match.
`remove` deletes only the validated regular non-link final path and retains
all metadata outside this class.

- [ ] **Step 1: Add file-store tests and inject every durability fault**

Cover `renameNoReplaceDurably` success, destination collision, missing source,
and cross-device failure in `atomic_file_tests`. In the store tests, cover
write, file sync, close, rename, directory sync, read-back, decode, and
hash failures. Assert that pre-rename failures expose no final path; a
post-rename failure leaves a reusable final file; retry returns the same
metadata; differing existing bytes fail closed. Add traversal, absolute path,
symlink file, symlink parent, hard-link replacement, hash mismatch, missing
file, slot range outside 0..3, and copy-without-content-conversion cases.

- [ ] **Step 2: Run RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target replay_file_store_tests -j 6
```

Expected: compile fails because `ReplayFileStore.h` does not exist.

- [ ] **Step 3: Implement safe resolution and finalization**

Resolve from the canonical profile root plus the normalized relative path,
walk every parent with `symlink_status`, and require the destination's parent
to be the real profile `replay/` directory. Use collision-resistant temp names
from the attempt/reservation token, private permissions, and the existing
atomic file primitives. Do not remove an unknown final file during retry.

- [ ] **Step 4: Run GREEN and commit**

Run:

```sh
cmake --build cmake-build-debug --target \
  atomic_file_tests replay_file_store_tests \
  beatoraja_replay_codec_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(atomic_file_tests|replay_file_store_tests|beatoraja_replay_codec_tests)$'
git diff --check
```

Expected: all commands pass.

Commit:

```sh
git add src/replay/ReplayFileStore.h src/replay/ReplayFileStore.cpp \
  src/AtomicFile.h src/AtomicFile.cpp tests/replay_file_store_tests.cpp \
  tests/atomic_file_tests.cpp \
  src/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add durable replay file store"
```

---

### Task 4: Split persisted results and provider-neutral IR snapshots from replay

**Files:**

- Modify: `src/ResultPersistenceModel.h`
- Modify: `src/ResultPersistenceModel.cpp`
- Create: `src/CompletedAttempt.h`
- Create: `src/LegacyChartResultAttempt.h`
- Create: `src/LegacyChartResultAttempt.cpp`
- Modify: `src/ResultPersistenceCoordinator.h`
- Modify: `src/ResultPersistenceCoordinator.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryInternal.h`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ResultScene.h`
- Delete: `src/LegacyChartResultAttempt.h`
- Delete: `src/LegacyChartResultAttempt.cpp`
- Create: `src/ir/IrSubmissionSnapshot.h`
- Create: `src/ir/IrSubmissionSnapshot.cpp`
- Modify: `src/ir/IrSubmission.h`
- Modify: `src/ir/IrSubmission.cpp`
- Modify: `src/ir/IrOutboxModels.h`
- Modify: `tests/result_persistence_model_tests.cpp`
- Modify: `tests/ir_submission_service_tests.cpp`
- Create: `tests/ir_submission_snapshot_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
namespace result_persistence {
struct PersistedChartResult {
  int resultId = 0;
  std::optional<std::string> attemptId;
  ChartScoreWrite score;
  int keyMode = 0;
  std::vector<float> adoptedGaugeHistory;
  std::optional<ChartJudgementTiming> judgementTiming;
  std::int64_t playedAtUnixMillis = 0;
  std::string resultFingerprint;
  bool operator==(const PersistedChartResult &) const = default;
};

struct PersistedCourseStageResult {
  int stageIndex = 0;
  ChartScoreWrite score;
  int keyMode = 0;
  std::vector<float> adoptedGaugeHistory;
  std::optional<ChartJudgementTiming> judgementTiming;
  bool operator==(const PersistedCourseStageResult &) const = default;
};

struct PersistedCourseResult {
  int resultId = 0;
  std::optional<std::string> attemptId;
  std::string courseKey;
  int legacyCourseId = 0;
  std::string courseName;
  std::string courseGroupName;
  std::string constraintJson;
  int completedCharts = 0;
  int totalCharts = 0;
  std::string requestedPlayOption;
  std::string assistOption;
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  int longNoteMode = 0;
  int finalScore = 0;
  int maxScore = 0;
  int maxCombo = 0;
  float finalGauge = 0.0F;
  int clearType = kClearTypeFailedRank;
  ScoreProvenance provenance = ScoreProvenance::Legacy();
  std::vector<PersistedCourseStageResult> stages;
  std::int64_t playedAtUnixMillis = 0;
  std::string resultFingerprint;
};

struct StageReceipt {
  std::string attemptId;
  int resultId = 0;
  std::string createdAt;
  bool scorePending = false;
};

std::optional<PersistedChartResult> capturePersistedChartResult(
    std::string attemptId, const bms_parser::ChartMeta &, const RhythmState &,
    const ScoreProvenance &, int storageLongNoteMode,
    std::int64_t playedAtUnixMillis, std::string &diagnostic);
std::string resultFingerprint(const PersistedChartResult &);
bool validatePersistedCourseResult(const PersistedCourseResult &,
                                   std::string &diagnostic);
std::string resultFingerprint(const PersistedCourseResult &);
} // namespace result_persistence

namespace ir {
struct IrSubmissionSnapshot {
  static constexpr int kSchemaVersion = 1;
  int schemaVersion = kSchemaVersion;
  IrSubmission submission;
  std::string fingerprint;
  bool operator==(const IrSubmissionSnapshot &) const = default;
};

IrSubmissionBuildOutcome makeIrSubmission(
    const result_persistence::PersistedChartResult &) noexcept;
std::optional<IrSubmissionSnapshot> captureIrSubmissionSnapshot(
    const result_persistence::PersistedChartResult &,
    std::string &diagnostic) noexcept;
std::optional<std::string> serializeIrSubmissionSnapshot(
    const IrSubmissionSnapshot &, std::string &diagnostic) noexcept;
std::optional<IrSubmissionSnapshot> deserializeIrSubmissionSnapshot(
    std::string_view, std::string_view expectedFingerprint,
    std::string &diagnostic) noexcept;
} // namespace ir

// src/CompletedAttempt.h; the only model that intentionally joins domains,
// and it is memory-only.
namespace result_persistence {
struct CompletedChartAttempt {
  PersistedChartResult result;
  replay::ReplayPlaybackData replay;
  ir::IrSubmissionSnapshot irSnapshot;
};

struct CompletedCourseAttempt {
  PersistedCourseResult result;
  replay::CourseReplayPlaybackData replay;
};
} // namespace result_persistence
```

Keep both completed-attempt commands in `src/CompletedAttempt.h`; this is the
only memory-only orchestration header allowed to include all three domains.
The persisted result and IR snapshot
headers must compile in a translation unit that does not include
`ReplayData.h` or `ReplayPlaybackData.h`. Provider outbox drafts continue to
contain their provider-specific serialized payload, but they are constructed
from `IrSubmissionSnapshot::submission`, not a replay-bearing attempt.
The optional result attempt ID exists only so migrated rows that never had a
canonical ID remain viewable. Every new `CompletedChartAttempt` and
`CompletedCourseAttempt` requires a canonical lower-case v4 ID; a missing ID
cannot reserve a path, create a snapshot, project a score, or enter IR.

To keep this task buildable before Task 7 changes repository staging, move the
old replay-bearing `ChartResultAttempt` and its fingerprint factory verbatim
to `legacy_result_persistence::LegacyChartResultAttempt` in the two temporary
legacy files. The existing coordinator/repository signatures use that
temporary namespace only through Task 6. Gameplay captures the new independent
result/snapshot in parallel for automatic IR, while the old command remains a
staging compatibility shim. Task 7 deletes the shim when it switches the
coordinator; no new fingerprint or IR logic may be added to it.

- [ ] **Step 1: Rewrite model tests to assert domain independence**

Add tests that capture a result and snapshot once, mutate every replay field
(input, touch, lane cover, legacy annotations, path, bytes), and assert that
both result and snapshot fingerprints remain unchanged. Mutate every result
or IR field individually and assert the appropriate fingerprint changes.
Test finite gauge samples, canonical chart identity, judgement timing totals,
canonical v4 attempt ID, non-negative played time, schema version, exact JSON
round trip, bad fingerprint, unknown version, missing/extra required fields,
and canonical float bit handling.
Apply the same field-by-field fingerprint/validation checks to compact course
results, including ordered stages, partial completion, constraints, gauge
configuration, and course provenance.

Add a source audit test/command:

```sh
! rg -n '#include .*Replay(Data|PlaybackData)' \
  src/ResultPersistenceModel.h src/ir/IrSubmission.h \
  src/ir/IrSubmissionSnapshot.h
```

- [ ] **Step 2: Run RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target \
  result_persistence_model_tests ir_submission_snapshot_tests -j 6
```

Expected: the result tests fail to compile against `PersistedChartResult` and
the snapshot header is missing.

- [ ] **Step 3: Implement canonical independent models**

Move chart/result facts currently validated against `ReplayData` into
`PersistedChartResult`. Compute `resultFingerprint` from a version tag and a
length-framed canonical encoding of result/provenance fields only. Serialize
the snapshot deterministically with all `IrSubmission` fields, hash that
serialization, and validate it on every load. Do not use replay data as a
secondary validation source.

- [ ] **Step 4: Adapt automatic IR draft construction**

Change each driver/draft builder call site to accept
`const IrSubmissionSnapshot &` or its `submission` value. Keep provider draft
and outbox semantics byte-for-byte for the same submission facts. Update
fixtures rather than weakening provider payload assertions.

- [ ] **Step 5: Run GREEN and commit**

Run:

```sh
cmake --build cmake-build-debug --target \
  result_persistence_model_tests ir_submission_snapshot_tests \
  ir_submission_service_tests tachi_driver_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(result_persistence_model_tests|ir_submission_snapshot_tests|ir_submission_service_tests|tachi_driver_tests)$'
! rg -n '#include .*Replay(Data|PlaybackData)' \
  src/ResultPersistenceModel.h src/ir/IrSubmission.h \
  src/ir/IrSubmissionSnapshot.h
git diff --check
```

Expected: all tests and the dependency audit pass.

Commit:

```sh
git add src/ResultPersistenceModel.h src/ResultPersistenceModel.cpp \
  src/CompletedAttempt.h src/LegacyChartResultAttempt.h \
  src/LegacyChartResultAttempt.cpp src/ResultPersistenceCoordinator.h \
  src/ResultPersistenceCoordinator.cpp src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryInternal.h \
  src/repositories/ReplayRepositoryRecords.cpp \
  src/scene/play/GamePlayScene.cpp src/scene/ResultScene.h \
  src/ir/IrSubmissionSnapshot.h src/ir/IrSubmissionSnapshot.cpp \
  src/ir/IrSubmission.h src/ir/IrSubmission.cpp src/ir/IrOutboxModels.h \
  tests/result_persistence_model_tests.cpp \
  tests/ir_submission_snapshot_tests.cpp tests/ir_submission_service_tests.cpp \
  src/CMakeLists.txt src/ir/CMakeLists.txt CMakeLists.txt
git commit -m "refactor: decouple results and ir snapshots from replay"
```

---

### Task 5: Capture accepted raw logical input, including scratch direction

**Files:**

- Create: `src/replay/ReplayInputRecorder.h`
- Create: `src/replay/ReplayInputRecorder.cpp`
- Modify: `src/input/InputTypes.h`
- Modify: `src/input/InputBindingResolver.cpp`
- Modify: `src/input/LogicalGameplayInputAdapter.h`
- Modify: `src/input/LogicalGameplayInputAdapter.cpp`
- Modify: `src/input/RhythmInputHandler.h`
- Modify: `src/input/RhythmInputHandler.cpp`
- Modify: `src/scene/play/RealtimeGameplayWorker.h`
- Modify: `src/scene/play/RealtimeGameplayWorker.cpp`
- Modify: `src/scene/play/RealtimeTouchInputRouter.h`
- Modify: `src/scene/play/RealtimeTouchInputRouter.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `tests/logical_gameplay_input_tests.cpp`
- Modify: `tests/realtime_touch_input_router_tests.cpp`
- Modify: `tests/realtime_gameplay_worker_tests.cpp`
- Create: `tests/replay_input_recorder_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
namespace replay {
struct ReplayInputRecorderLimits {
  std::int64_t minimumSongTimeMicros = -30'000'000;
  std::size_t maximumTransitions = 1'000'000;
};

struct ReplayClock {
  void *context = nullptr;
  std::optional<std::int64_t> (*mapSteadyToSong)(void *, std::int64_t) = nullptr;
};

class ReplayInputRecorder {
public:
  explicit ReplayInputRecorder(ReplayClock,
                               ReplayInputRecorderLimits = {});
  bool record(std::int64_t steadyTimestampMicros,
              LogicalControl control, bool pressed,
              std::string &diagnostic) noexcept;
  bool recordSongTime(std::int64_t songTimeMicros,
                      LogicalControl control, bool pressed,
                      std::string &diagnostic) noexcept;
  std::vector<InputTransition> finish(std::string &diagnostic) noexcept;
};
} // namespace replay

namespace input {
struct LogicalInputTransition {
  InputScope scope;
  LogicalAction action;
  bool pressed = false;
  float value = 0.0F;
  std::int64_t steadyTimestampMicros = 0;
};
} // namespace input

class LogicalGameplayInputAdapter {
public:
  struct AppliedTransition {
    input::LogicalInputTransition source;
    replay::LogicalControl control;
    bool pressed = false;
  };
  using AppliedTransitionCallback =
      std::function<void(const AppliedTransition &)>;

  LogicalGameplayInputAdapter(IRhythmControl &, CommandCallback,
                              AppliedTransitionCallback = {});
};

namespace gameplay {
enum class RealtimeLogicalControlKind : std::uint8_t {
  Lane,
  ScratchClockwise,
  ScratchCounterClockwise,
};

struct RealtimeGameplayInput {
  std::uint64_t epoch = 0;
  RealtimeGameplayInputType type = RealtimeGameplayInputType::Press;
  int lane = -1;
  int compensateLane = -1;
  bool backSpin = false;
  std::int64_t steadyTimestampMicros = 0;
  std::int64_t inputDelayMicros = 0;
  RealtimeLogicalControlKind replayControl =
      RealtimeLogicalControlKind::Lane;
};

class RealtimeGameplayWorker {
public:
  std::vector<replay::InputTransition>
  copyAcceptedReplayInputAfterStop() const;
};
} // namespace gameplay
```

Add normalized `steadyTimestampMicros` to `LogicalInputTransition`; the
resolver copies it from `PhysicalInputEvent`, and direct keyboard calls use
the injected steady-clock provider already owned by the input handler. The
adapter callback fires after an effective lane/scratch state transition, not
for duplicate presses, unmatched releases, shadowed scratch directions,
Pause/Retry/cover commands, or rejected input.

The realtime worker maps timestamps with its existing `RealtimeGameplayClock`
and appends only inputs it actually processes before terminal state. The
touch router sets clockwise/counterclockwise from its existing
`scratchDirection`; releases retain the matching direction. The finished
worker stream is merged in sequence with main-thread touch/control samples
without timestamp reordering.

- [ ] **Step 1: Add behavioral tests before implementation**

For adapter input, assert lane press/release, clockwise and counterclockwise
scratch, direction reversal, opposite-direction release, duplicate physical
bindings, device disconnect releases, and two-player lane mappings emit the
minimum effective logical transition stream. Assert commands never emit.

For touch/realtime input, assert vertical scratch movement preserves both
directions and matching releases, normal lanes remain `Lane`, preparation
inputs use mapped song time, terminal/epoch/clock failures do not record, and
more than 256 valid inputs survive `copyAcceptedReplayInputAfterStop()`.

For the recorder, reject absent clocks, decreasing mapped time, negative
times outside allowed pre-roll, invalid controls, duplicate state, and calls
after `finish`; return sorted immutable data on success.

- [ ] **Step 2: Run RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target \
  replay_input_recorder_tests logical_gameplay_input_tests \
  realtime_touch_input_router_tests realtime_gameplay_worker_tests -j 6
```

Expected: compile fails on the new recorder/callback/control fields.

- [ ] **Step 3: Implement accepted-transition observation**

Keep judgement calls unchanged. Introduce one state-aware callback point in
`LogicalGameplayInputAdapter` after each effective call to `IRhythmControl`,
and one accepted-input vector owned only by the realtime worker thread. Do not
record `GameplayReplayEvent`, judgement, gauge, combo, score, or mine/miss
events into the new input stream.

- [ ] **Step 4: Replace gameplay's new-recording source**

`GamePlayScene::beginReplayRecording` initializes `ReplayPlaybackData.setup`
and a `ReplayInputRecorder`. `finishReplayRecording` collects raw input,
touch samples, and lane-cover changes without copying final score or
provenance into that payload. Until Task 7 switches the persistence
coordinator, continue producing the old judged in-memory data in parallel for
the existing save call and practice/analytics; never derive the new raw stream
from it. After Task 7, the old type remains only for legacy playback,
practice/analytics, and schema-v10 migration until Task 13.

- [ ] **Step 5: Run GREEN and commit**

Run:

```sh
cmake --build cmake-build-debug --target \
  replay_input_recorder_tests logical_gameplay_input_tests \
  realtime_touch_input_router_tests realtime_gameplay_worker_tests \
  gameplay_simulation_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(replay_input_recorder_tests|foundation_input_gameplay|realtime_touch_input_router_tests|realtime_gameplay_worker_tests|gameplay_simulation_tests)$'
git diff --check
```

Expected: all commands pass; new recordings contain raw input and no result
facts.

Commit:

```sh
git add src/replay/ReplayInputRecorder.h src/replay/ReplayInputRecorder.cpp \
  src/input/InputTypes.h src/input/InputBindingResolver.cpp \
  src/input/LogicalGameplayInputAdapter.h \
  src/input/LogicalGameplayInputAdapter.cpp src/input/RhythmInputHandler.h \
  src/input/RhythmInputHandler.cpp src/scene/play/RealtimeGameplayWorker.h \
  src/scene/play/RealtimeGameplayWorker.cpp \
  src/scene/play/RealtimeTouchInputRouter.h \
  src/scene/play/RealtimeTouchInputRouter.cpp \
  src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp \
  tests/replay_input_recorder_tests.cpp \
  tests/logical_gameplay_input_tests.cpp \
  tests/realtime_touch_input_router_tests.cpp \
  tests/realtime_gameplay_worker_tests.cpp src/CMakeLists.txt CMakeLists.txt
git commit -m "feat: capture raw replay input"
```

---

### Task 6: Define schema v11 compact results, snapshots, references, and reservations

**Files:**

- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryInternal.h`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Create: `src/repositories/ReplayRepositoryResultRecords.cpp`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `tests/RepositorySqliteTestSupport.h`
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Public models/API:**

```cpp
struct ReplayFileReference {
  enum class RecordKind { ChartResult, CourseResult };
  std::int64_t id = 0;
  RecordKind recordKind = RecordKind::ChartResult;
  int recordId = 0;
  std::string stem;
  std::int64_t historyIndex = 0;
  std::filesystem::path relativePath;
  std::string contentSha256;
  std::uint64_t compressedSize = 0;
  int codecVersion = 1;
};

struct ReplayFileReservation {
  std::string attemptId;
  std::string stem;
  std::int64_t historyIndex = 0;
  std::filesystem::path relativePath;
};

struct ReservationOutcome {
  enum class Status { Reserved, AlreadyReserved, Invalid, StorageFailure,
                      IntegrityConflict };
  Status status = Status::StorageFailure;
  std::optional<ReplayFileReservation> reservation;
  std::string diagnostic;
};

struct ResultRecord {
  result_persistence::PersistedChartResult result;
  std::optional<ReplayFileReference> replayFile;
};

struct ResultReadOutcome {
  enum class Status { Loaded, NotFound, Invalid, StorageFailure,
                      IntegrityConflict };
  Status status = Status::StorageFailure;
  std::optional<ResultRecord> record;
  std::string diagnostic;
};

namespace ir {
struct IrSubmissionSnapshotReadOutcome {
  enum class Status { Loaded, NotFound, Invalid, StorageFailure,
                      IntegrityConflict };
  Status status = Status::StorageFailure;
  std::optional<IrSubmissionSnapshot> snapshot;
  std::string diagnostic;
};
} // namespace ir

class ReplayRepository {
public:
  static constexpr int kCurrentSchemaVersion = 11;
  ReservationOutcome reserveReplayFile(std::string_view attemptId,
                                       std::string_view stem);
  result_persistence::StageOutcome stageCompletedChartAttempt(
      const result_persistence::PersistedChartResult &,
      const ir::IrSubmissionSnapshot &,
      const ReplayFileReference &,
      std::span<const ir::IrOutboxDraft>);
  ResultReadOutcome loadChartResult(int resultId);
  ir::IrSubmissionSnapshotReadOutcome
  loadIrSubmissionSnapshot(std::string_view attemptId);
};
```

`ReplaySummary` keeps its public `id` field for list identity but it now means
result ID. Replace event/touch counts with `replayFileState` and optional byte
size. Rename `PendingChartScoreWrite::replayId` to `resultId`,
`StageReceipt::replayId` to `resultId`, and acknowledgement parameters the
same way throughout score projection tests and call sites.

Schema v11 tables and required keys:

```sql
chart_results(id INTEGER PRIMARY KEY, attempt_id TEXT UNIQUE,
              chart_path TEXT NOT NULL, chart_md5 TEXT NOT NULL,
              chart_sha256 TEXT NOT NULL, chart_title TEXT NOT NULL,
              chart_artist TEXT NOT NULL, key_mode INTEGER NOT NULL,
              long_note_mode INTEGER NOT NULL, score INTEGER NOT NULL,
              max_score INTEGER NOT NULL, max_combo INTEGER NOT NULL,
              combo_break INTEGER NOT NULL, p_great INTEGER NOT NULL,
              great INTEGER NOT NULL, good INTEGER NOT NULL,
              bad INTEGER NOT NULL, poor INTEGER NOT NULL,
              k_poor INTEGER NOT NULL, fast INTEGER NOT NULL,
              slow INTEGER NOT NULL, final_gauge REAL NOT NULL,
              clear_type INTEGER NOT NULL, gauge_history_json TEXT NOT NULL,
              judgement_timing_json TEXT, provenance_json TEXT NOT NULL,
              result_fingerprint TEXT NOT NULL,
              played_at_unix_ms INTEGER NOT NULL);
course_results(id INTEGER PRIMARY KEY, attempt_id TEXT UNIQUE,
               course_key TEXT NOT NULL, legacy_course_id INTEGER NOT NULL,
               course_name TEXT NOT NULL, course_group_name TEXT NOT NULL,
               constraint_json TEXT NOT NULL,
               completed_charts INTEGER NOT NULL,
               total_charts INTEGER NOT NULL,
               final_score INTEGER NOT NULL, max_score INTEGER NOT NULL,
               max_combo INTEGER NOT NULL, final_gauge REAL NOT NULL,
               clear_type INTEGER NOT NULL, provenance_json TEXT NOT NULL,
               result_fingerprint TEXT NOT NULL,
               played_at_unix_ms INTEGER NOT NULL);
course_result_stages(
    course_result_id INTEGER NOT NULL, stage_index INTEGER NOT NULL,
    chart_path TEXT NOT NULL, chart_md5 TEXT NOT NULL,
    chart_sha256 TEXT NOT NULL, chart_title TEXT NOT NULL,
    chart_artist TEXT NOT NULL, key_mode INTEGER NOT NULL,
    long_note_mode INTEGER NOT NULL, score INTEGER NOT NULL,
    max_score INTEGER NOT NULL, max_combo INTEGER NOT NULL,
    combo_break INTEGER NOT NULL, p_great INTEGER NOT NULL,
    great INTEGER NOT NULL, good INTEGER NOT NULL,
    bad INTEGER NOT NULL, poor INTEGER NOT NULL, k_poor INTEGER NOT NULL,
    fast INTEGER NOT NULL, slow INTEGER NOT NULL,
    final_gauge REAL NOT NULL, clear_type INTEGER NOT NULL,
    gauge_history_json TEXT NOT NULL, judgement_timing_json TEXT,
    provenance_json TEXT NOT NULL,
    PRIMARY KEY(course_result_id, stage_index),
    FOREIGN KEY(course_result_id) REFERENCES course_results(id));
replay_files(id INTEGER PRIMARY KEY,
             chart_result_id INTEGER UNIQUE,
             course_result_id INTEGER UNIQUE,
             stem TEXT NOT NULL,
             history_index INTEGER NOT NULL, relative_path TEXT UNIQUE NOT NULL,
             content_sha256 TEXT NOT NULL, compressed_size INTEGER NOT NULL,
             codec_version INTEGER NOT NULL,
             CHECK((chart_result_id IS NOT NULL) !=
                   (course_result_id IS NOT NULL)),
             UNIQUE(stem, history_index),
             FOREIGN KEY(chart_result_id) REFERENCES chart_results(id),
             FOREIGN KEY(course_result_id) REFERENCES course_results(id));
replay_file_reservations(attempt_id TEXT PRIMARY KEY, stem TEXT NOT NULL,
                         history_index INTEGER NOT NULL,
                         relative_path TEXT UNIQUE NOT NULL,
                         created_at_unix_ms INTEGER NOT NULL,
                         UNIQUE(stem, history_index));
ir_submission_snapshots(attempt_id TEXT PRIMARY KEY,
                        schema_version INTEGER NOT NULL,
                        payload_json TEXT NOT NULL,
                        fingerprint TEXT NOT NULL);
```

Pending score rows reference `chart_results(id)` and attempt ID. IR tables
continue to use attempt ID. Add foreign keys and indexes for result ordering,
chart hashes, course identity, replay record association, reservation
allocation, and IR candidate lookup. Do not create the legacy event/touch/
lane-cover tables in a fresh database.

- [ ] **Step 1: Replace fresh-schema tests first**

Assert schema version 11, exact table/index/foreign-key presence, absence of
legacy event tables, one compact result/reference/snapshot row for a fixture
with 100,000 input transitions, result ID continuity, and snapshot/result
load validation. Assert direct SQL attempts to reuse stem/index, path,
attempt ID, or record association fail.

Reservation tests must prove repeated attempt/stem returns the same row,
attempt/stem conflict fails, concurrent distinct attempts get consecutive
indexes, deleted files/results do not lower the next index, and an existing
highest reservation is included in allocation.

- [ ] **Step 2: Run RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target replay_repository_tests -j 6
```

Expected: new schema assertions fail because version 10 and legacy event
tables are still active.

- [ ] **Step 3: Implement fresh schema and compact record readers/writers**

Split new result SQL out of the existing 3,000-line records file. Bind every
field explicitly, validate canonical fingerprints on read, cap vector/JSON
sizes, and keep diagnostic text free of replay bytes and credentials. Use
`BEGIN IMMEDIATE` during index reservation and compute
`MAX(history_index) + 1` across both references and reservations for the
requested stem with checked 64-bit arithmetic.

At this task boundary, schema-v10 opening calls a clearly separated
`migrateReplaySchema10To11` stub that returns a blocking
`MigrationRequired` outcome. This prevents production from silently creating
an empty v11 database over v10 data. Task 8 replaces only that stub with the
real migrator.

- [ ] **Step 4: Run GREEN for fresh databases and commit**

Run:

```sh
cmake --build cmake-build-debug --target replay_repository_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^replay_repository_tests$'
git diff --check
```

Expected: fresh-schema and compact-record tests pass. Legacy-v10 cases assert
the explicit fail-closed `MigrationRequired` outcome; there is no fallback
that discards v10 data.

Commit:

```sh
git add src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryInternal.h \
  src/repositories/ReplayRepositorySchema.cpp \
  src/repositories/ReplayRepositoryResultRecords.cpp \
  src/repositories/ReplayRepositoryRecords.cpp \
  tests/RepositorySqliteTestSupport.h \
  tests/replay_repository_tests.cpp src/CMakeLists.txt CMakeLists.txt
git commit -m "feat: add compact replay result schema"
```

---

### Task 7: Coordinate reservation, file finalization, and compact result commit

**Files:**

- Modify: `src/ResultPersistenceCoordinator.h`
- Modify: `src/ResultPersistenceCoordinator.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryResultRecords.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `tests/result_persistence_coordinator_tests.cpp`
- Modify: `tests/result_persistence_integration_tests.cpp`
- Modify: `tests/application_result_recovery_tests.cpp`
- Modify: `tests/replay_repository_tests.cpp`

**Coordinator contract:**

```cpp
namespace result_persistence {
struct Dependencies {
  std::function<ReservationOutcome(std::string_view, std::string_view)> reserve;
  std::function<replay::FinalizeOutcome(
      const replay::ReplayPathIdentity &, std::span<const std::byte>,
      const replay::ExpectedReplayIdentity &, std::string_view)> finalizeReplay;
  std::function<StageOutcome(
      const PersistedChartResult &, const ir::IrSubmissionSnapshot &,
      const ReplayFileReference &, std::span<const ir::IrOutboxDraft>)> stage;
  std::function<PendingReadOutcome(std::string_view)> loadPending;
  std::function<PendingBatchOutcome(std::size_t)> listPending;
  std::function<ProjectionOutcome(const PendingChartScoreWrite &)> project;
  std::function<AcknowledgeOutcome(std::string_view, int)>
      acknowledgeAndActivate;
  std::function<RecoveryMarkOutcome(std::string_view, RecoveryAttemptKind)>
      recordRecoveryAttempt;
};

class Coordinator {
public:
  Coordinator(ScoreRepository &, ReplayRepository &,
              replay::ReplayFileStore &, replay::BeatorajaReplayCodec &);
  explicit Coordinator(Dependencies);
  SaveOutcome persist(const CompletedChartAttempt &,
                      std::span<const ir::IrOutboxDraft> = {});
};
} // namespace result_persistence
```

`persist` performs these phases in order and returns a durable state only
after phase 5:

1. Validate independent result, snapshot, replay, and draft association.
2. Derive the Beatoraja stem and reserve one stable history index/path by
   attempt ID.
3. Deterministically encode and finalize/read-back the `.brd`.
4. In one SQLite transaction, insert-or-verify result, replay reference,
   snapshot, pending score projection, and automatic provider drafts; consume
   the exact reservation.
5. Project/acknowledge the score with the existing cross-database protocol.

`SaveOutcome` distinguishes `UnfinalizedReplay`, `Unstaged`, `PendingScore`,
`PendingAcknowledgement`, and integrity-conflict states without exposing
paths, attempt IDs, SQL, or replay data in `userMessage`.

- [ ] **Step 1: Rewrite coordinator unit tests around phase ordering**

Use dependency spies to prove validation stops before reservation, reservation
failure stops before encoding/finalization, file failure stops before stage,
stage failure stops before projection, and successful stage retains the
current pending-score recovery behavior. Assert a retry uses the same
reservation, `AlreadyStaged` requires identical independent fingerprints and
file metadata, and a conflicting result/snapshot/file fails closed.

- [ ] **Step 2: Add filesystem/SQLite integration fault tests**

Inject termination-equivalent failures before/after temp write, file sync,
rename, directory sync, read-back, result `BEGIN`, every insert group,
reservation delete, and result commit. Reopen repository/store after each
failure and assert:

- no database reference ever points to an unfinished/missing file;
- post-rename orphan files are reused by the same attempt;
- no retry allocates a second history index;
- no score projection occurs before the result transaction;
- a retry after ambiguous commit produces one result/reference/snapshot/file;
- input count changes file size only, never SQLite row count.

- [ ] **Step 3: Run RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target \
  result_persistence_coordinator_tests \
  result_persistence_integration_tests -j 6
```

Expected: compile fails because `Coordinator::persist` still accepts the
temporary replay-bearing
`legacy_result_persistence::LegacyChartResultAttempt`.

- [ ] **Step 4: Implement the coordinator and transactional stage**

Hold one outer `profile_database_activity::WriteGuard` across reservation,
file finalization, result staging, and score projection. Keep each SQLite
transaction short; never leave one open while writing the file. In
`stageCompletedChartAttempt`, compare existing result, snapshot, and
reference fingerprints/metadata before returning `AlreadyStaged`, and delete
only the matching reservation inside the successful transaction.

- [ ] **Step 5: Switch live chart completion to `CompletedChartAttempt`**

In `GamePlayScene::scheduleResultTransition`, capture
`PersistedChartResult`, `IrSubmissionSnapshot`, and raw
`ReplayPlaybackData`; build automatic IR drafts from the snapshot; then call
the new coordinator. `ResultPersistenceOptions` retains the result and
snapshot for immediate result UI/retry but does not require replay bytes for
IR. Remove all assignments of final score/provenance into the new replay
payload.

- [ ] **Step 6: Run GREEN and commit**

Run:

```sh
cmake --build cmake-build-debug --target \
  result_persistence_coordinator_tests \
  result_persistence_integration_tests \
  application_result_recovery_tests replay_repository_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(result_persistence_coordinator_tests|result_persistence_integration_tests|application_result_recovery_tests|replay_repository_tests)$'
git diff --check
```

Expected: all focused tests pass on fresh v11 databases.

Commit:

```sh
git add src/ResultPersistenceCoordinator.h \
  src/ResultPersistenceCoordinator.cpp \
  src/LegacyChartResultAttempt.h src/LegacyChartResultAttempt.cpp \
  src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryResultRecords.cpp \
  src/scene/play/GamePlayScene.cpp src/scene/ResultScene.h \
  src/scene/ResultScene.cpp \
  tests/result_persistence_coordinator_tests.cpp \
  tests/result_persistence_integration_tests.cpp \
  tests/application_result_recovery_tests.cpp \
  tests/replay_repository_tests.cpp
git commit -m "feat: persist replay files with compact results"
```

---

### Task 8: Atomically migrate schema-v10 replay rows into `.brd` files

**Files:**

- Create: `src/repositories/ReplayRepositoryReplayFileMigration.h`
- Create: `src/repositories/ReplayRepositoryReplayFileMigration.cpp`
- Create: `tests/replay_file_migration_tests.cpp`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Modify: `src/repositories/ReplayRepositoryInternal.h`
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `tests/player_profile_manager_tests.cpp`
- Modify: `tests/profile_switch_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Migration contract:**

```cpp
namespace replay_repository_detail {
struct ReplayMigrationFaults {
  std::function<bool(std::string_view phase, std::int64_t publicId)> failAt;
};

struct ReplayMigrationOutcome {
  enum class Status { Migrated, AlreadyCurrent, InvalidLegacyData,
                      FileFailure, StorageFailure };
  Status status = Status::StorageFailure;
  std::size_t chartFiles = 0;
  std::size_t courseFiles = 0;
  std::string diagnostic;
};

ReplayMigrationOutcome migrateReplaySchema10To11(
    sqlite3 *database, const std::filesystem::path &profileRoot,
    const replay::BeatorajaReplayCodec &,
    replay::ReplayFileStore &, ReplayMigrationFaults = {});
} // namespace replay_repository_detail
```

The migrator owns a migration-only `LegacyReplaySnapshot` decoder. Normal
repository, result recall, and IR code cannot call it. Load and validate the
entire v10 chart/course header, event, touch, lane-cover, result-attempt,
gauge/timing, pending-score, outbox, receipt, and remote-mirror state before
writing any file.

Assign paths deterministically by exact stem, then creation timestamp and
public integer ID. For each legacy replay, encode stock press/release input
where representable and always preserve the complete old playback stream in
`asobmashow.legacyPlaybackEvents` when scratch direction or outcome
annotations cannot be represented. Mark `stockScratchDirectionBestEffort`
for direction-sensitive legacy charts.

- [ ] **Step 1: Build a real schema-v10 migration fixture**

Create the fixture by running the v10 DDL/data builders retained in test
support, not hand-written partial tables. Include:

- two same-stem chart replays with IDs/timestamps out of insertion order;
- undefined-LN modes 0/1/2;
- normal and scratch-direction-sensitive event streams;
- touch and timed cover rows;
- one partial and one complete course with rest timing;
- result attempt gauge/timing facts and provenance;
- pending score rows, ready/blocked/succeeded outbox rows, receipts, and
  remote mirrors;
- a legacy row with no independent IR snapshot.

Snapshot every v10 table and its row count before migration.

- [ ] **Step 2: Write success and exhaustive fault tests**

On success, assert exact public IDs, result/provenance facts, course stage
order, pending/outbox/receipt/mirror contents, deterministic path indexes,
decoded extension data, file hashes/sizes, `foreign_key_check`, schema version
11, and absence of every old replay/event table. Assert the record lacking a
snapshot cannot initiate a new manual upload.

For every file and migration phase, inject failure at legacy read, encode,
temp write, rename, read-back, pre-cutover revalidation, schema create, each
copy group, count verification, foreign-key verification, legacy drop,
version update, and commit. After reopening, assert `user_version == 10` and
all source rows equal the snapshot. Retry without the fault and assert
existing deterministic final files are validated/reused.

- [ ] **Step 3: Run RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target replay_file_migration_tests -j 6
```

Expected: compile fails because the migration interface is missing.

- [ ] **Step 4: Implement all-files-first staging**

Outside any SQLite write transaction, read v10 rows into bounded in-memory
snapshots, assign all identities, finalize every file, and validate every
file again. Existing deterministic paths may be reused only when hash and
decoded chart/course identity exactly match. On mismatch, stop and preserve
v10 as authoritative; never overwrite the conflicting file.

- [ ] **Step 5: Implement one-transaction cutover**

Start `BEGIN IMMEDIATE` only after final revalidation. Create v11 tables,
copy compact result/course facts preserving IDs, insert references, copy only
independently valid IR snapshots, relink pending rows, copy provider outbox
payloads and receipts unchanged, verify source/destination counts and foreign
keys, drop all v10 structures, set `user_version = 11`, and commit once. Any
error executes rollback and returns a bounded blocking diagnostic.

- [ ] **Step 6: Activate migration from repository/profile startup**

`ReplayRepository::EnsureSchema` obtains the existing profile database write
guard and invokes the migrator before scenes can observe the profile.
Profile validation treats v10 as migratable and v11 as current; future
versions still fail closed. Remove Task 6's temporary fail-closed migration
expectations and restore success expectations in all repository/profile
tests.

- [ ] **Step 7: Run GREEN and commit**

Run:

```sh
cmake --build cmake-build-debug --target \
  replay_file_migration_tests replay_repository_tests \
  player_profile_manager_tests profile_switch_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(replay_file_migration_tests|replay_repository_tests|foundation_profile_manager|foundation_profile_switch)$'
git diff --check
```

Expected: all success/fault/retry cases pass; no schema-v10 database is
partially modified.

Commit:

```sh
git add src/repositories/ReplayRepositoryReplayFileMigration.h \
  src/repositories/ReplayRepositoryReplayFileMigration.cpp \
  src/repositories/ReplayRepositorySchema.cpp \
  src/repositories/ReplayRepositoryInternal.h \
  tests/replay_file_migration_tests.cpp \
  tests/replay_repository_tests.cpp \
  tests/player_profile_manager_tests.cpp tests/profile_switch_tests.cpp \
  src/CMakeLists.txt CMakeLists.txt
git commit -m "feat: migrate replay rows to brd files"
```

---

### Task 9: Read `.brd` playback while result recall and manual IR use SQLite only

**Files:**

- Create: `src/replay/ReplayPlaybackDriver.h`
- Create: `src/replay/ReplayPlaybackDriver.cpp`
- Create: `src/replay/ReplayPlaybackMaterializer.h`
- Create: `src/replay/ReplayPlaybackMaterializer.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryResultRecords.cpp`
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/IrUploadsScene.cpp`
- Modify: `src/ResultRecallBuilder.h`
- Modify: `src/ResultRecallBuilder.cpp`
- Modify: `src/ReplayVideoExporter.h`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `tests/gameplay_playback_startup_tests.cpp`
- Modify: `tests/replay_keysound_schedule_tests.cpp`
- Modify: `tests/result_recall_builder_tests.cpp`
- Modify: `tests/ir_saved_result_upload_tests.cpp`
- Modify: `tests/ir_saved_result_batch_upload_tests.cpp`
- Modify: `tests/replay_repository_tests.cpp`
- Create: `tests/replay_playback_driver_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
namespace replay {
class ReplayPlaybackDriver {
public:
  using CommandCallback = std::function<void(const LogicalControl &, bool)>;
  ReplayPlaybackDriver(const ReplayPlaybackData &, IRhythmControl &,
                       CommandCallback = {});
  void advanceTo(std::int64_t songTimeMicros);
  void reset();
  [[nodiscard]] bool finished() const noexcept;
};

struct MaterializedReplay {
  std::vector<GameplayReplayEvent> judgedEvents;
  GameplayAttemptSnapshot attempt;
  std::vector<float> gaugeHistory;
};

MaterializeOutcome materializeReplay(
    const ReplayPlaybackData &, const bms_parser::Chart &,
    const gameplay::GameplayRulesetPolicy &);
} // namespace replay

namespace result_recall {
using ResultChartLoader = std::function<std::unique_ptr<bms_parser::Chart>(
    const result_persistence::PersistedChartResult &, std::atomic_bool &)>;

ChartBuildOutcome BuildChartResult(
    result_persistence::PersistedChartResult,
    std::atomic_bool &, ResultChartLoader = {});
CourseBuildOutcome BuildCourseResult(
    result_persistence::PersistedCourseResult,
    std::atomic_bool &, ResultChartLoader = {});
} // namespace result_recall
```

Add `std::shared_ptr<const replay::ReplayPlaybackData> replayPlayback` to
`StartOptions`. The driver maps both scratch directions to the correct
physical scratch lane while preserving direction for BSS/backspin semantics,
and uses ordinary `IRhythmControl` input so new replays are rejudged by the
recorded playback configuration. Migrated files with
`legacyPlaybackEvents` use an isolated legacy playback adapter for Watch and
video only; that adapter cannot return a result or `IrSubmission`.

`BuildChartResult` populates the read-only result `RhythmState` directly from
`PersistedChartResult` fields. It may load a chart for title/background/assets
but never opens/decodes a replay, advances gameplay, compares a reconstructed
outcome, or computes IR facts. Manual/batch upload calls
`loadIrSubmissionSnapshot(attemptId)` and hands that snapshot to the selected
driver; retry continues to use the stored provider payload.

- [ ] **Step 1: Write raw playback and stock-import tests**

Decode the golden stock chart, drive a fake `IRhythmControl`, and assert exact
timestamp order, lanes, presses/releases, scratch reversals, Start/Select
handling, reset, and final state. Run the same raw stream through
`materializeReplay` and a live `GameplaySimulation` fixture and compare
judgements/gauge/score. Assert unsupported extension data never changes stock
input handling and the migration-only adapter is selected only when the
legacy track exists.

- [ ] **Step 2: Rewrite result recall tests to delete the replay first**

Persist a result/snapshot/file, delete the `.brd`, then assert:

- `BuildChartResult` and course result recall still reproduce score,
  judgements, timing, gauge history, clear state, provenance display, and
  played time;
- single and batch manual IR draft construction still succeeds from the
  stored snapshot;
- ready/active/retry outbox processing never invokes a replay-store spy;
- Watch, G-Battle, and video loading fail with replay-unavailable status;
- a record without an independently valid snapshot suppresses manual upload
  and never falls back to reconstruction.

- [ ] **Step 3: Run RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target \
  replay_playback_driver_tests result_recall_builder_tests \
  ir_saved_result_upload_tests ir_saved_result_batch_upload_tests -j 6
```

Expected: tests fail because playback/result/IR still consume `ReplayData`.

- [ ] **Step 4: Implement file-backed Watch and video consumers**

Load a reference, inspect/hash it, then decode it immediately before replay-
dependent actions. Apply starting config and timed cover/touch samples from
the playback model. Use `ReplayPlaybackMaterializer` for video, pacemaker,
ghost, and other consumers that need judged annotations; never persist its
derived result and never expose it to IR.

- [ ] **Step 5: Replace result recall and manual IR reconstruction**

Remove `HistoricalIrContext` replay reconstruction. In `MainMenuScene` and
`IrUploadsScene`, load `PersistedChartResult` for View Result and
`IrSubmissionSnapshot` for new uploads. Preserve provider eligibility and
account checks, but remove calls to `makeChartResultAttempt`,
`makeIrSubmission` from replay, and replay/result fingerprint comparison.

- [ ] **Step 6: Add dependency audits**

Run as tests or verification commands:

```sh
! rg -n 'Replay(ResultStateBuilder|Data)|LoadReplay(Result)?|reconstruct' \
  src/ResultRecallBuilder.* src/scene/IrUploadsScene.cpp \
  src/ir/IrSubmission.*
! rg -n 'ReplayFile(Store|Reference)|\.brd|LoadReplay' \
  src/ir src/repositories/ReplayRepositoryIrOutbox.cpp
```

Expected: no result-recall/manual-IR reconstruction or replay-file dependency.

- [ ] **Step 7: Run GREEN and commit**

Run:

```sh
cmake --build cmake-build-debug --target \
  replay_playback_driver_tests gameplay_playback_startup_tests \
  replay_keysound_schedule_tests result_recall_builder_tests \
  ir_saved_result_upload_tests ir_saved_result_batch_upload_tests \
  replay_repository_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(replay_playback_driver_tests|gameplay_playback_startup_tests|replay_keysound_schedule_tests|result_recall_builder_tests|ir_saved_result_upload_tests|ir_saved_result_batch_upload_tests|replay_repository_tests)$'
git diff --check
```

Expected: all commands and dependency audits pass.

Commit:

```sh
git add src/replay/ReplayPlaybackDriver.h \
  src/replay/ReplayPlaybackDriver.cpp \
  src/replay/ReplayPlaybackMaterializer.h \
  src/replay/ReplayPlaybackMaterializer.cpp \
  src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryResultRecords.cpp \
  src/scene/play/GamePlayStartOptions.h src/scene/play/GamePlayScene.h \
  src/scene/play/GamePlayScene.cpp src/scene/MainMenuScene.cpp \
  src/scene/IrUploadsScene.cpp src/ResultRecallBuilder.h \
  src/ResultRecallBuilder.cpp src/ReplayVideoExporter.h \
  src/ReplayVideoExporter.cpp tests/gameplay_playback_startup_tests.cpp \
  tests/replay_keysound_schedule_tests.cpp \
  tests/result_recall_builder_tests.cpp \
  tests/ir_saved_result_upload_tests.cpp \
  tests/ir_saved_result_batch_upload_tests.cpp \
  tests/replay_repository_tests.cpp tests/replay_playback_driver_tests.cpp \
  src/CMakeLists.txt CMakeLists.txt
git commit -m "refactor: separate replay playback from result recall"
```

---

### Task 10: Persist and play course replays through one Beatoraja course file

**Files:**

- Modify: `src/ResultPersistenceModel.h`
- Modify: `src/ResultPersistenceModel.cpp`
- Modify: `src/CoursePlaySession.h`
- Modify: `src/ResultPersistenceCoordinator.h`
- Modify: `src/ResultPersistenceCoordinator.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryResultRecords.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `tests/course_identity_tests.cpp`
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `tests/result_persistence_integration_tests.cpp`
- Modify: `tests/result_recall_builder_tests.cpp`
- Modify: `tests/gameplay_playback_startup_tests.cpp`

**Models/API:** The persisted course/result structs are introduced beside the
chart models in Task 4 so migration can use them. This task activates their
normal runtime path and adds:

```cpp
namespace result_persistence {
SaveOutcome Coordinator::persistCourse(const CompletedCourseAttempt &);
} // namespace result_persistence
```

The file is a Beatoraja JSON array at the course stem from Task 1. Its stage
objects use stock fields; `asobmashow` retains stage rest timing and other
playback-only additions. The result/course tables hold summary and per-stage
result/provenance facts without input arrays.

- [ ] **Step 1: Add failing course commit/playback tests**

Cover completed and partial courses, 2+ stages with repeated chart hashes,
LN prefix/constraint stem formation, deterministic rest times, option/seeds,
course gauge continuation, stage order, file retry/idempotency, result recall
after deletion, migrated legacy course playback, and stock Beatoraja
course-array decoding. Reject zero completed stages, stage/result/file count
mismatch, wrong stage chart identity, unsafe path, and invalid constraints.

- [ ] **Step 2: Run RED**

Run:

```sh
cmake --build cmake-build-debug --target \
  result_persistence_integration_tests replay_repository_tests \
  result_recall_builder_tests gameplay_playback_startup_tests -j 6
```

Expected: course tests fail because `SaveCourseReplay` still accepts the old
row-bearing model.

- [ ] **Step 3: Implement the course vertical slice**

Accumulate `ReplayPlaybackData` stages in `CoursePlaySession`. At course
result completion, capture `PersistedCourseResult`, reserve/finalize one
course `.brd`, and atomically stage compact course rows/reference. Load course
Watch from the file and View Result from compact rows. Reuse the same file
validation and legacy playback boundary as chart replays.

- [ ] **Step 4: Remove normal `SaveCourseReplay` row persistence and run GREEN**

Run:

```sh
cmake --build cmake-build-debug --target \
  course_identity_tests result_persistence_integration_tests \
  replay_repository_tests result_recall_builder_tests \
  gameplay_playback_startup_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(course_identity_tests|result_persistence_integration_tests|replay_repository_tests|result_recall_builder_tests|gameplay_playback_startup_tests)$'
git diff --check
```

Expected: all focused chart/course tests pass and no new course event rows are
created.

Commit:

```sh
git add src/ResultPersistenceModel.h src/ResultPersistenceModel.cpp \
  src/CoursePlaySession.h src/ResultPersistenceCoordinator.h \
  src/ResultPersistenceCoordinator.cpp src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryResultRecords.cpp \
  src/scene/play/GamePlayScene.cpp src/scene/ResultScene.cpp \
  src/scene/MainMenuScene.cpp \
  tests/course_identity_tests.cpp tests/replay_repository_tests.cpp \
  tests/result_persistence_integration_tests.cpp \
  tests/result_recall_builder_tests.cpp \
  tests/gameplay_playback_startup_tests.cpp
git commit -m "feat: store course replays as brd files"
```

---

### Task 11: Expose truthful replay availability, share, and file-only deletion

**Files:**

- Create: `src/replay/ReplayFileActionService.h`
- Create: `src/replay/ReplayFileActionService.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryResultRecords.cpp`
- Modify: `src/ResultRecordSummary.h`
- Modify: `src/ResultRecordSummary.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/PlatformDocumentHandoff.h`
- Modify: `src/PlatformDocumentHandoff.cpp`
- Modify: `tests/replay_summary_list_tests.cpp`
- Modify: `tests/replay_summary_list_view_tests.cpp`
- Modify: `tests/result_record_summary_tests.cpp`
- Modify: `tests/result_record_list_view_tests.cpp`
- Modify: `tests/platform_document_handoff_tests.cpp`
- Create: `tests/replay_file_actions_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Behavior:**

```cpp
struct ResultRecordCapabilities {
  bool watch = false;
  bool gBattle = false;
  bool resultRecall = false;
  bool videoExport = false;
  bool shareReplay = false;
  bool deleteReplayFile = false;
  bool irUpload = false;
};

enum class ReplayAvailability { Available, Missing, Corrupt, Unsafe, IoFailure };

struct ReplayFileActionOutcome {
  ReplayAvailability availability = ReplayAvailability::IoFailure;
  bool changed = false;
  std::string diagnostic;
};

struct LocalResultRecordId {
  ReplayFileReference::RecordKind kind =
      ReplayFileReference::RecordKind::ChartResult;
  int resultId = 0;
  bool operator==(const LocalResultRecordId &) const = default;
};

class ReplayFileActionService {
public:
  ReplayFileActionService(ReplayRepository &, replay::ReplayFileStore &);
  ReplayFileActionOutcome inspect(LocalResultRecordId);
  ReplayFileActionOutcome remove(LocalResultRecordId);
  ReplayFileActionOutcome copyToBeatorajaSlot(LocalResultRecordId, int slot);
};
```

Availability is computed by safe resolution plus stored SHA-256 verification,
not a database boolean. Available local records enable Watch/G-Battle/video/
share/delete. Missing/corrupt/unsafe files disable replay-dependent actions;
View Result and valid IR upload remain independent. Remote-only records expose
none of the file actions.

Replace the current integer-only `LocalReplayRecordId` variant member with
`LocalResultRecordId` so chart result 7 and course result 7 have distinct
stable keys and cannot target each other's files.

- [ ] **Step 1: Add capability/action tests before UI code**

Test available, manually deleted, app-deleted, restored-identical,
restored-different, corrupt, unsafe, and I/O-failure states. App deletion must
remove only the `.brd`; assert result, score projection, snapshot, outbox,
receipt, remote mirrors, and replay reference remain byte-for-byte. Assert a
restored exact file automatically becomes available.

For sharing, assert `PlatformDocumentHandoff::ExportDocumentAsync` receives
the existing `.brd` with `application/octet-stream` and a Beatoraja filename,
without re-encoding. For history index >3, test both ordinary file export and
an explicit Copy to Beatoraja Slot 0..3 action that copies unchanged bytes to
the chosen filename and refuses overwrite unless the destination is already
identical.

- [ ] **Step 2: Run RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target \
  replay_file_actions_tests result_record_summary_tests \
  result_record_list_view_tests -j 6
```

Expected: capability fields and file action APIs are absent.

- [ ] **Step 3: Implement repository projections and file actions**

List queries load compact result/reference metadata. The action service then
uses `ReplayFileStore` outside the SQLite read transaction under the profile
activity guard, keeping the repository free of filesystem mutation. Return
bounded diagnostics and preserve rows for all missing/error states. File
deletion revalidates the path and hash immediately before removal and syncs
the directory; it does not issue SQL DELETE.

- [ ] **Step 4: Add Main Menu actions and confirmation**

Add `Share Replay` and `Delete Replay File` to the local record modal. The
delete confirmation states that only the replay file will be removed and the
score/result will remain. Disable repeat clicks during asynchronous export or
delete, handle scene/profile teardown, refresh the selected row after
completion, and keep View Result/IR status visible. Show concise Missing vs
Corrupt messaging without paths or hashes.

- [ ] **Step 5: Run GREEN and commit**

Run:

```sh
cmake --build cmake-build-debug --target \
  replay_file_actions_tests replay_summary_list_tests \
  replay_summary_list_view_tests result_record_summary_tests \
  result_record_list_view_tests platform_document_handoff_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(replay_file_actions_tests|replay_summary_list_tests|replay_summary_list_view_tests|result_record_summary_tests|result_record_list_view_tests|foundation_platform_document_handoff)$'
git diff --check
```

Expected: all file states and UI capabilities are truthful; deletion affects
only the file.

Commit:

```sh
git add src/replay/ReplayFileActionService.h \
  src/replay/ReplayFileActionService.cpp \
  src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryResultRecords.cpp \
  src/ResultRecordSummary.h src/ResultRecordSummary.cpp \
  src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp \
  src/PlatformDocumentHandoff.h src/PlatformDocumentHandoff.cpp \
  tests/replay_file_actions_tests.cpp tests/replay_summary_list_tests.cpp \
  tests/replay_summary_list_view_tests.cpp \
  tests/result_record_summary_tests.cpp \
  tests/result_record_list_view_tests.cpp \
  tests/platform_document_handoff_tests.cpp src/CMakeLists.txt CMakeLists.txt
git commit -m "feat: manage replay files from records"
```

---

### Task 12: Include `replay/` in profile lifecycle and portable archives

**Files:**

- Modify: `src/PlayerProfile.h`
- Modify: `src/PlayerProfileManager.cpp`
- Modify: `src/ProfileSessionCoordinator.cpp`
- Modify: `src/ProfileArchive.cpp`
- Modify: `src/ProfileExportStaging.cpp`
- Modify: `tests/player_profile_manager_tests.cpp`
- Modify: `tests/profile_switch_tests.cpp`
- Modify: `tests/profile_archive_tests.cpp`
- Modify: `tests/profile_export_staging_tests.cpp`

**Path rule:**

```cpp
struct PlayerProfilePaths {
  std::filesystem::path root;
  std::filesystem::path profileJson;
  std::filesystem::path settingsJson;
  std::filesystem::path inputJson;
  std::filesystem::path irCredentialsJson;
  std::filesystem::path bokutachiCacheJson;
  std::filesystem::path scoresDb;
  std::filesystem::path replaysDb;
  std::filesystem::path practiceDirectory;
  std::filesystem::path replayDirectory;
};
```

Profile creation makes this directory private. Duplicate, legacy-profile
migration, overwrite import, and profile installation stage `replays.db` and
`replay/` together before the existing durable directory rename. Validation
allows referenced files to be absent but rejects an unsafe path, link, or
escape.

Portable archives recursively include regular non-link `replay/*.brd`
members. The manifest and `checksums.sha256` cover each file. Extend existing
member-count, per-file compressed/expanded, total compressed/expanded, and
path-depth budgets to replay members. No other file type under `replay/` is
exported or accepted.

- [ ] **Step 1: Add profile lifecycle tests before implementation**

Assert new profile path/layout/permissions; duplicate copies all replay files
and preserves missing references; a profile switch binds repository/store to
the new root; failed duplicate/import leaves neither partial database nor
replay directory; future schema validation still fails closed; legacy profile
migration creates `replay/` and performs schema-v10 replay conversion before
activation.

- [ ] **Step 2: Add archive tests before implementation**

Round-trip multiple chart/course `.brd` files including indexes >3 and a
deliberately missing referenced file. Assert exact bytes/checksums/paths.
Reject traversal, absolute/backslash aliases, symlink entries, non-`.brd`
members, duplicate aliases, case collisions, too many files, oversized
individual/aggregate data, compressed bombs, checksum mismatch, database
references outside `replay/`, and changed replay bytes during export.

Add a concurrency fixture where an app-driven replay write/delete races an
export. The profile database activity guard must yield either the complete
before or complete after snapshot, never a mixed reference/file archive.

- [ ] **Step 3: Run RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target \
  player_profile_manager_tests profile_switch_tests \
  profile_archive_tests profile_export_staging_tests -j 6
```

Expected: new replay-directory and archive-member assertions fail.

- [ ] **Step 4: Implement profile directory lifecycle**

Add `root / "replay"` in `makePathsAtRoot`; apply private directory
permissions; extend staged-copy/install/rollback code with regular-file and
link checks. Bind the `ReplayFileStore` root atomically with repository path
during profile activation.

- [ ] **Step 5: Implement guarded archive copy/import**

During export, snapshot SQLite and copy/hash replay files under one profile
activity guard. A reference already missing is omitted; a file that vanishes
or changes during the guarded copy fails export. Validate archive members in
the private import workspace before copying them to profile staging and run
repository schema/file-reference validation before install.

- [ ] **Step 6: Run GREEN and commit**

Run:

```sh
cmake --build cmake-build-debug --target \
  player_profile_manager_tests profile_switch_tests \
  profile_archive_tests profile_export_staging_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(foundation_profile_manager|foundation_profile_switch|foundation_profile_archive|foundation_profile_export_staging)$'
git diff --check
```

Expected: all lifecycle/archive tests pass with exact replay bytes.

Commit:

```sh
git add src/PlayerProfile.h src/PlayerProfileManager.cpp \
  src/ProfileSessionCoordinator.cpp src/ProfileArchive.cpp \
  src/ProfileExportStaging.cpp tests/player_profile_manager_tests.cpp \
  tests/profile_switch_tests.cpp tests/profile_archive_tests.cpp \
  tests/profile_export_staging_tests.cpp
git commit -m "feat: include replay files in profiles"
```

---

### Task 13: Remove legacy runtime coupling and verify the complete cutover

**Files:**

- Create: `src/analysis/JudgedPlaybackData.h`
- Create: `tests/replay_decoupling_audit_tests.cpp`
- Modify: `src/ChartPlaybackDuration.h`
- Modify: `src/CoursePlaySession.h`
- Modify: `src/GBattleMode.h`
- Modify: `src/PlayOptionUtils.h`
- Modify: `src/ReplayAutoPlay.h`
- Modify: `src/ReplayGhostUtils.h`
- Modify: `src/ReplayVideoExporter.h`
- Modify: `src/ReplayVideoExporter.cpp`
- Modify: `src/ResultImageExporter.h`
- Modify: `src/ResultImageExporter.cpp`
- Modify: `src/ResultPresentationUtils.h`
- Modify: `src/audio/ChartAudioRenderer.h`
- Modify: `src/audio/ChartAudioRenderer.cpp`
- Modify: `src/input/RhythmInputHandler.h`
- Modify: `src/practice/PracticeAnalytics.h`
- Modify: `src/practice/PracticeAnalytics.cpp`
- Modify: `src/practice/PracticeLaunchRequest.h`
- Modify: `src/practice/PracticeLaunchRequest.cpp`
- Modify: `src/practice/PracticeResultFlow.h`
- Modify: `src/practice/PracticeResultFlow.cpp`
- Modify: `src/practice/PracticeResultModel.h`
- Modify: `src/practice/PracticeResultModel.cpp`
- Modify: `src/practice/PracticeSession.h`
- Modify: `src/practice/PracticeSession.cpp`
- Modify: `src/scene/ChartViewerScene.h`
- Modify: `src/scene/ChartViewerScene.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/scene/play/Pacemaker.h`
- Modify: `src/scene/play/BMSRenderer.h`
- Modify: `src/scene/play/BMSRenderer.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `src/scene/play/ReplayKeysoundSchedule.h`
- Modify: `src/scene/play/RhythmLaneInputController.h`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Delete: `src/ReplayResultStateBuilder.h`
- Delete: `src/ReplayResultStateBuilder.cpp`
- Delete: `src/ReplayData.h`
- Modify: `tests/gameplay_playback_startup_tests.cpp`
- Modify: `tests/gameplay_practice_input_boundary_tests.cpp`
- Modify: `tests/gameplay_ruleset_policy_tests.cpp`
- Modify: `tests/gbattle_tests.cpp`
- Modify: `tests/ir_submission_service_tests.cpp`
- Modify: `tests/practice_analytics_tests.cpp`
- Modify: `tests/practice_launch_tests.cpp`
- Modify: `tests/practice_preset_store_tests.cpp`
- Modify: `tests/practice_result_flow_tests.cpp`
- Modify: `tests/practice_result_model_tests.cpp`
- Modify: `tests/practice_rule_override_tests.cpp`
- Modify: `tests/practice_session_tests.cpp`
- Modify: `tests/replay_keysound_schedule_tests.cpp`
- Modify: `tests/replay_summary_list_tests.cpp`
- Modify: `tests/result_presentation_model_tests.cpp`
- Modify: `tests/score_provenance_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

`JudgedPlaybackData` is an in-memory analysis/presentation projection used by
practice, pacemakers, ghosts, and rendering. It may contain derived judgement
events and score progression but not `ScoreProvenance`, IR fields, repository
IDs, replay paths, or persistence methods. Persisted `.brd` input is converted
to it only by `ReplayPlaybackMaterializer`. Schema-v10 row structs remain
private to `ReplayRepositoryReplayFileMigration.cpp` and are not exposed by a
normal header.

- [ ] **Step 1: Add source and schema audits before removal**

Create a CTest source-audit target that requires:

```sh
! rg -n 'struct ReplayData|ChartResultAttempt|SaveReplay\(|SaveCourseReplay\(|LoadReplayResult' src
! rg -n 'replay_events|replay_touch_samples|replay_lane_cover_events' \
  src --glob '!repositories/ReplayRepositoryReplayFileMigration.cpp'
! rg -n 'ScoreProvenance|IrSubmission|attemptId|resultFingerprint' \
  src/replay src/analysis/JudgedPlaybackData.h
! rg -n 'legacyPlaybackEvents' src \
  --glob '!replay/BeatorajaReplayCodec.*' \
  --glob '!repositories/ReplayRepositoryReplayFileMigration.cpp' \
  --glob '!replay/ReplayPlaybackMaterializer.cpp'
```

Add a fresh-database SQL audit that asserts no table/trigger/view mentions the
three legacy event tables, and a persistence-size test that records 100 versus
100,000 transitions and asserts equal SQLite row counts with growth confined
to the two `.brd` file sizes.

- [ ] **Step 2: Run RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target replay_decoupling_audit_tests -j 6
```

Expected: the audit finds old replay/result reconstruction symbols and legacy
row SQL in normal production files.

- [ ] **Step 3: Replace remaining normal `ReplayData` consumers**

Move practice/pacemaker/ghost/render-only fields to `JudgedPlaybackData` or
narrow typed inputs. Make result image export accept the persisted result
presentation model. Make replay video export accept file playback plus a
materialized analysis projection. Remove provenance-based replay startup;
only playback configuration from `ReplayPlaybackData.setup` may affect replay
interpretation.

- [ ] **Step 4: Delete normal legacy repository and reconstruction code**

Remove `SaveReplay`, old `SaveCourseReplay`, `LoadReplayResult`, old
`ChartResultAttempt`, `makeChartResultAttempt`, replay-backed result
fingerprints, runtime row serializers/readers, and
`ReplayResultStateBuilder`. Keep the schema-v10 reader/DDL constants only in
the migration translation unit. Remove old sources from all CMake targets and
delete `ReplayData.h` after every normal consumer has a narrow replacement.

- [ ] **Step 5: Run focused regression suites**

Run:

```sh
cmake --build cmake-build-debug --target \
  replay_decoupling_audit_tests beatoraja_replay_path_tests \
  beatoraja_replay_codec_tests replay_file_store_tests \
  replay_input_recorder_tests replay_playback_driver_tests \
  replay_file_migration_tests replay_file_actions_tests \
  replay_repository_tests result_persistence_model_tests \
  result_persistence_coordinator_tests \
  result_persistence_integration_tests result_recall_builder_tests \
  ir_submission_snapshot_tests ir_saved_result_upload_tests \
  ir_saved_result_batch_upload_tests player_profile_manager_tests \
  profile_switch_tests profile_archive_tests \
  profile_export_staging_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '(replay|result_persistence|result_recall|ir_submission_snapshot|ir_saved_result|profile)'
```

Expected: all focused suites pass.

- [ ] **Step 6: Perform final repository verification**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
git status --short
```

Then manually smoke-test with a disposable profile copy:

1. Launch schema-v10 data and confirm migration produces deterministic files
   under `<profile>/replay/` and preserves record IDs.
2. Record a new scratch-direction and live lane-cover play; confirm its `.brd`
   opens in AsoBMaShow and stock Beatoraja (stock uses initial cover only).
3. Record at least five plays of one chart; confirm filenames use unsuffixed
   index 0 then `_1` through `_4`, and copying `_4` to a chosen 0..3 slot does
   not alter bytes.
4. Delete one replay file; confirm View Result and postponed IR remain usable
   while Watch, video, G-Battle, and Share disable.
5. Export/import and duplicate the disposable profile; confirm all present
   replay bytes survive and deliberately missing files remain non-fatal.

Expected: all automated commands and manual acceptance checks pass. Do not
claim completion if any migration, hash, dependency-audit, or full-suite
failure remains.

- [ ] **Step 7: Commit the cleanup**

```sh
git add src/analysis/JudgedPlaybackData.h \
  tests/replay_decoupling_audit_tests.cpp \
  src/ChartPlaybackDuration.h src/CoursePlaySession.h src/GBattleMode.h \
  src/PlayOptionUtils.h src/ReplayAutoPlay.h src/ReplayGhostUtils.h \
  src/ReplayVideoExporter.h src/ReplayVideoExporter.cpp \
  src/ResultImageExporter.h src/ResultImageExporter.cpp \
  src/ResultPresentationUtils.h src/audio/ChartAudioRenderer.h \
  src/audio/ChartAudioRenderer.cpp src/input/RhythmInputHandler.h \
  src/practice/PracticeAnalytics.h src/practice/PracticeAnalytics.cpp \
  src/practice/PracticeLaunchRequest.h \
  src/practice/PracticeLaunchRequest.cpp \
  src/practice/PracticeResultFlow.h src/practice/PracticeResultFlow.cpp \
  src/practice/PracticeResultModel.h src/practice/PracticeResultModel.cpp \
  src/practice/PracticeSession.h src/practice/PracticeSession.cpp \
  src/scene/ChartViewerScene.h src/scene/ChartViewerScene.cpp \
  src/scene/ResultScene.h src/scene/ResultScene.cpp \
  src/scene/play/Pacemaker.h src/scene/play/BMSRenderer.h \
  src/scene/play/BMSRenderer.cpp src/scene/play/GamePlayScene.h \
  src/scene/play/GamePlayScene.cpp src/scene/play/GamePlayStartOptions.h \
  src/scene/play/ReplayKeysoundSchedule.h \
  src/scene/play/RhythmLaneInputController.h \
  src/repositories/ReplayRepositoryRecords.cpp \
  src/repositories/ReplayRepositorySchema.cpp \
  src/ReplayResultStateBuilder.h src/ReplayResultStateBuilder.cpp \
  src/ReplayData.h src/CMakeLists.txt CMakeLists.txt \
  tests/gameplay_playback_startup_tests.cpp \
  tests/gameplay_practice_input_boundary_tests.cpp \
  tests/gameplay_ruleset_policy_tests.cpp tests/gbattle_tests.cpp \
  tests/ir_submission_service_tests.cpp tests/practice_analytics_tests.cpp \
  tests/practice_launch_tests.cpp tests/practice_preset_store_tests.cpp \
  tests/practice_result_flow_tests.cpp \
  tests/practice_result_model_tests.cpp \
  tests/practice_rule_override_tests.cpp tests/practice_session_tests.cpp \
  tests/replay_keysound_schedule_tests.cpp \
  tests/replay_summary_list_tests.cpp \
  tests/result_presentation_model_tests.cpp \
  tests/score_provenance_tests.cpp
git diff --cached --name-only
git commit -m "refactor: complete file-based replay cutover"
```

## Completion Checklist

- [ ] Every new chart/course attempt writes one validated `.brd` beneath the
  profile's Beatoraja-compatible `replay/` layout.
- [ ] Stock keyinput, filename stems, LN prefixes, course constraints, and
  slots 0..3 match the pinned Beatoraja fixtures.
- [ ] New raw input preserves scratch direction; timed lane cover and touch
  data round-trip through the Aso extension.
- [ ] SQLite v11 contains compact results, snapshots, references, and durable
  work only—no row-per-event structures.
- [ ] Result recall and every postponed/manual IR path pass after the replay
  file is deleted and have no replay reconstruction fallback.
- [ ] Schema-v10 migration is deterministic, resumable, and changes the
  database only in one final transaction after all files validate.
- [ ] App/manual deletion leaves result, provenance, scores, snapshots,
  outbox, receipts, and remote mirrors untouched.
- [ ] Profile duplicate/archive/switch operations preserve replay files and
  reject unsafe paths or changed bytes.
- [ ] Full CTest, desktop `main`, dependency audits, and `git diff --check`
  pass.
