#include "skin/beatoraja/SkinAcceptanceRecorder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
std::atomic<bool> rejectNextAllocation{false};
}

void *operator new(std::size_t size) {
  if (rejectNextAllocation.exchange(false, std::memory_order_relaxed)) {
    throw std::bad_alloc();
  }
  if (void *allocation = std::malloc(size)) {
    return allocation;
  }
  throw std::bad_alloc();
}

void operator delete(void *allocation) noexcept { std::free(allocation); }

void operator delete(void *allocation, std::size_t) noexcept {
  std::free(allocation);
}

namespace {

using namespace skin;
using namespace std::chrono_literals;

int failures = 0;

class CommaDecimalPunct final : public std::numpunct<char> {
protected:
  char do_decimal_point() const override { return ','; }
};

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string digest(char value) { return std::string(64, value); }

SkinAcceptanceActivationKey activation() {
  return {
      .profileId = {.opaque = "private-profile-must-not-be-exported"},
      .entry = {.package = {.directoryName = "private-package",
                            .collisionKey = "private-package"},
                .packageRelativePath = "private/resources/play7.luaskin",
                .collisionKey = "private/resources/play7.luaskin"},
      .revisionDigest = digest('a'),
      .configurationDigest = digest('b'),
  };
}

PlaySkinSessionIdentity identity(std::uint64_t serial = 41) {
  const auto key = activation();
  return {.sessionSerial = serial,
          .profileId = key.profileId,
          .entry = key.entry,
          .revisionDigest = key.revisionDigest,
          .configurationDigest = key.configurationDigest};
}

SkinAcceptanceScenarioContract contract(SkinAcceptanceRunKind kind) {
  SkinAcceptanceScenarioContract value;
  value.scenarioId =
      kind == SkinAcceptanceRunKind::Performance         ? "performance-normal"
      : kind == SkinAcceptanceRunKind::ResourceLifecycle ? "resource-lifecycle"
                                                         : "render-io-negative";
  value.kind = kind;
  value.expectedChartSha256 = digest('c');
  value.expectedLayoutId = "16:9-fit";
  value.expectedOpaqueGuardVectorSha256 = digest('d');
  if (kind == SkinAcceptanceRunKind::RenderIoNegative) {
    value.expectedDiagnosticCode = "skin_file_render_phase_denied";
    value.expectedFallbackAction =
        "discard_frame_disable_session_same_frame_builtin";
    value.expectedDeniedOperation = SkinRenderIoOperation::FilesystemRead;
  }
  return value;
}

SkinAcceptanceSessionFacts facts(std::uint64_t serial = 41) {
  return {.identity = identity(serial),
          .chartSha256 = digest('c'),
          .layoutId = "16:9-fit",
          .actualRefreshHz = 60,
          .observedOpaqueGuardVectorSha256 = digest('d')};
}

SkinFrameTelemetryEnvelope envelope(std::uint64_t frameSerial,
                                    std::int64_t visualTimeMicros,
                                    SkinRenderIoCounters counters = {},
                                    bool fallback = false,
                                    std::uint64_t sessionSerial = 41) {
  SkinFrameTelemetryEnvelope value;
  value.identity = identity(sessionSerial);
  value.sample.frameSerial = frameSerial;
  value.sample.visualTimeMicros = visualTimeMicros;
  value.sample.evaluationMicros = 50;
  value.sample.submissionMicros = 25;
  value.sample.residentBytes = 1024;
  value.sample.renderIo = counters;
  value.sample.fallback = fallback;
  value.contributions = kCompleteSkinTelemetryContributions;
  return value;
}

class FakeOverlayProvider final : public IAsyncSkinOverlayDigestProvider {
public:
  SkinOverlayDigestTicket
  beginDigest(const SkinAcceptanceActivationKey &) override {
    ++beginCalls;
    const auto ticket = nextSkinOverlayDigestTicket();
    polls[ticket.value] = {.state = SkinOverlayDigestPollState::Pending};
    return ticket;
  }

  SkinOverlayDigestPollResult
  pollDigest(SkinOverlayDigestTicket ticket) const noexcept override {
    ++pollCalls;
    const auto found = polls.find(ticket.value);
    return found == polls.end() ? SkinOverlayDigestPollResult{} : found->second;
  }

  void cancelDigest(SkinOverlayDigestTicket ticket) noexcept override {
    cancelled.push_back(ticket.value);
  }

  void shutdown() noexcept override {
    if (drained) {
      return;
    }
    ++shutdownCalls;
    drained = true;
  }

  void ready(SkinOverlayDigestTicket ticket, std::string value) {
    polls[ticket.value] = {.state = SkinOverlayDigestPollState::Ready,
                           .lowercaseSha256 = std::move(value)};
  }

  void fail(SkinOverlayDigestTicket ticket) {
    polls[ticket.value] = {.state = SkinOverlayDigestPollState::Ready,
                           .failure =
                               SkinDiagnostic{.code = "overlay_digest_failed",
                                              .message = "sanitized"}};
  }

  SkinOverlayDigestTicket latest() const {
    return polls.empty() ? SkinOverlayDigestTicket{}
                         : SkinOverlayDigestTicket{polls.rbegin()->first};
  }

  mutable std::uint64_t pollCalls = 0;
  std::uint64_t beginCalls = 0;
  std::uint64_t shutdownCalls = 0;
  bool drained = false;
  std::vector<std::uint64_t> cancelled;
  std::map<std::uint64_t, SkinOverlayDigestPollResult> polls;
};

struct WriterProbe {
  bool write(const std::filesystem::path &path,
             std::span<const std::byte> bytes, std::string &) {
    std::unique_lock lock(mutex);
    entered = true;
    writerThread = std::this_thread::get_id();
    condition.notify_all();
    condition.wait(lock, [this] { return released; });
    writtenPath = path;
    payload.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    ++writes;
    return succeeds;
  }

  void waitUntilEntered() {
    std::unique_lock lock(mutex);
    condition.wait(lock, [this] { return entered; });
  }

  void release() {
    std::lock_guard lock(mutex);
    released = true;
    condition.notify_all();
  }

  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool released = false;
  bool succeeds = true;
  std::uint64_t writes = 0;
  std::thread::id writerThread;
  std::filesystem::path writtenPath;
  std::string payload;
};

struct InjectedWriterCopyFailure {};

struct ThrowOnCopyAndNextAllocationWriter {
  explicit ThrowOnCopyAndNextAllocationWriter(WriterProbe &probe)
      : writer(&probe) {}

  ThrowOnCopyAndNextAllocationWriter(
      const ThrowOnCopyAndNextAllocationWriter &other)
      : writer(other.writer) {
    rejectNextAllocation.store(true, std::memory_order_relaxed);
    throw InjectedWriterCopyFailure{};
  }
  ThrowOnCopyAndNextAllocationWriter &
  operator=(const ThrowOnCopyAndNextAllocationWriter &) = delete;
  ThrowOnCopyAndNextAllocationWriter(
      ThrowOnCopyAndNextAllocationWriter &&) noexcept = default;
  ThrowOnCopyAndNextAllocationWriter &
  operator=(ThrowOnCopyAndNextAllocationWriter &&) noexcept = default;

  bool operator()(const std::filesystem::path &path,
                  std::span<const std::byte> bytes, std::string &error) const {
    return writer->write(path, bytes, error);
  }

  WriterProbe *writer = nullptr;
};

SkinAcceptanceRecorderDependencies dependencies(SkinAcceptanceRunKind kind,
                                                FakeOverlayProvider *provider,
                                                WriterProbe *writer) {
  return {
      .documentsRoot = "/sandbox/Documents",
      .buildIdentity = {.commit = std::string(40, '1'),
                        .configuration = "Release",
                        .cleanSource = true},
      .resolveScenario = [kind](std::string_view scenarioId)
          -> std::optional<SkinAcceptanceScenarioContract> {
        auto value = contract(kind);
        return value.scenarioId == scenarioId ? std::optional(value)
                                              : std::nullopt;
      },
      .overlayDigests = provider,
      .writeAtomic =
          [writer](const std::filesystem::path &path,
                   std::span<const std::byte> bytes, std::string &error) {
            return writer->write(path, bytes, error);
          },
  };
}

void recordExpectedNegativeEvidence(
    SkinAcceptanceRecorder &recorder,
    const SkinAcceptanceBoundScope &boundScope) {
  recorder.recordDiagnosticEvidence(
      boundScope,
      SkinDiagnostic{
          .code = "skin_file_render_phase_denied",
          .message = "private resource path and device name are stripped",
          .virtualPath = "private/resources/secret.png"});
  recorder.recordFallbackAction(
      boundScope, "discard_frame_disable_session_same_frame_builtin");
}

SkinAcceptanceExportPollResult
waitForExport(SkinAcceptanceRecorder &recorder,
              SkinAcceptanceExportTicket ticket) {
  for (int attempt = 0; attempt < 10'000; ++attempt) {
    const auto result = recorder.pollExport(ticket);
    if (result.state == SkinAcceptanceExportPollState::Ready) {
      return result;
    }
    std::this_thread::yield();
  }
  return {};
}

std::size_t countOccurrences(std::string_view value, std::string_view needle) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = value.find(needle, position)) != std::string_view::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

void completePerformanceRun(SkinAcceptanceRecorder &recorder,
                            std::uint64_t firstFrame = 1) {
  recorder.record(envelope(firstFrame, 1'000'000));
  recorder.record(envelope(firstFrame + 1, 31'000'000));
  recorder.record(envelope(firstFrame + 2, 211'000'000));
}

void testPerformanceStateMachineWorkerTicketAndSanitizedPayload() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  std::mutex diagnosticMutex;
  std::uint64_t diagnosticSnapshotCalls = 0;
  std::thread::id diagnosticSnapshotThread;
  std::vector<SkinDiagnostic> diagnosticHistory{
      {.code = "skin_lua_event_execution_failed",
       .message = "private-profile raw-device-label third-party-module-label",
       .virtualPath = "private/resources/third-party-file.png"},
      {.code = "scuro_private_module_label",
       .message = "raw-device-label"}};
  auto recorderDependencies =
      dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer);
  recorderDependencies.snapshotDiagnostics = [&] {
    std::lock_guard lock(diagnosticMutex);
    ++diagnosticSnapshotCalls;
    diagnosticSnapshotThread = std::this_thread::get_id();
    return diagnosticHistory;
  };
  SkinAcceptanceRecorder recorder(std::move(recorderDependencies));
  const auto stoppingThread = std::this_thread::get_id();

  expect(recorder.state() == SkinAcceptanceCaptureState::Idle,
         "recorder starts idle");
  expect(
      recorder.arm("run-0123456789abcdef", "performance-normal", activation()),
      "a clean typed activation arms");
  expect(recorder.state() == SkinAcceptanceCaptureState::Armed,
         "arm changes state");
  expect(recorder.bindSession(facts()).has_value(),
         "matching scene facts bind with an opaque recorder-issued scope");
  expect(recorder.state() == SkinAcceptanceCaptureState::WarmingUp,
         "performance binding starts warmup");
  const auto previousLocale = std::locale::global(
      std::locale(std::locale::classic(), new CommaDecimalPunct));
  completePerformanceRun(recorder);
  std::locale::global(previousLocale);

  const auto ticket = recorder.currentExportTicket();
  expect(ticket.has_value() && ticket->value != 0,
         "exact measurement completion creates a typed export ticket");
  writer.waitUntilEntered();
  {
    std::lock_guard lock(diagnosticMutex);
    expect(diagnosticSnapshotCalls == 1 &&
               diagnosticSnapshotThread == stoppingThread,
           "diagnostic history is snapshotted synchronously at export "
           "linearization");
    diagnosticHistory.clear();
  }
  expect(recorder.pollExport(*ticket).state ==
             SkinAcceptanceExportPollState::Pending,
         "accepted worker work polls pending without doing the write");
  expect(recorder.pollExport({ticket->value + 1000}).state ==
             SkinAcceptanceExportPollState::Unknown,
         "unknown tickets are distinguished");
  expect(recorder.currentExportTicket() == ticket,
         "closing and reopening a controller can recover the same ticket");
  writer.release();
  const auto result = waitForExport(recorder, *ticket);
  expect(result.state == SkinAcceptanceExportPollState::Ready &&
             result.result.has_value() && result.result->exported,
         "worker completion becomes one retained terminal result");
  expect(recorder.state() == SkinAcceptanceCaptureState::Exported,
         "a successful worker result reaches the exported state");
  expect(result.result &&
             result.result->documentsRelativePath ==
                 std::filesystem::path("SkinAcceptance") /
                     "run-0123456789abcdef.json" &&
             result.result->lowercaseSha256.size() == 64,
         "result exposes only the Files-visible relative path and digest");
  expect(writer.writerThread != std::this_thread::get_id(),
         "the injected writer runs outside the polling thread");
  expect(writer.payload.find("\"warmupStartMicros\":1000000") !=
                 std::string::npos &&
             writer.payload.find("\"recordingStartMicros\":31000000") !=
                 std::string::npos &&
             writer.payload.find("\"recordingEndMicros\":211000000") !=
                 std::string::npos,
         "export preserves trusted exact 30/180 visual-clock boundaries");
  expect(writer.payload.find("\"missedPresentationPercent\":0") !=
             std::string::npos,
         "performance percentages stay locale-independent JSON numbers on the "
         "iOS 14 formatter");
  expect(
      writer.payload.find("private-profile") == std::string::npos &&
          writer.payload.find("private-package") == std::string::npos &&
          writer.payload.find("secret.png") == std::string::npos &&
          writer.payload.find("raw-device-label") == std::string::npos &&
          writer.payload.find("third-party") == std::string::npos &&
          writer.payload.find("scuro") == std::string::npos &&
          writer.payload.find("pixel") == std::string::npos,
      "value-owned output leaks no raw profile, resource, device, or pixels");
  expect(writer.payload.find("\"diagnosticHistoryCodes\":[\"skin_lua_event_execution_failed\"]") !=
             std::string::npos,
         "the worker payload owns only sanitized diagnostic-history codes");
  expect(
      !recorder.arm("blocked-0123456789", "performance-normal", activation()),
      "an unacknowledged terminal result blocks a second arm");
  expect(recorder.acknowledgeExport(*ticket) &&
             recorder.state() == SkinAcceptanceCaptureState::Idle &&
             !recorder.currentExportTicket().has_value() &&
             recorder.pollExport(*ticket).state ==
                 SkinAcceptanceExportPollState::Unknown,
         "explicit acknowledgement releases the sole terminal result");

  {
    std::lock_guard lock(writer.mutex);
    writer.entered = false;
    writer.released = true;
  }
  expect(
      recorder.arm("repeat-0123456789", "performance-normal", activation()) &&
          recorder.bindSession(facts()).has_value(),
      "acknowledgement permits a later capture");
  completePerformanceRun(recorder, 10);
  const auto laterTicket = recorder.currentExportTicket();
  expect(laterTicket && laterTicket->value > ticket->value,
         "accepted export tickets are process-monotonic and never reused");
  const auto later = waitForExport(recorder, *laterTicket);
  expect(later.result && later.result->exported,
         "the sole worker slot can be reused after acknowledgement");
  expect(recorder.acknowledgeExport(*laterTicket),
         "later terminal result can also be acknowledged");
}

void testDiagnosticHistoryRejectsInternalNamespaceLookalikes() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  auto recorderDependencies =
      dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer);
  recorderDependencies.snapshotDiagnostics = [] {
    return std::vector<SkinDiagnostic>{
        {.code = "skin_file_render_phase_denied"},
        {.code = "skin_private-profile_device-007_third-party-module",
         .message = "private-profile device-007 third-party-module",
         .virtualPath = "private/resources/third-party-module.lua"}};
  };
  SkinAcceptanceRecorder recorder(std::move(recorderDependencies));
  expect(recorder.arm("history-bypass-01234", "performance-normal",
                      activation()) &&
             recorder.bindSession(facts()).has_value(),
         "diagnostic allowlist bypass fixture binds");
  completePerformanceRun(recorder);
  const auto ticket = recorder.currentExportTicket();
  const auto result = ticket ? waitForExport(recorder, *ticket)
                             : SkinAcceptanceExportPollResult{};
  expect(result.result && result.result->exported &&
             writer.payload.find(
                 "\"diagnosticHistoryCodes\":[\"skin_file_render_phase_denied\"]") !=
                 std::string::npos &&
             writer.payload.find("private-profile") == std::string::npos &&
             writer.payload.find("device-007") == std::string::npos &&
             writer.payload.find("third-party-module") == std::string::npos,
         "namespace-shaped unknown codes cannot bypass diagnostic sanitization");
}

void testStrictBindingAndPerformanceFailures() {
  auto runBadBind = [](auto mutate, std::string_view message) {
    FakeOverlayProvider provider;
    WriterProbe writer;
    writer.released = true;
    SkinAcceptanceRecorder recorder(
        dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer));
    expect(
        recorder.arm("strict-0123456789", "performance-normal", activation()),
        "strict fixture arms");
    auto actual = facts();
    mutate(actual);
    expect(!recorder.bindSession(actual) &&
               recorder.state() == SkinAcceptanceCaptureState::Failed,
           message);
  };
  runBadBind([](auto &value) { value.chartSha256 = digest('e'); },
             "wrong chart is acceptance-fatal");
  runBadBind([](auto &value) { value.layoutId = "4:3-fit"; },
             "wrong effective layout is acceptance-fatal");
  runBadBind([](auto &value) { value.actualRefreshHz = 0; },
             "zero refresh is acceptance-fatal");
  runBadBind([](auto &value) { value.actualRefreshHz = 241; },
             "refresh above frozen maximum is acceptance-fatal");
  runBadBind([](auto &value) { value.actualRefreshHz = 59; },
             "refresh below the scenario's exact expected rate is acceptance-fatal");
  runBadBind(
      [](auto &value) { value.observedOpaqueGuardVectorSha256 = digest('e'); },
      "wrong guard vector is acceptance-fatal");
  runBadBind([](auto &value) { value.identity.revisionDigest = digest('e'); },
             "wrong activation identity is acceptance-fatal");

  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  SkinAcceptanceRecorder recorder(
      dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer));
  expect(!recorder.arm("../unsafe", "performance-normal", activation()),
         "unsafe opaque filename is rejected");
  auto deps =
      dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer);
  deps.buildIdentity.cleanSource = false;
  SkinAcceptanceRecorder dirty(std::move(deps));
  expect(!dirty.arm("clean-0123456789", "performance-normal", activation()),
         "dirty build identity cannot arm evidence");

  auto invalidRefreshContract =
      dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer);
  invalidRefreshContract.resolveScenario = [](std::string_view scenarioId)
      -> std::optional<SkinAcceptanceScenarioContract> {
    auto value = contract(SkinAcceptanceRunKind::Performance);
    value.expectedRefreshHz = 0;
    return value.scenarioId == scenarioId ? std::optional(value)
                                          : std::nullopt;
  };
  SkinAcceptanceRecorder invalidRefresh(std::move(invalidRefreshContract));
  expect(!invalidRefresh.arm("invalid-hz-01234567", "performance-normal",
                             activation()),
         "a scenario cannot freeze a refresh rate outside 1 through 240 Hz");

  auto excessiveRefreshContract =
      dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer);
  excessiveRefreshContract.resolveScenario = [](std::string_view scenarioId)
      -> std::optional<SkinAcceptanceScenarioContract> {
    auto value = contract(SkinAcceptanceRunKind::Performance);
    value.expectedRefreshHz = 241;
    return value.scenarioId == scenarioId ? std::optional(value)
                                          : std::nullopt;
  };
  SkinAcceptanceRecorder excessiveRefresh(
      std::move(excessiveRefreshContract));
  expect(!excessiveRefresh.arm("excessive-hz-012345", "performance-normal",
                               activation()),
         "a scenario cannot freeze a refresh rate above 240 Hz");

  expect(recorder.arm("clock-0123456789", "performance-normal", activation()) &&
             recorder.bindSession(facts()).has_value(),
         "clock failure fixture binds");
  recorder.record(envelope(1, 5'000'000));
  recorder.record(envelope(2, 4'999'999));
  expect(recorder.state() == SkinAcceptanceCaptureState::Failed,
         "visual clock reversal is fatal");

  FakeOverlayProvider provider2;
  SkinAcceptanceRecorder early(
      dependencies(SkinAcceptanceRunKind::Performance, &provider2, &writer));
  expect(early.arm("early-0123456789", "performance-normal", activation()) &&
             early.bindSession(facts()).has_value(),
         "early end fixture binds");
  early.record(envelope(1, 1'000'000));
  early.sessionEnded(identity());
  expect(early.state() == SkinAcceptanceCaptureState::Failed,
         "session end before exact measurement is fatal");
}

void testExactRefreshContractAcceptsInclusiveBoundaries() {
  for (const std::uint32_t refreshHz : {1U, 240U}) {
    FakeOverlayProvider provider;
    WriterProbe writer;
    writer.released = true;
    auto recorderDependencies =
        dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer);
    recorderDependencies.resolveScenario = [refreshHz](std::string_view scenarioId)
        -> std::optional<SkinAcceptanceScenarioContract> {
      auto value = contract(SkinAcceptanceRunKind::Performance);
      value.expectedRefreshHz = refreshHz;
      return value.scenarioId == scenarioId ? std::optional(value)
                                            : std::nullopt;
    };
    SkinAcceptanceRecorder recorder(std::move(recorderDependencies));
    auto sessionFacts = facts();
    sessionFacts.actualRefreshHz = refreshHz;
    expect(recorder.arm("refresh-boundary-" + std::to_string(refreshHz),
                        "performance-normal", activation()) &&
               recorder.bindSession(sessionFacts).has_value(),
           "inclusive refresh boundary arms and binds exactly");
    completePerformanceRun(recorder);
    const auto ticket = recorder.currentExportTicket();
    const auto result = ticket ? waitForExport(recorder, *ticket)
                               : SkinAcceptanceExportPollResult{};
    expect(result.result && result.result->exported,
           "inclusive refresh boundary produces accepted evidence");
  }
}

void testDiagnosticHistorySnapshotCapacityBoundary() {
  {
    FakeOverlayProvider provider;
    WriterProbe writer;
    writer.released = true;
    auto recorderDependencies =
        dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer);
    recorderDependencies.snapshotDiagnostics = [] {
      std::vector<SkinDiagnostic> diagnostics(
          255, SkinDiagnostic{.code = "skin_file_render_phase_denied"});
      diagnostics.push_back(
          SkinDiagnostic{.code = "skin_lua_event_execution_failed"});
      return diagnostics;
    };
    SkinAcceptanceRecorder recorder(std::move(recorderDependencies));
    expect(recorder.arm("history-limit-0123456", "performance-normal",
                        activation()) &&
               recorder.bindSession(facts()).has_value(),
           "diagnostic-history capacity fixture binds");
    completePerformanceRun(recorder);
    const auto ticket = recorder.currentExportTicket();
    const auto result = ticket ? waitForExport(recorder, *ticket)
                               : SkinAcceptanceExportPollResult{};
    const auto lastRepeated =
        writer.payload.rfind("skin_file_render_phase_denied");
    const auto finalDistinct =
        writer.payload.find("skin_lua_event_execution_failed");
    expect(result.result && result.result->exported &&
               countOccurrences(writer.payload,
                                "skin_file_render_phase_denied") == 255 &&
               countOccurrences(writer.payload,
                                "skin_lua_event_execution_failed") == 1 &&
               lastRepeated != std::string::npos &&
               finalDistinct != std::string::npos &&
               lastRepeated < finalDistinct,
           "exactly 256 allowed diagnostic codes export in source chronology");
  }

  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  auto recorderDependencies =
      dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer);
  recorderDependencies.snapshotDiagnostics = [] {
    return std::vector<SkinDiagnostic>(
        257, SkinDiagnostic{.code = "skin_runtime_callback_failed"});
  };
  SkinAcceptanceRecorder recorder(std::move(recorderDependencies));
  expect(recorder.arm("history-overflow-0123", "performance-normal",
                      activation()) &&
             recorder.bindSession(facts()).has_value(),
         "diagnostic-history overflow fixture binds");
  completePerformanceRun(recorder);
  const auto ticket = recorder.currentExportTicket();
  const auto result = ticket ? recorder.pollExport(*ticket)
                             : SkinAcceptanceExportPollResult{};
  expect(ticket && recorder.state() == SkinAcceptanceCaptureState::Failed &&
             result.state == SkinAcceptanceExportPollState::Ready &&
             result.result && !result.result->exported && writer.writes == 0,
         "more than 256 diagnostic-history values fail before worker export");
}

void testPostTicketTerminalConstructionExceptionPublishesRecoverableFailure() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  auto recorderDependencies =
      dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer);
  recorderDependencies.writeAtomic =
      ThrowOnCopyAndNextAllocationWriter(writer);
  SkinAcceptanceRecorder recorder(std::move(recorderDependencies));
  expect(recorder.arm("prepare-throw-012345", "performance-normal",
                      activation()) &&
             recorder.bindSession(facts()).has_value(),
         "post-ticket terminal-construction failure fixture binds");
  completePerformanceRun(recorder);
  const auto ticket = recorder.currentExportTicket();
  expect(rejectNextAllocation.exchange(false, std::memory_order_relaxed),
         "terminal failure publication performs no allocation after the "
         "ticket becomes observable");
  const auto result = ticket ? recorder.pollExport(*ticket)
                             : SkinAcceptanceExportPollResult{};
  expect(ticket && result.state == SkinAcceptanceExportPollState::Ready &&
             result.result && !result.result->exported &&
             result.result->failure.has_value() && writer.writes == 0,
         "a post-ticket allocation failure still publishes one ready failure");
  expect(ticket && recorder.acknowledgeExport(*ticket) &&
             recorder.state() == SkinAcceptanceCaptureState::Idle &&
             recorder.arm("prepare-rearm-012345", "performance-normal",
                          activation()) &&
             recorder.bindSession(facts()).has_value(),
         "a terminal-publication failure can be acknowledged before a fresh "
         "arm");
}

void testPerformanceUsesActualContiguousBoundaryFrames() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;

  SkinAcceptanceRecorder lateStart(
      dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer));
  expect(
      lateStart.arm("late-start-012345", "performance-normal", activation()) &&
          lateStart.bindSession(facts()).has_value(),
      "late-start fixture binds");
  lateStart.record(envelope(1, 1'000'000));
  lateStart.record(envelope(2, 31'000'001));
  expect(lateStart.state() == SkinAcceptanceCaptureState::Failed,
         "the first retained frame cannot synthesize the warmup endpoint");

  FakeOverlayProvider provider2;
  SkinAcceptanceRecorder lateEnd(
      dependencies(SkinAcceptanceRunKind::Performance, &provider2, &writer));
  expect(lateEnd.arm("late-end-01234567", "performance-normal", activation()) &&
             lateEnd.bindSession(facts()).has_value(),
         "late-end fixture binds");
  lateEnd.record(envelope(1, 1'000'000));
  lateEnd.record(envelope(2, 31'000'000));
  lateEnd.record(envelope(3, 210'999'999));
  lateEnd.record(envelope(4, 211'000'001));
  expect(lateEnd.state() == SkinAcceptanceCaptureState::Failed &&
             !lateEnd.currentExportTicket(),
         "the last retained frame cannot synthesize the measurement endpoint");

  FakeOverlayProvider provider3;
  SkinAcceptanceRecorder frameGap(
      dependencies(SkinAcceptanceRunKind::Performance, &provider3, &writer));
  expect(
      frameGap.arm("frame-gap-0123456", "performance-normal", activation()) &&
          frameGap.bindSession(facts()).has_value(),
      "frame-gap fixture binds");
  frameGap.record(envelope(8, 1'000'000));
  frameGap.record(envelope(10, 31'000'000));
  expect(frameGap.state() == SkinAcceptanceCaptureState::Failed,
         "a missing frame serial breaks contiguous evidence");
}

void testTimestampArithmeticOverflowFailsClosed() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  SkinAcceptanceRecorder recorder(
      dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer));
  expect(recorder.arm("timestamp-overflow-01", "performance-normal",
                      activation()) &&
             recorder.bindSession(facts()).has_value(),
         "timestamp-overflow fixture binds");
  recorder.record(envelope(1, std::numeric_limits<std::int64_t>::max() - 10));
  expect(recorder.state() == SkinAcceptanceCaptureState::Failed,
         "warmup and recording timestamp addition overflow fails closed");
}

void testCapacityIncompleteAndMismatchAreFatal() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  SkinAcceptanceRecorder recorder(
      dependencies(SkinAcceptanceRunKind::Performance, &provider, &writer));
  expect(
      recorder.arm("overflow-0123456789", "performance-normal", activation()) &&
          recorder.bindSession(facts()).has_value(),
      "overflow fixture binds");
  recorder.record(envelope(1, 1));
  for (std::uint64_t index = 0; index <= SkinPerformanceTelemetry::maxSamples;
       ++index) {
    recorder.record(
        envelope(index + 2, 30'000'001 + static_cast<std::int64_t>(index)));
  }
  expect(recorder.state() == SkinAcceptanceCaptureState::Failed,
         "sample 65,537 is fatal instead of evicting");

  FakeOverlayProvider provider2;
  SkinAcceptanceRecorder incomplete(
      dependencies(SkinAcceptanceRunKind::Performance, &provider2, &writer));
  expect(incomplete.arm("partial-0123456789", "performance-normal",
                        activation()) &&
             incomplete.bindSession(facts()).has_value(),
         "partial fixture binds");
  auto partial = envelope(1, 1);
  partial.contributions &=
      ~static_cast<std::uint32_t>(SkinTelemetryContribution::MainLoop);
  incomplete.record(std::move(partial));
  expect(incomplete.state() == SkinAcceptanceCaptureState::Failed,
         "incomplete contribution envelope is fatal");

  FakeOverlayProvider provider3;
  SkinAcceptanceRecorder mismatch(
      dependencies(SkinAcceptanceRunKind::Performance, &provider3, &writer));
  expect(
      mismatch.arm("mismatch-0123456789", "performance-normal", activation()) &&
          mismatch.bindSession(facts()).has_value(),
      "mismatch fixture binds");
  mismatch.record(envelope(1, 1, {}, false, 42));
  expect(mismatch.state() == SkinAcceptanceCaptureState::Failed,
         "wrong session envelope is fatal");
}

void testLifecycleRequiresBaselineAndTenSequentialDestroyedSessions() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  SkinAcceptanceRecorder recorder(dependencies(
      SkinAcceptanceRunKind::ResourceLifecycle, &provider, &writer));
  expect(
      recorder.arm("lifecycle-0123456789", "resource-lifecycle", activation()),
      "lifecycle fixture arms");
  recorder.recordResourceLifecycle(
      {.phase = SkinResourceLifecyclePhase::BeforeFirstEntry,
       .cycleIndex = 0,
       .liveTextures = 7,
       .liveResources = 9,
       .residentBytes = 10'000});
  for (std::uint32_t cycle = 1; cycle <= 10; ++cycle) {
    const std::uint64_t serial = 100 + cycle;
    expect(recorder.bindSession(facts(serial)).has_value(),
           "each lifecycle session binds with a fresh serial");
    recorder.sessionEnded(identity(serial));
    recorder.recordResourceLifecycle(
        {.phase = SkinResourceLifecyclePhase::AfterExit,
         .cycleIndex = cycle,
         .liveTextures = 7,
         .liveResources = 9,
         .residentBytes = 10'000 + cycle});
    recorder.sessionTeardownComplete(identity(serial));
  }
  const auto ticket = recorder.currentExportTicket();
  expect(ticket.has_value() &&
             recorder.state() == SkinAcceptanceCaptureState::Exporting,
         "the tenth explicit post-destruction sample triggers export");
  const auto result = waitForExport(recorder, *ticket);
  expect(
      result.result && result.result->exported &&
          writer.payload.find("\"cycle\":10") != std::string::npos &&
          writer.payload.find("\"residentBytes\":10000") != std::string::npos &&
          writer.payload.find("\"residentBytes\":10010") != std::string::npos,
      "lifecycle export contains exactly the ten numbered samples");
}

void testLifecycleOmitsUnknownResidentBytes() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  SkinAcceptanceRecorder recorder(dependencies(
      SkinAcceptanceRunKind::ResourceLifecycle, &provider, &writer));
  expect(recorder.arm("lifecycle-unknown-0123456789", "resource-lifecycle",
                      activation()),
         "unknown-residency lifecycle fixture arms");
  recorder.recordResourceLifecycle(
      {.phase = SkinResourceLifecyclePhase::BeforeFirstEntry,
       .cycleIndex = 0,
       .liveTextures = 7,
       .liveResources = 9});
  for (std::uint32_t cycle = 1; cycle <= 10; ++cycle) {
    const std::uint64_t serial = 200 + cycle;
    expect(recorder.bindSession(facts(serial)).has_value(),
           "each unknown-residency lifecycle session binds");
    recorder.sessionEnded(identity(serial));
    recorder.recordResourceLifecycle(
        {.phase = SkinResourceLifecyclePhase::AfterExit,
         .cycleIndex = cycle,
         .liveTextures = 7,
         .liveResources = 9});
    recorder.sessionTeardownComplete(identity(serial));
  }
  const auto ticket = recorder.currentExportTicket();
  expect(ticket.has_value(), "unknown-residency lifecycle export starts");
  const auto result = waitForExport(recorder, *ticket);
  expect(result.result && result.result->exported &&
             writer.payload.find("\"residentBytes\"") == std::string::npos,
         "unknown process residency is omitted from lifecycle export");
}

void testNegativeOverlayOrderingCountersAndProviderShutdown() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  SkinAcceptanceRecorder recorder(dependencies(
      SkinAcceptanceRunKind::RenderIoNegative, &provider, &writer));
  expect(
      recorder.arm("negative-0123456789", "render-io-negative", activation()) &&
          provider.beginCalls == 1,
      "negative arm queues the before-overlay digest");
  const auto before = provider.latest();
  expect(!recorder.bindSession(facts()),
         "binding cannot precede memory-only before-digest completion");
  const auto pollsBefore = provider.pollCalls;
  recorder.pollAsyncDependencies();
  recorder.pollAsyncDependencies();
  expect(provider.beginCalls == 1 && provider.pollCalls == pollsBefore + 2,
         "polling consumes provider memory without starting digest work");
  provider.ready(before, digest('e'));
  recorder.pollAsyncDependencies();
  const auto boundScope = recorder.bindSession(facts());
  expect(boundScope.has_value() &&
             recorder.state() == SkinAcceptanceCaptureState::Recording,
         "ready before digest permits the matching real session");
  SkinRenderIoCounters counters;
  counters.filesystemReadsDenied = 1;
  recorder.record(envelope(1, 1, counters, true));
  recordExpectedNegativeEvidence(recorder, *boundScope);
  recorder.sessionEnded(identity());
  expect(provider.beginCalls == 1,
         "session end alone cannot queue the after digest");
  recorder.sessionTeardownComplete(identity());
  expect(provider.beginCalls == 2,
         "after digest is armed only after matching teardown completion");
  const auto after = provider.latest();
  provider.ready(after, digest('e'));
  recorder.pollAsyncDependencies();
  expect(provider.cancelled ==
             std::vector<std::uint64_t>{before.value, after.value},
         "recorder releases each copied terminal provider result");
  const auto ticket = recorder.currentExportTicket();
  expect(ticket.has_value(), "equal overlay digests trigger negative export");
  const auto result = waitForExport(recorder, *ticket);
  expect(result.result && result.result->exported &&
             writer.payload.find("skin_file_render_phase_denied") !=
                 std::string::npos &&
             writer.payload.find(
                 "discard_frame_disable_session_same_frame_builtin") !=
                 std::string::npos &&
             writer.payload.find("\"filesystemReads\":1") !=
                 std::string::npos &&
             writer.payload.find("\"overlayDigestBefore\":\"" + digest('e') +
                                 "\"") != std::string::npos,
         "negative export proves exact diagnostic/action/denial/equal digests");

  FakeOverlayProvider pendingProvider;
  {
    SkinAcceptanceRecorder pending(dependencies(
        SkinAcceptanceRunKind::RenderIoNegative, &pendingProvider, &writer));
    expect(
        pending.arm("shutdown-0123456789", "render-io-negative", activation()),
        "shutdown fixture arms pending digest");
    const auto pendingTicket = pendingProvider.latest();
    pending.shutdown();
    expect(pendingProvider.cancelled.size() == 1 &&
               pendingProvider.cancelled.front() == pendingTicket.value &&
               pendingProvider.shutdownCalls == 0,
           "recorder cancels its ticket but does not outlive-own the provider");
  }
  pendingProvider.shutdown();
  expect(pendingProvider.drained && pendingProvider.shutdownCalls == 1,
         "the longer-lived provider drains independently and idempotently");
  pendingProvider.shutdown();
  expect(pendingProvider.shutdownCalls == 1 && pendingProvider.drained,
         "provider shutdown is idempotent after its first drain");
}

void testObservedNegativeEvidenceIsScopedAndCompared() {
  auto readyNegative = [](SkinAcceptanceRecorder &recorder,
                          FakeOverlayProvider &provider,
                          std::string opaqueRunId)
      -> std::optional<SkinAcceptanceBoundScope> {
    expect(recorder.arm(std::move(opaqueRunId), "render-io-negative",
                        activation()),
           "negative evidence fixture arms");
    provider.ready(provider.latest(), digest('e'));
    recorder.pollAsyncDependencies();
    auto boundScope = recorder.bindSession(facts());
    expect(boundScope.has_value(),
           "negative evidence fixture binds after the before digest");
    SkinRenderIoCounters counters;
    counters.filesystemReadsDenied = 1;
    recorder.record(envelope(1, 1, counters, true));
    return boundScope;
  };

  WriterProbe writer;
  writer.released = true;
  FakeOverlayProvider sourceProvider;
  SkinAcceptanceRecorder source(dependencies(
      SkinAcceptanceRunKind::RenderIoNegative, &sourceProvider, &writer));
  const auto sourceScope =
      readyNegative(source, sourceProvider, "shared-scope-012345");

  FakeOverlayProvider foreignProvider;
  SkinAcceptanceRecorder foreign(dependencies(
      SkinAcceptanceRunKind::RenderIoNegative, &foreignProvider, &writer));
  const auto foreignScope =
      readyNegative(foreign, foreignProvider, "shared-scope-012345");
  expect(sourceScope && foreignScope,
         "two recorders issue scopes for identical run and session facts");
  foreign.recordDiagnosticEvidence(
      *sourceScope,
      SkinDiagnostic{.code = "skin_file_render_phase_denied"});
  expect(foreign.state() == SkinAcceptanceCaptureState::Failed,
         "a valid scope from another recorder is nonforgeable and rejected");

  FakeOverlayProvider staleProvider;
  SkinAcceptanceRecorder stale(dependencies(
      SkinAcceptanceRunKind::RenderIoNegative, &staleProvider, &writer));
  const auto staleScope =
      readyNegative(stale, staleProvider, "stale-scope-0123456");
  stale.sessionEnded(identity());
  stale.recordFallbackAction(
      *staleScope, "discard_frame_disable_session_same_frame_builtin");
  expect(stale.state() == SkinAcceptanceCaptureState::Failed,
         "a bound scope is rejected after its exact session has ended");

  FakeOverlayProvider wrongDiagnosticCodeProvider;
  SkinAcceptanceRecorder wrongDiagnosticCode(
      dependencies(SkinAcceptanceRunKind::RenderIoNegative,
                   &wrongDiagnosticCodeProvider, &writer));
  const auto wrongDiagnosticCodeScope =
      readyNegative(wrongDiagnosticCode, wrongDiagnosticCodeProvider,
                    "diag-code-0123456789");
  wrongDiagnosticCode.recordDiagnosticEvidence(
      *wrongDiagnosticCodeScope,
      SkinDiagnostic{.code = "different_observed_code"});
  wrongDiagnosticCode.recordFallbackAction(
      *wrongDiagnosticCodeScope,
      "discard_frame_disable_session_same_frame_builtin");
  wrongDiagnosticCode.sessionEnded(identity());
  wrongDiagnosticCode.sessionTeardownComplete(identity());
  wrongDiagnosticCodeProvider.ready(wrongDiagnosticCodeProvider.latest(),
                                    digest('e'));
  wrongDiagnosticCode.pollAsyncDependencies();
  expect(wrongDiagnosticCode.state() == SkinAcceptanceCaptureState::Failed,
         "the observed diagnostic code must exactly match the contract");

  FakeOverlayProvider wrongActionProvider;
  SkinAcceptanceRecorder wrongAction(dependencies(
      SkinAcceptanceRunKind::RenderIoNegative, &wrongActionProvider, &writer));
  const auto wrongActionScope =
      readyNegative(wrongAction, wrongActionProvider, "action-0123456789");
  wrongAction.recordDiagnosticEvidence(
      *wrongActionScope,
      SkinDiagnostic{.code = "skin_file_render_phase_denied"});
  wrongAction.recordFallbackAction(
      *wrongActionScope, "different_observed_fallback_action");
  wrongAction.sessionEnded(identity());
  wrongAction.sessionTeardownComplete(identity());
  wrongActionProvider.ready(wrongActionProvider.latest(), digest('e'));
  wrongAction.pollAsyncDependencies();
  expect(wrongAction.state() == SkinAcceptanceCaptureState::Failed,
         "the observed fallback action must exactly match the contract");
}

void testAcknowledgedRunInvalidatesItsBoundScopeGeneration() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  SkinAcceptanceRecorder recorder(dependencies(
      SkinAcceptanceRunKind::RenderIoNegative, &provider, &writer));

  expect(recorder.arm("scope-generation-0123", "render-io-negative",
                      activation()),
         "first scope-generation run arms");
  provider.ready(provider.latest(), digest('e'));
  recorder.pollAsyncDependencies();
  const auto oldScope = recorder.bindSession(facts());
  expect(oldScope.has_value(), "first run issues a bound scope");
  SkinRenderIoCounters counters;
  counters.filesystemReadsDenied = 1;
  recorder.record(envelope(1, 1, counters, true));
  recordExpectedNegativeEvidence(recorder, *oldScope);
  recorder.sessionEnded(identity());
  recorder.sessionTeardownComplete(identity());
  provider.ready(provider.latest(), digest('e'));
  recorder.pollAsyncDependencies();
  const auto ticket = recorder.currentExportTicket();
  const auto exported = ticket ? waitForExport(recorder, *ticket)
                               : SkinAcceptanceExportPollResult{};
  expect(ticket && exported.result && exported.result->exported &&
             recorder.acknowledgeExport(*ticket),
         "first scope-generation run exports and acknowledges");

  expect(recorder.arm("scope-generation-0123", "render-io-negative",
                      activation()),
         "the same recorder and record ID can arm a later run");
  provider.ready(provider.latest(), digest('e'));
  recorder.pollAsyncDependencies();
  const auto newScope = recorder.bindSession(facts());
  expect(newScope.has_value(),
         "later run binds the same five-field session facts");
  recorder.recordDiagnosticEvidence(
      *oldScope, SkinDiagnostic{.code = "skin_file_render_phase_denied"});
  expect(recorder.state() == SkinAcceptanceCaptureState::Failed,
         "an acknowledged run's scope is stale across identical re-arm facts");
}

void testNegativeFramesAfterSessionEndFailClosed() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  SkinAcceptanceRecorder recorder(dependencies(
      SkinAcceptanceRunKind::RenderIoNegative, &provider, &writer));
  expect(
      recorder.arm("post-end-0123456789", "render-io-negative", activation()),
      "post-end fixture arms");
  provider.ready(provider.latest(), digest('e'));
  recorder.pollAsyncDependencies();
  expect(recorder.bindSession(facts()).has_value(), "post-end fixture binds");
  recorder.sessionEnded(identity());
  SkinRenderIoCounters counters;
  counters.filesystemReadsDenied = 1;
  recorder.record(envelope(1, 1, counters, true));
  expect(recorder.state() == SkinAcceptanceCaptureState::Failed,
         "every negative-evidence frame after sessionEnded is rejected");

  FakeOverlayProvider afterTeardownProvider;
  SkinAcceptanceRecorder afterTeardown(
      dependencies(SkinAcceptanceRunKind::RenderIoNegative,
                   &afterTeardownProvider, &writer));
  expect(afterTeardown.arm("post-teardown-012345", "render-io-negative",
                           activation()),
         "post-teardown fixture arms");
  afterTeardownProvider.ready(afterTeardownProvider.latest(), digest('e'));
  afterTeardown.pollAsyncDependencies();
  expect(afterTeardown.bindSession(facts()).has_value(),
         "post-teardown fixture binds");
  afterTeardown.record(envelope(1, 1, counters, true));
  afterTeardown.sessionEnded(identity());
  afterTeardown.sessionTeardownComplete(identity());
  afterTeardown.record(envelope(2, 2, counters, true));
  expect(afterTeardown.state() == SkinAcceptanceCaptureState::Failed,
         "a negative frame after teardown cannot bypass the ended-session guard");
}

void testDigestFailureAndMismatchBecomeTerminalFailures() {
  FakeOverlayProvider provider;
  WriterProbe writer;
  writer.released = true;
  SkinAcceptanceRecorder failed(dependencies(
      SkinAcceptanceRunKind::RenderIoNegative, &provider, &writer));
  expect(
      failed.arm("digestfail-0123456789", "render-io-negative", activation()),
      "failure fixture arms");
  provider.fail(provider.latest());
  failed.pollAsyncDependencies();
  expect(failed.state() == SkinAcceptanceCaptureState::Failed &&
             !failed.bindSession(facts()),
         "failed before digest permanently rejects binding");
  const auto failureTicket = failed.beginStopAndExport();
  const auto failure = failed.pollExport(failureTicket);
  expect(failure.state == SkinAcceptanceExportPollState::Ready &&
             failure.result && !failure.result->exported &&
             failure.result->failure.has_value(),
         "fatal evidence resolves as one sanitized terminal failure");

  FakeOverlayProvider mismatchProvider;
  SkinAcceptanceRecorder mismatch(dependencies(
      SkinAcceptanceRunKind::RenderIoNegative, &mismatchProvider, &writer));
  expect(
      mismatch.arm("digestdiff-0123456789", "render-io-negative", activation()),
      "mismatch fixture arms");
  mismatchProvider.ready(mismatchProvider.latest(), digest('e'));
  mismatch.pollAsyncDependencies();
  const auto mismatchScope = mismatch.bindSession(facts());
  expect(mismatchScope.has_value(), "mismatch fixture binds");
  SkinRenderIoCounters counters;
  counters.filesystemReadsDenied = 1;
  mismatch.record(envelope(1, 1, counters, true));
  recordExpectedNegativeEvidence(mismatch, *mismatchScope);
  mismatch.sessionEnded(identity());
  mismatch.sessionTeardownComplete(identity());
  mismatchProvider.ready(mismatchProvider.latest(), digest('f'));
  mismatch.pollAsyncDependencies();
  expect(mismatch.state() == SkinAcceptanceCaptureState::Failed,
         "different before/after overlay digests are fatal");
}

} // namespace

int main() {
  testPerformanceStateMachineWorkerTicketAndSanitizedPayload();
  testDiagnosticHistoryRejectsInternalNamespaceLookalikes();
  testStrictBindingAndPerformanceFailures();
  testExactRefreshContractAcceptsInclusiveBoundaries();
  testDiagnosticHistorySnapshotCapacityBoundary();
  testPostTicketTerminalConstructionExceptionPublishesRecoverableFailure();
  testPerformanceUsesActualContiguousBoundaryFrames();
  testTimestampArithmeticOverflowFailsClosed();
  testCapacityIncompleteAndMismatchAreFatal();
  testLifecycleRequiresBaselineAndTenSequentialDestroyedSessions();
  testLifecycleOmitsUnknownResidentBytes();
  testNegativeOverlayOrderingCountersAndProviderShutdown();
  testObservedNegativeEvidenceIsScopedAndCompared();
  testAcknowledgedRunInvalidatesItsBoundScopeGeneration();
  testNegativeFramesAfterSessionEndFailClosed();
  testDigestFailureAndMismatchBecomeTerminalFailures();
  return failures == 0 ? 0 : 1;
}
