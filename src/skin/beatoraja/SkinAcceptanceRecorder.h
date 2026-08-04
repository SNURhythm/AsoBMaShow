#pragma once

#include "../../BuildIdentity.h"
#include "SkinOverlayDigestProvider.h"
#include "SkinPerformanceTelemetry.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skin {

struct SkinAcceptanceActivationKey {
  SkinProfileId profileId;
  SkinEntryId entry;
  std::string revisionDigest;
  std::string configurationDigest;
};

enum class SkinAcceptanceRunKind : std::uint8_t {
  Performance,
  ResourceLifecycle,
  RenderIoNegative,
};

struct SkinAcceptanceScenarioContract {
  std::string scenarioId;
  SkinAcceptanceRunKind kind = SkinAcceptanceRunKind::Performance;
  std::string expectedChartSha256;
  std::string expectedLayoutId;
  std::int64_t warmupMicros = 30'000'000;
  std::int64_t measurementMicros = 180'000'000;
  std::uint32_t requiredExitCycles = 10;
  std::uint32_t expectedRefreshHz = 60;
  std::uint32_t maximumRefreshHz = 240;
  std::string expectedOpaqueGuardVectorSha256;
  std::optional<std::string> expectedDiagnosticCode;
  std::optional<std::string> expectedFallbackAction;
  std::optional<SkinRenderIoOperation> expectedDeniedOperation;
};

struct SkinAcceptanceSessionFacts {
  PlaySkinSessionIdentity identity;
  std::string chartSha256;
  std::string layoutId;
  std::uint32_t actualRefreshHz = 0;
  std::string observedOpaqueGuardVectorSha256;
};

class SkinAcceptanceBoundScope final {
public:
  SkinAcceptanceBoundScope(const SkinAcceptanceBoundScope &) = default;
  SkinAcceptanceBoundScope &operator=(const SkinAcceptanceBoundScope &) =
      default;
  SkinAcceptanceBoundScope(SkinAcceptanceBoundScope &&) noexcept = default;
  SkinAcceptanceBoundScope &operator=(SkinAcceptanceBoundScope &&) noexcept =
      default;

private:
  SkinAcceptanceBoundScope(std::uint64_t recorderSerial,
                           std::uint64_t runSerial,
                           std::uint64_t bindingSerial,
                           PlaySkinSessionIdentity identity) noexcept
      : recorderSerial_(recorderSerial), runSerial_(runSerial),
        bindingSerial_(bindingSerial), identity_(std::move(identity)) {}

  std::uint64_t recorderSerial_ = 0;
  std::uint64_t runSerial_ = 0;
  std::uint64_t bindingSerial_ = 0;
  PlaySkinSessionIdentity identity_;

  friend class SkinAcceptanceRecorder;
};

enum class SkinResourceLifecyclePhase : std::uint8_t {
  BeforeFirstEntry,
  AfterExit,
};

struct SkinResourceLifecycleSample {
  SkinResourceLifecyclePhase phase =
      SkinResourceLifecyclePhase::BeforeFirstEntry;
  std::uint32_t cycleIndex = 0;
  std::uint64_t liveTextures = 0;
  std::uint64_t liveResources = 0;
  // Only a platform process-residency probe may populate this. Decoded or
  // uploaded bytes are not an honest substitute for resident process memory.
  std::optional<std::uint64_t> residentBytes;
};

struct SkinAcceptanceScenarioMetadata {
  std::string opaqueRunId;
  std::string scenarioId;
  std::string layoutId;
  std::string chartSha256;
  SkinEntryId entry;
  std::string revisionDigest;
  std::string configurationDigest;
  std::uint32_t configuredRefreshHz = 0;
  std::int64_t warmupMicros = 30'000'000;
  std::int64_t measurementMicros = 180'000'000;
  std::optional<std::string> overlayDigestBefore;
  std::optional<std::string> overlayDigestAfter;
  std::string expectedOpaqueGuardVectorSha256;
  std::string observedOpaqueGuardVectorSha256;
  std::vector<std::string> observedDiagnosticCodes;
  std::optional<std::string> observedFallbackAction;
};

struct SkinAcceptanceExportResult {
  bool exported = false;
  std::filesystem::path documentsRelativePath;
  std::string lowercaseSha256;
  std::optional<SkinDiagnostic> failure;
};

enum class SkinAcceptanceCaptureState : std::uint8_t {
  Idle,
  Armed,
  WarmingUp,
  Recording,
  Exporting,
  Exported,
  Failed,
};

struct SkinAcceptanceExportTicket {
  std::uint64_t value = 0;

  explicit operator bool() const noexcept { return value != 0; }
  auto operator<=>(const SkinAcceptanceExportTicket &) const = default;
};

enum class SkinAcceptanceExportPollState : std::uint8_t {
  Unknown,
  Pending,
  Ready,
};

struct SkinAcceptanceExportPollResult {
  SkinAcceptanceExportPollState state = SkinAcceptanceExportPollState::Unknown;
  std::optional<SkinAcceptanceExportResult> result;
};

struct SkinAcceptanceRecorderDependencies {
  std::filesystem::path documentsRoot;
  SkinBuildIdentity buildIdentity;
  std::function<std::optional<SkinAcceptanceScenarioContract>(std::string_view)>
      resolveScenario;
  // Non-owning. ApplicationContext drains this after recorder shutdown.
  IAsyncSkinOverlayDigestProvider *overlayDigests = nullptr;
  // Called synchronously at export linearization. The recorder retains only a
  // bounded value copy of safe diagnostic codes; messages and paths never
  // cross to the export worker.
  std::function<std::vector<SkinDiagnostic>()> snapshotDiagnostics;
  std::function<bool(const std::filesystem::path &, std::span<const std::byte>,
                     std::string &)>
      writeAtomic;
};

class SkinAcceptanceRecorder final {
public:
  explicit SkinAcceptanceRecorder(SkinAcceptanceRecorderDependencies);
  ~SkinAcceptanceRecorder();

  SkinAcceptanceRecorder(const SkinAcceptanceRecorder &) = delete;
  SkinAcceptanceRecorder &operator=(const SkinAcceptanceRecorder &) = delete;
  SkinAcceptanceRecorder(SkinAcceptanceRecorder &&) = delete;
  SkinAcceptanceRecorder &operator=(SkinAcceptanceRecorder &&) = delete;

  bool arm(std::string opaqueRunId, std::string scenarioId,
           SkinAcceptanceActivationKey);
  [[nodiscard]] std::optional<SkinAcceptanceBoundScope>
  bindSession(const SkinAcceptanceSessionFacts &);
  void record(SkinFrameTelemetryEnvelope &&) noexcept;
  void recordDiagnosticEvidence(const SkinAcceptanceBoundScope &,
                                SkinDiagnostic) noexcept;
  void recordFallbackAction(const SkinAcceptanceBoundScope &,
                            std::string action) noexcept;
  void recordResourceLifecycle(SkinResourceLifecycleSample) noexcept;
  void sessionEnded(const PlaySkinSessionIdentity &) noexcept;
  void sessionTeardownComplete(const PlaySkinSessionIdentity &) noexcept;
  void pollAsyncDependencies() noexcept;

  [[nodiscard]] SkinAcceptanceExportTicket beginStopAndExport();
  [[nodiscard]] SkinAcceptanceExportPollResult
      pollExport(SkinAcceptanceExportTicket) const;
  bool acknowledgeExport(SkinAcceptanceExportTicket) noexcept;
  [[nodiscard]] std::optional<SkinAcceptanceExportTicket>
  currentExportTicket() const noexcept;
  [[nodiscard]] SkinAcceptanceCaptureState state() const noexcept;
  void shutdown() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace skin
