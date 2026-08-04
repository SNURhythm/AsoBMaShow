#include "SkinPerformanceTelemetry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace skin {
namespace {

constexpr std::int64_t kWarmupMicros = 30'000'000;
constexpr std::int64_t kRecordingMicros = 180'000'000;
constexpr std::int64_t kMaximumResidentDriftBytes = 32LL * 1024 * 1024;

bool validIdentity(const PlaySkinSessionIdentity &value) noexcept {
  return value.sessionSerial != 0 && !value.profileId.opaque.empty() &&
         !value.entry.package.directoryName.empty() &&
         !value.entry.package.collisionKey.empty() &&
         !value.entry.packageRelativePath.empty() &&
         !value.entry.collisionKey.empty() && !value.revisionDigest.empty() &&
         !value.configurationDigest.empty();
}

bool validContributionPrefix(std::uint32_t contributions) noexcept {
  return (contributions & ~kCompleteSkinTelemetryContributions) == 0 &&
         (contributions & (contributions + 1U)) == 0;
}

bool sameIdentity(const PlaySkinSessionIdentity &left,
                  const PlaySkinSessionIdentity &right) noexcept {
  return left.sessionSerial == right.sessionSerial &&
         left.profileId == right.profileId && left.entry == right.entry &&
         left.revisionDigest == right.revisionDigest &&
         left.configurationDigest == right.configurationDigest;
}

std::uint64_t saturatingAdd(std::uint64_t left,
                            std::uint64_t right) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

std::int64_t signedDifference(std::uint64_t last,
                              std::uint64_t first) noexcept {
  constexpr auto maximum =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (last >= first) {
    const auto difference = last - first;
    return difference > maximum ? std::numeric_limits<std::int64_t>::max()
                                : static_cast<std::int64_t>(difference);
  }
  const auto difference = first - last;
  return difference > maximum ? std::numeric_limits<std::int64_t>::min()
                              : -static_cast<std::int64_t>(difference);
}

void addCounters(SkinRenderIoCounters &total,
                 const SkinRenderIoCounters &sample) noexcept {
  total.filesystemReadsPerformed = saturatingAdd(
      total.filesystemReadsPerformed, sample.filesystemReadsPerformed);
  total.filesystemReadsDenied = saturatingAdd(total.filesystemReadsDenied,
                                              sample.filesystemReadsDenied);
  total.filesystemWritesPerformed = saturatingAdd(
      total.filesystemWritesPerformed, sample.filesystemWritesPerformed);
  total.filesystemWritesDenied = saturatingAdd(total.filesystemWritesDenied,
                                               sample.filesystemWritesDenied);
  total.filesystemDirectoryScansPerformed =
      saturatingAdd(total.filesystemDirectoryScansPerformed,
                    sample.filesystemDirectoryScansPerformed);
  total.filesystemDirectoryScansDenied =
      saturatingAdd(total.filesystemDirectoryScansDenied,
                    sample.filesystemDirectoryScansDenied);
  total.resourceUploadsPerformed = saturatingAdd(
      total.resourceUploadsPerformed, sample.resourceUploadsPerformed);
  total.resourceUploadsDenied = saturatingAdd(
      total.resourceUploadsDenied, sample.resourceUploadsDenied);
}

bool allIoCountersZero(const SkinRenderIoCounters &value) noexcept {
  return value.filesystemReadsPerformed == 0 &&
         value.filesystemReadsDenied == 0 &&
         value.filesystemWritesPerformed == 0 &&
         value.filesystemWritesDenied == 0 &&
         value.filesystemDirectoryScansPerformed == 0 &&
         value.filesystemDirectoryScansDenied == 0 &&
         value.resourceUploadsPerformed == 0 &&
         value.resourceUploadsDenied == 0;
}

std::uint64_t deniedCount(const SkinRenderIoCounters &value,
                          SkinRenderIoOperation operation) noexcept {
  switch (operation) {
  case SkinRenderIoOperation::FilesystemRead:
    return value.filesystemReadsDenied;
  case SkinRenderIoOperation::FilesystemWrite:
    return value.filesystemWritesDenied;
  case SkinRenderIoOperation::FilesystemDirectoryScan:
    return value.filesystemDirectoryScansDenied;
  case SkinRenderIoOperation::ResourceUpload:
    return value.resourceUploadsDenied;
  }
  return 0;
}

bool noPerformedIo(const SkinRenderIoCounters &value) noexcept {
  return value.filesystemReadsPerformed == 0 &&
         value.filesystemWritesPerformed == 0 &&
         value.filesystemDirectoryScansPerformed == 0 &&
         value.resourceUploadsPerformed == 0;
}

bool validIoOperation(SkinRenderIoOperation operation) noexcept {
  switch (operation) {
  case SkinRenderIoOperation::FilesystemRead:
  case SkinRenderIoOperation::FilesystemWrite:
  case SkinRenderIoOperation::FilesystemDirectoryScan:
  case SkinRenderIoOperation::ResourceUpload:
    return true;
  }
  return false;
}

bool onlySelectedDenial(const SkinRenderIoCounters &value,
                        SkinRenderIoOperation selected) noexcept {
  constexpr SkinRenderIoOperation operations[] = {
      SkinRenderIoOperation::FilesystemRead,
      SkinRenderIoOperation::FilesystemWrite,
      SkinRenderIoOperation::FilesystemDirectoryScan,
      SkinRenderIoOperation::ResourceUpload,
  };
  for (const auto operation : operations) {
    const auto count = deniedCount(value, operation);
    if ((operation == selected && count == 0) ||
        (operation != selected && count != 0)) {
      return false;
    }
  }
  return true;
}

} // namespace

SkinTelemetryContributionResult
addSkinTelemetryContribution(SkinFrameTelemetryEnvelope &envelope,
                             SkinTelemetryContribution contribution) noexcept {
  if (!validIdentity(envelope.identity) || envelope.sample.frameSerial == 0) {
    return SkinTelemetryContributionResult::InvalidEnvelope;
  }

  const auto bit = static_cast<std::uint32_t>(contribution);
  if (bit == 0 || (bit & (bit - 1U)) != 0 ||
      (bit & kCompleteSkinTelemetryContributions) == 0) {
    return SkinTelemetryContributionResult::InvalidContribution;
  }
  if (!validContributionPrefix(envelope.contributions)) {
    return SkinTelemetryContributionResult::InvalidEnvelope;
  }
  if ((envelope.contributions & bit) != 0) {
    return SkinTelemetryContributionResult::Duplicate;
  }

  std::uint32_t expectedBit = 1U;
  while ((envelope.contributions & expectedBit) != 0) {
    expectedBit <<= 1U;
  }
  if (bit != expectedBit) {
    return SkinTelemetryContributionResult::OutOfOrder;
  }
  envelope.contributions |= bit;
  return SkinTelemetryContributionResult::Accepted;
}

bool isCompleteSkinTelemetryEnvelope(
    const SkinFrameTelemetryEnvelope &envelope) noexcept {
  return validIdentity(envelope.identity) && envelope.sample.frameSerial != 0 &&
         envelope.contributions == kCompleteSkinTelemetryContributions;
}

SkinTelemetryEnvelopeError validateSkinTelemetryEnvelope(
    const SkinFrameTelemetryEnvelope &envelope,
    const PlaySkinSessionIdentity &expected,
    std::uint64_t previousFrameSerial) noexcept {
  if (!validIdentity(envelope.identity) || !validIdentity(expected)) {
    return SkinTelemetryEnvelopeError::InvalidIdentity;
  }
  if (envelope.sample.frameSerial == 0) {
    return SkinTelemetryEnvelopeError::ZeroFrameSerial;
  }
  if (envelope.contributions != kCompleteSkinTelemetryContributions) {
    return SkinTelemetryEnvelopeError::IncompleteContributions;
  }
  if (!sameIdentity(envelope.identity, expected)) {
    return SkinTelemetryEnvelopeError::UnexpectedIdentity;
  }
  if (envelope.sample.frameSerial <= previousFrameSerial) {
    return SkinTelemetryEnvelopeError::NonIncreasingFrameSerial;
  }
  return SkinTelemetryEnvelopeError::None;
}

SkinPerformanceTelemetry::SkinPerformanceTelemetry(bool enabled) {
  if (!enabled) {
    return;
  }
  samples_ = std::make_unique<SkinFrameTelemetrySample[]>(maxSamples);
  percentileScratch_ = std::make_unique<std::uint64_t[]>(maxSamples);
}

bool SkinPerformanceTelemetry::enabled() const noexcept {
  return samples_ != nullptr;
}

void SkinPerformanceTelemetry::record(
    const SkinFrameTelemetrySample &sample) noexcept {
  if (!enabled()) {
    return;
  }
  receivedSampleCount_ = saturatingAdd(receivedSampleCount_, 1);
  if (retainedSampleCount_ == maxSamples) {
    return;
  }
  samples_[retainedSampleCount_++] = sample;
}

SkinPerformanceSummary SkinPerformanceTelemetry::summarize() const noexcept {
  SkinPerformanceSummary summary;
  if (!enabled()) {
    return summary;
  }

  summary.receivedSampleCount = receivedSampleCount_;
  summary.retainedSampleCount = retainedSampleCount_;
  summary.overflowSampleCount =
      receivedSampleCount_ > retainedSampleCount_
          ? receivedSampleCount_ - retainedSampleCount_
          : 0;
  if (retainedSampleCount_ == 0) {
    return summary;
  }

  const auto &first = samples_[0];
  const auto &last = samples_[retainedSampleCount_ - 1];
  std::uint64_t missedPresentations = 0;
  for (std::size_t index = 0; index < retainedSampleCount_; ++index) {
    const auto &sample = samples_[index];
    percentileScratch_[index] =
        saturatingAdd(sample.evaluationMicros, sample.submissionMicros);
    missedPresentations =
        saturatingAdd(missedPresentations, sample.missedPresentation ? 1 : 0);
    summary.fallbackCount =
        saturatingAdd(summary.fallbackCount, sample.fallback ? 1 : 0);
    summary.peakLuaAllocatorBytes =
        std::max(summary.peakLuaAllocatorBytes, sample.luaAllocatorBytes);
    summary.peakResidentBytes =
        std::max(summary.peakResidentBytes, sample.residentBytes);
    addCounters(summary.renderIo, sample.renderIo);
  }

  const auto nearestRank =
      (99U * static_cast<std::uint64_t>(retainedSampleCount_) + 99U) / 100U;
  const auto percentileIndex = static_cast<std::size_t>(nearestRank - 1U);
  std::nth_element(percentileScratch_.get(),
                   percentileScratch_.get() + percentileIndex,
                   percentileScratch_.get() + retainedSampleCount_);
  summary.p99SkinCpuMicros = percentileScratch_[percentileIndex];
  summary.missedPresentationPercent =
      100.0 * static_cast<double>(missedPresentations) /
      static_cast<double>(retainedSampleCount_);
  summary.residentDriftBytes =
      signedDifference(last.residentBytes, first.residentBytes);
  summary.liveTextureDrift =
      signedDifference(last.liveTextures, first.liveTextures);
  summary.liveResourceDrift =
      signedDifference(last.liveResources, first.liveResources);
  return summary;
}

SkinPerformanceRunValidation validateSkinPerformanceRun(
    const SkinPerformanceSummary &summary,
    const SkinPerformanceRunFacts &facts) noexcept {
  if (facts.configuredRefreshHz < 1 || facts.configuredRefreshHz > 240) {
    return {.failure = SkinPerformanceRunFailure::InvalidRefreshRate};
  }
  const auto maximumClock = std::numeric_limits<std::int64_t>::max();
  if (facts.warmupStartedVisualTimeMicros > maximumClock - kWarmupMicros ||
      facts.trustedFirstRecordedVisualTimeMicros !=
          facts.warmupStartedVisualTimeMicros + kWarmupMicros ||
      facts.trustedFirstRecordedVisualTimeMicros >
          maximumClock - kRecordingMicros ||
      facts.trustedLastRecordedVisualTimeMicros !=
          facts.trustedFirstRecordedVisualTimeMicros + kRecordingMicros) {
    return {.failure =
                SkinPerformanceRunFailure::InvalidTrustedClockWindow};
  }
  if (facts.sessionEndedBeforeRecordingComplete) {
    return {.failure = SkinPerformanceRunFailure::EarlySessionEnd};
  }
  if (!facts.envelopeStreamValid) {
    return {.failure = SkinPerformanceRunFailure::InvalidEnvelopeStream};
  }
  if (summary.retainedSampleCount == 0) {
    return {.failure = SkinPerformanceRunFailure::NoRetainedSamples};
  }
  if (summary.retainedSampleCount > SkinPerformanceTelemetry::maxSamples ||
      summary.receivedSampleCount < summary.retainedSampleCount ||
      summary.receivedSampleCount - summary.retainedSampleCount !=
          summary.overflowSampleCount) {
    return {.failure = SkinPerformanceRunFailure::SampleCountMismatch};
  }
  if (summary.overflowSampleCount != 0) {
    return {.failure = SkinPerformanceRunFailure::SampleOverflow};
  }
  if (!allIoCountersZero(summary.renderIo)) {
    return {.failure = SkinPerformanceRunFailure::RenderIoObserved};
  }

  const auto cpuBudgetComparison =
      static_cast<long double>(summary.p99SkinCpuMicros) *
      static_cast<long double>(facts.configuredRefreshHz) * 10.0L;
  if (cpuBudgetComparison > 9'000'000.0L) {
    return {.failure = SkinPerformanceRunFailure::CpuBudgetExceeded};
  }
  if (!std::isfinite(summary.missedPresentationPercent) ||
      summary.missedPresentationPercent < 0.0 ||
      summary.missedPresentationPercent > 0.5) {
    return {
        .failure = SkinPerformanceRunFailure::MissedPresentationBudgetExceeded};
  }
  if (summary.residentDriftBytes > kMaximumResidentDriftBytes) {
    return {.failure = SkinPerformanceRunFailure::ResidentDriftExceeded};
  }
  return {};
}

bool validateNegativeSkinRenderIo(
    const SkinRenderIoCounters &counters,
    SkinRenderIoOperation selectedDenied) noexcept {
  return validIoOperation(selectedDenied) && noPerformedIo(counters) &&
         onlySelectedDenial(counters, selectedDenied);
}

} // namespace skin
