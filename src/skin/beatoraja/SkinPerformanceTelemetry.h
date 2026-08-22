#pragma once

#include "PlaySkinSessionIdentity.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace skin {

enum class SkinLoadingPhase : std::uint8_t {
  Document,
  ResourcePreparation,
  Movie,
  Upload,
};

struct SkinLoadingResourceCounters {
  std::uint64_t filesystemReads = 0;
  std::uint64_t imageDecodes = 0;
  std::uint64_t fontDecodes = 0;
  std::uint64_t movieDecodes = 0;
  std::uint64_t audioDecodes = 0;
  std::uint64_t textureUploads = 0;
  std::uint64_t encodedBytes = 0;
  std::uint64_t decodedBytes = 0;
};

// Path-free, value-owned preparation evidence. Phase bits distinguish a
// measured zero-duration phase from one skipped by cancellation/failure.
struct SkinLoadingTelemetry {
  std::uint64_t documentMicros = 0;
  std::uint64_t resourcePreparationMicros = 0;
  std::uint64_t movieMicros = 0;
  std::uint64_t uploadMicros = 0;
  std::uint64_t totalMicros = 0;
  SkinLoadingResourceCounters resources;
  std::uint8_t completedPhases = 0;
  bool sessionPublished = false;
  bool cancelled = false;
};

[[nodiscard]] bool recordSkinLoadingPhase(SkinLoadingTelemetry &,
                                          SkinLoadingPhase,
                                          std::uint64_t micros) noexcept;
[[nodiscard]] bool
isCompleteSkinLoadingTelemetry(const SkinLoadingTelemetry &) noexcept;

struct SkinRenderIoCounters {
  std::uint64_t filesystemReadsPerformed = 0;
  std::uint64_t filesystemReadsDenied = 0;
  std::uint64_t filesystemWritesPerformed = 0;
  std::uint64_t filesystemWritesDenied = 0;
  std::uint64_t filesystemDirectoryScansPerformed = 0;
  std::uint64_t filesystemDirectoryScansDenied = 0;
  std::uint64_t resourceUploadsPerformed = 0;
  std::uint64_t resourceUploadsDenied = 0;
};

enum class SkinRenderIoOperation : std::uint8_t {
  FilesystemRead,
  FilesystemWrite,
  FilesystemDirectoryScan,
  ResourceUpload,
};

struct SkinFrameTelemetrySample {
  std::uint64_t frameSerial = 0;
  std::int64_t visualTimeMicros = 0;
  // Full frame evaluation duration, including Lua callback execution.
  std::uint64_t evaluationMicros = 0;
  std::uint64_t submissionMicros = 0;
  std::uint64_t luaInstructions = 0;
  // Diagnostic subset of evaluationMicros; never added to total frame CPU.
  std::uint64_t callbackMicros = 0;
  std::uint32_t commandCount = 0;
  std::uint32_t batchCount = 0;
  SkinRenderIoCounters renderIo;
  std::uint64_t liveTextures = 0;
  std::uint64_t liveResources = 0;
  std::uint64_t luaAllocatorBytes = 0;
  std::uint64_t residentBytes = 0;
  bool missedPresentation = false;
  bool fallback = false;
};

enum class SkinTelemetryContribution : std::uint32_t {
  Runtime = 1U << 0U,
  FileSystem = 1U << 1U,
  Resources = 1U << 2U,
  Renderer = 1U << 3U,
  Session = 1U << 4U,
  Coordinator = 1U << 5U,
  MainLoop = 1U << 6U,
};

inline constexpr std::uint32_t kCompleteSkinTelemetryContributions = 0x7fU;

struct SkinFrameTelemetryEnvelope {
  PlaySkinSessionIdentity identity;
  SkinFrameTelemetrySample sample;
  std::uint32_t contributions = 0;
};

enum class SkinTelemetryContributionResult : std::uint8_t {
  Accepted,
  InvalidEnvelope,
  InvalidContribution,
  Duplicate,
  OutOfOrder,
};

[[nodiscard]] SkinTelemetryContributionResult
addSkinTelemetryContribution(SkinFrameTelemetryEnvelope &,
                             SkinTelemetryContribution) noexcept;

[[nodiscard]] bool
isCompleteSkinTelemetryEnvelope(const SkinFrameTelemetryEnvelope &) noexcept;

enum class SkinTelemetryEnvelopeError : std::uint8_t {
  None,
  InvalidIdentity,
  ZeroFrameSerial,
  IncompleteContributions,
  UnexpectedIdentity,
  NonIncreasingFrameSerial,
};

[[nodiscard]] SkinTelemetryEnvelopeError validateSkinTelemetryEnvelope(
    const SkinFrameTelemetryEnvelope &, const PlaySkinSessionIdentity &expected,
    std::uint64_t previousFrameSerial) noexcept;

struct SkinPerformanceSummary {
  std::uint64_t receivedSampleCount = 0;
  std::uint64_t retainedSampleCount = 0;
  std::uint64_t overflowSampleCount = 0;
  std::uint64_t p99SkinCpuMicros = 0;
  double missedPresentationPercent = 0.0;
  std::uint64_t peakLuaAllocatorBytes = 0;
  std::uint64_t peakResidentBytes = 0;
  std::int64_t residentDriftBytes = 0;
  SkinRenderIoCounters renderIo;
  std::int64_t liveTextureDrift = 0;
  std::int64_t liveResourceDrift = 0;
  std::uint64_t fallbackCount = 0;
};

// Disabled by default. Enabling performs the only two allocations up front;
// record() and summarize() then reuse fixed storage and remain allocation-free.
class SkinPerformanceTelemetry {
public:
  static constexpr std::size_t maxSamples = 65'536;

  SkinPerformanceTelemetry() noexcept = default;
  explicit SkinPerformanceTelemetry(bool enabled);
  SkinPerformanceTelemetry(const SkinPerformanceTelemetry &) = delete;
  SkinPerformanceTelemetry &operator=(const SkinPerformanceTelemetry &) =
      delete;
  SkinPerformanceTelemetry(SkinPerformanceTelemetry &&) noexcept = default;
  SkinPerformanceTelemetry &
  operator=(SkinPerformanceTelemetry &&) noexcept = default;

  [[nodiscard]] bool enabled() const noexcept;
  void record(const SkinFrameTelemetrySample &) noexcept;
  [[nodiscard]] SkinPerformanceSummary summarize() const noexcept;

private:
  std::unique_ptr<SkinFrameTelemetrySample[]> samples_;
  mutable std::unique_ptr<std::uint64_t[]> percentileScratch_;
  std::uint64_t receivedSampleCount_ = 0;
  std::size_t retainedSampleCount_ = 0;
};

struct SkinPerformanceRunFacts {
  std::uint16_t configuredRefreshHz = 0;
  std::int64_t warmupStartedVisualTimeMicros = 0;
  std::int64_t trustedFirstRecordedVisualTimeMicros = 0;
  std::int64_t trustedLastRecordedVisualTimeMicros = 0;
  bool sessionEndedBeforeRecordingComplete = false;
  bool envelopeStreamValid = false;
};

enum class SkinPerformanceRunFailure : std::uint8_t {
  None,
  InvalidRefreshRate,
  InvalidTrustedClockWindow,
  EarlySessionEnd,
  InvalidEnvelopeStream,
  NoRetainedSamples,
  SampleCountMismatch,
  SampleOverflow,
  RenderIoObserved,
  CpuBudgetExceeded,
  MissedPresentationBudgetExceeded,
  ResidentDriftExceeded,
};

struct SkinPerformanceRunValidation {
  SkinPerformanceRunFailure failure = SkinPerformanceRunFailure::None;

  [[nodiscard]] bool passed() const noexcept {
    return failure == SkinPerformanceRunFailure::None;
  }
};

[[nodiscard]] SkinPerformanceRunValidation
validateSkinPerformanceRun(const SkinPerformanceSummary &,
                           const SkinPerformanceRunFacts &) noexcept;

[[nodiscard]] bool
validateNegativeSkinRenderIo(const SkinRenderIoCounters &,
                             SkinRenderIoOperation selectedDenied) noexcept;

} // namespace skin
