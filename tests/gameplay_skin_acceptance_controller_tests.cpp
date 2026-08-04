#include "scene/GameplaySkinAcceptanceController.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string digest(char value) { return std::string(64, value); }

SkinAcceptanceActivationKey activation() {
  return {
      .profileId = {.opaque = "acceptance-profile"},
      .entry = {.package = {.directoryName = "acceptance-package",
                            .collisionKey = "acceptance-package"},
                .packageRelativePath = "play7.luaskin",
                .collisionKey = "play7.luaskin"},
      .revisionDigest = digest('a'),
      .configurationDigest = digest('b'),
  };
}

PlaySkinSessionIdentity sessionIdentity() {
  const auto key = activation();
  return {.sessionSerial = 1,
          .profileId = key.profileId,
          .entry = key.entry,
          .revisionDigest = key.revisionDigest,
          .configurationDigest = key.configurationDigest};
}

SkinAcceptanceSessionFacts sessionFacts() {
  return {.identity = sessionIdentity(),
          .chartSha256 = digest('c'),
          .layoutId = "16:9-fit",
          .actualRefreshHz = 60,
          .observedOpaqueGuardVectorSha256 = digest('d')};
}

SkinAcceptanceScenarioContract performanceContract() {
  return {.scenarioId = "performance-normal",
          .kind = SkinAcceptanceRunKind::Performance,
          .expectedChartSha256 = digest('c'),
          .expectedLayoutId = "16:9-fit",
          .expectedOpaqueGuardVectorSha256 = digest('d')};
}

struct WriterProbe {
  bool write(const std::filesystem::path &, std::span<const std::byte>,
             std::string &) {
    std::unique_lock lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait(lock, [this] { return released; });
    return succeeds;
  }

  bool waitUntilEntered() {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(5),
                              [this] { return entered; });
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
};

SkinAcceptanceRecorderDependencies dependencies(WriterProbe &writer) {
  return {
      .documentsRoot = "/sandbox/Documents",
      .buildIdentity = {.commit = std::string(40, '1'),
                        .configuration = "Release",
                        .cleanSource = true},
      .resolveScenario = [](std::string_view scenarioId)
          -> std::optional<SkinAcceptanceScenarioContract> {
        const auto contract = performanceContract();
        return scenarioId == contract.scenarioId ? std::optional(contract)
                                                 : std::nullopt;
      },
      .writeAtomic =
          [&writer](const std::filesystem::path &path,
                    std::span<const std::byte> bytes, std::string &error) {
            return writer.write(path, bytes, error);
          },
  };
}

SkinFrameTelemetryEnvelope envelope(std::uint64_t frameSerial,
                                    std::int64_t visualTimeMicros) {
  SkinFrameTelemetryEnvelope value;
  value.identity = sessionIdentity();
  value.sample.frameSerial = frameSerial;
  value.sample.visualTimeMicros = visualTimeMicros;
  value.sample.evaluationMicros = 50;
  value.sample.submissionMicros = 25;
  value.contributions = kCompleteSkinTelemetryContributions;
  return value;
}

void completePerformanceRun(SkinAcceptanceRecorder &recorder) {
  recorder.record(envelope(1, 1'000'000));
  recorder.record(envelope(2, 31'000'000));
  recorder.record(envelope(3, 211'000'000));
}

std::optional<SkinAcceptanceExportPollResult>
waitForReadyExport(SkinAcceptanceRecorder &recorder,
                   SkinAcceptanceExportTicket ticket) {
  for (int attempt = 0; attempt < 5'000; ++attempt) {
    auto result = recorder.pollExport(ticket);
    if (result.state == SkinAcceptanceExportPollState::Ready) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return std::nullopt;
}

void testSnapshotProjectionKeepsExportStateCoherent() {
  const SkinAcceptanceExportTicket ticket{.value = 41};
  const SkinAcceptanceExportResult exported = {
      .exported = true,
      .documentsRelativePath = "SkinAcceptance/run.json",
      .lowercaseSha256 = digest('e'),
  };
  const SkinAcceptanceExportResult failed = {
      .failure =
          SkinDiagnostic{.code = "export_failed", .message = "Export failed"},
  };

  const auto pending = projectGameplaySkinAcceptanceSnapshot(
      SkinAcceptanceCaptureState::Armed, ticket,
      SkinAcceptanceExportPollResult{
          .state = SkinAcceptanceExportPollState::Pending});
  expect(pending.state == SkinAcceptanceCaptureState::Exporting &&
             pending.exportTicket == ticket && !pending.lastExport &&
             pending.statusMessage == "Acceptance export is pending.",
         "a pending ticket always projects one coherent Exporting snapshot");

  const auto ready = projectGameplaySkinAcceptanceSnapshot(
      SkinAcceptanceCaptureState::Exporting, ticket,
      SkinAcceptanceExportPollResult{
          .state = SkinAcceptanceExportPollState::Ready, .result = exported});
  expect(ready.state == SkinAcceptanceCaptureState::Exported &&
             ready.exportTicket == ticket && ready.lastExport &&
             ready.lastExport->exported &&
             ready.statusMessage == "Acceptance export is ready.",
         "a successful ready result overrides a stale Exporting state");

  const auto failedReady = projectGameplaySkinAcceptanceSnapshot(
      SkinAcceptanceCaptureState::Exporting, ticket,
      SkinAcceptanceExportPollResult{
          .state = SkinAcceptanceExportPollState::Ready, .result = failed});
  expect(failedReady.state == SkinAcceptanceCaptureState::Failed &&
             failedReady.exportTicket == ticket && failedReady.lastExport &&
             !failedReady.lastExport->exported &&
             failedReady.statusMessage ==
                 "Acceptance export completed with failure evidence.",
         "a failed ready result overrides a stale Exporting state");

  const auto unknown = projectGameplaySkinAcceptanceSnapshot(
      SkinAcceptanceCaptureState::Failed, ticket,
      SkinAcceptanceExportPollResult{
          .state = SkinAcceptanceExportPollState::Unknown});
  expect(unknown.state == SkinAcceptanceCaptureState::Failed &&
             unknown.exportTicket == ticket && !unknown.lastExport &&
             unknown.statusMessage ==
                 "Acceptance export ticket is no longer available.",
         "an unknown ticket retains the safely resampled recorder state");
}

void testStartRejectsMissingAndInvalidSelectionWithoutEditableFacts() {
  WriterProbe writer;
  writer.release();
  SkinAcceptanceRecorder recorder(dependencies(writer));
  std::optional<SkinAcceptanceActivationKey> selected;
  GameplaySkinAcceptanceController controller(recorder,
                                              [&selected] { return selected; });

  const auto noSelection =
      controller.start({.opaqueRunId = "missing-selection-012345",
                        .scenarioId = "performance-normal"});
  expect(!noSelection.accepted &&
             controller.snapshot().state == SkinAcceptanceCaptureState::Idle,
         "start rejects a missing selected activation");

  selected = activation();
  selected->revisionDigest = "not-a-digest";
  const auto invalidSelection =
      controller.start({.opaqueRunId = "invalid-selection-012345",
                        .scenarioId = "performance-normal"});
  expect(!invalidSelection.accepted &&
             controller.snapshot().state == SkinAcceptanceCaptureState::Idle,
         "start rejects an invalid four-field selected activation");

  selected = activation();
  const auto started =
      controller.start({.opaqueRunId = "valid-selection-012345",
                        .scenarioId = "performance-normal"});
  expect(started.accepted &&
             controller.snapshot().state == SkinAcceptanceCaptureState::Armed,
         "start arms only with the callback-derived activation");
  expect(!controller
              .start({.opaqueRunId = "repeat-before-stop-012345",
                      .scenarioId = "performance-normal"})
              .accepted,
         "an active acceptance run cannot be replaced");

  controller.close();
  controller.close();
  expect(recorder.state() == SkinAcceptanceCaptureState::Armed,
         "safe Settings close does not cancel an armed recorder");
}

void testCloseReopenRecoversTheSamePendingTicket() {
  WriterProbe writer;
  SkinAcceptanceRecorder recorder(dependencies(writer));
  const CurrentAcceptanceActivation selected = [] {
    return std::optional<SkinAcceptanceActivationKey>(activation());
  };
  GameplaySkinAcceptanceController first(recorder, selected);
  expect(first
             .start({.opaqueRunId = "reopen-pending-012345",
                     .scenarioId = "performance-normal"})
             .accepted,
         "pending reopen fixture arms the recorder");
  expect(recorder.bindSession(sessionFacts()).has_value(),
         "pending reopen fixture binds the real session");
  completePerformanceRun(recorder);
  if (!writer.waitUntilEntered()) {
    expect(false, "pending reopen fixture starts its export worker");
    writer.release();
    return;
  }
  first.poll();
  const auto originalTicket = first.snapshot().exportTicket;
  expect(originalTicket &&
             first.snapshot().state == SkinAcceptanceCaptureState::Exporting &&
             !first.snapshot().lastExport,
         "blocked writer retains one pending export ticket");
  first.close();

  GameplaySkinAcceptanceController reopened(recorder, selected);
  expect(reopened.snapshot().state == SkinAcceptanceCaptureState::Exporting &&
             reopened.snapshot().exportTicket == originalTicket &&
             !reopened.snapshot().lastExport,
         "reopened Settings recovers the exact pending export ticket");
  writer.release();
}

void testCloseReopenRecoversTheSameReadyTicketAndRequiresOneAcknowledgement() {
  WriterProbe writer;
  SkinAcceptanceRecorder recorder(dependencies(writer));
  const CurrentAcceptanceActivation selected = [] {
    return std::optional<SkinAcceptanceActivationKey>(activation());
  };
  GameplaySkinAcceptanceController first(recorder, selected);
  expect(first
             .start({.opaqueRunId = "reopen-ready-012345",
                     .scenarioId = "performance-normal"})
             .accepted,
         "first Settings controller arms the recorder");
  expect(recorder.bindSession(sessionFacts()).has_value(),
         "ready reopen fixture binds the real session");
  completePerformanceRun(recorder);
  if (!writer.waitUntilEntered()) {
    expect(false, "ready reopen fixture starts its export worker");
    writer.release();
    return;
  }
  first.poll();
  const auto originalTicket = first.snapshot().exportTicket;
  expect(originalTicket &&
             first.snapshot().state == SkinAcceptanceCaptureState::Exporting,
         "ready reopen fixture first retains a pending ticket");
  first.close();
  if (!originalTicket) {
    writer.release();
    return;
  }

  writer.release();
  const auto ready = waitForReadyExport(recorder, *originalTicket);
  expect(ready && ready->result && ready->result->exported,
         "ready reopen fixture deterministically completes its export");

  GameplaySkinAcceptanceController reopened(recorder, selected);
  expect(reopened.snapshot().state == SkinAcceptanceCaptureState::Exported &&
             reopened.snapshot().exportTicket == originalTicket &&
             reopened.snapshot().lastExport.has_value() &&
             reopened.snapshot().lastExport->exported,
         "reopened Settings recovers the exact ready terminal ticket");
  expect(!reopened
              .start({.opaqueRunId = "repeat-before-ack-012345",
                      .scenarioId = "performance-normal"})
              .accepted,
         "repeat start is blocked until the retained export is acknowledged");
  expect(reopened.acknowledgeLastExport().accepted &&
             !reopened.snapshot().exportTicket &&
             reopened.snapshot().state == SkinAcceptanceCaptureState::Idle,
         "acknowledgement clears the retained terminal export");
  expect(!reopened.acknowledgeLastExport().accepted,
         "the same terminal export cannot be acknowledged twice");
  expect(reopened
             .start({.opaqueRunId = "repeat-after-ack-012345",
                     .scenarioId = "performance-normal"})
             .accepted,
         "acknowledgement permits a new acceptance run");
}

void testPollDistinguishesNoPendingAndReadyExportTickets() {
  WriterProbe writer;
  SkinAcceptanceRecorder recorder(dependencies(writer));
  const CurrentAcceptanceActivation selected = [] {
    return std::optional<SkinAcceptanceActivationKey>(activation());
  };
  GameplaySkinAcceptanceController controller(recorder, selected);

  controller.poll();
  expect(recorder.pollExport({.value = 999}).state ==
                 SkinAcceptanceExportPollState::Unknown &&
             !controller.snapshot().exportTicket &&
             !controller.snapshot().lastExport &&
             !controller.acknowledgeLastExport().accepted,
         "an unknown export ticket has no acknowledgement action");
  expect(controller
                 .start({.opaqueRunId = "pending-ticket-012345",
                         .scenarioId = "performance-normal"})
                 .accepted &&
             recorder.bindSession(sessionFacts()).has_value(),
         "pending-ticket fixture arms and binds");
  completePerformanceRun(recorder);
  if (!writer.waitUntilEntered()) {
    expect(false, "pending-ticket fixture starts its export worker");
    writer.release();
    return;
  }
  controller.poll();
  expect(
      controller.snapshot().exportTicket && !controller.snapshot().lastExport &&
          controller.snapshot().state == SkinAcceptanceCaptureState::Exporting,
      "poll retains a pending export ticket without inventing a result");

  const auto ticket = controller.snapshot().exportTicket;
  if (!ticket) {
    writer.release();
    return;
  }
  writer.release();
  const auto ready = waitForReadyExport(recorder, *ticket);
  controller.poll();
  expect(ready && controller.snapshot().exportTicket == ticket &&
             controller.snapshot().lastExport.has_value() &&
             controller.snapshot().lastExport->exported &&
             controller.snapshot().state ==
                 SkinAcceptanceCaptureState::Exported,
         "poll exposes one coherent Exported snapshot for its retained ticket");
}

} // namespace

int main() {
  testSnapshotProjectionKeepsExportStateCoherent();
  testStartRejectsMissingAndInvalidSelectionWithoutEditableFacts();
  testCloseReopenRecoversTheSamePendingTicket();
  testCloseReopenRecoversTheSameReadyTicketAndRequiresOneAcknowledgement();
  testPollDistinguishesNoPendingAndReadyExportTickets();
  return failures == 0 ? 0 : 1;
}
