#include "skin/beatoraja/SkinPerformanceTelemetry.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>

namespace allocation_probe {
std::atomic<std::size_t> allocations{0};
}

void *operator new(std::size_t size) {
  allocation_probe::allocations.fetch_add(1, std::memory_order_relaxed);
  if (void *result = std::malloc(size == 0 ? 1 : size)) {
    return result;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
  allocation_probe::allocations.fetch_add(1, std::memory_order_relaxed);
  if (void *result = std::malloc(size == 0 ? 1 : size)) {
    return result;
  }
  throw std::bad_alloc();
}

void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete[](void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept {
  std::free(pointer);
}

namespace {

using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

PlaySkinSessionIdentity identity(std::uint64_t sessionSerial = 17) {
  return {
      .sessionSerial = sessionSerial,
      .profileId = {.opaque = "opaque-profile"},
      .entry = {.package = {.directoryName = "modernchic",
                            .collisionKey = "modernchic"},
                .packageRelativePath = "skin/play7.luaskin",
                .collisionKey = "skin/play7.luaskin"},
      .revisionDigest = "revision-digest",
      .configurationDigest = "configuration-digest",
  };
}

SkinFrameTelemetryEnvelope envelope(std::uint64_t frameSerial = 1) {
  SkinFrameTelemetryEnvelope value;
  value.identity = identity();
  value.sample.frameSerial = frameSerial;
  value.sample.visualTimeMicros = static_cast<std::int64_t>(frameSerial) * 1000;
  return value;
}

void setDeniedCounter(SkinRenderIoCounters &counters,
                      SkinRenderIoOperation operation,
                      std::uint64_t count) {
  switch (operation) {
  case SkinRenderIoOperation::FilesystemRead:
    counters.filesystemReadsDenied = count;
    return;
  case SkinRenderIoOperation::FilesystemWrite:
    counters.filesystemWritesDenied = count;
    return;
  case SkinRenderIoOperation::FilesystemDirectoryScan:
    counters.filesystemDirectoryScansDenied = count;
    return;
  case SkinRenderIoOperation::ResourceUpload:
    counters.resourceUploadsDenied = count;
    return;
  }
}

void contributeCompleteFrame(SkinFrameTelemetryEnvelope &value) {
  constexpr SkinTelemetryContribution ordered[] = {
      SkinTelemetryContribution::Runtime,
      SkinTelemetryContribution::FileSystem,
      SkinTelemetryContribution::Resources,
      SkinTelemetryContribution::Renderer,
      SkinTelemetryContribution::Session,
      SkinTelemetryContribution::Coordinator,
      SkinTelemetryContribution::MainLoop,
  };
  for (const auto contribution : ordered) {
    expect(addSkinTelemetryContribution(value, contribution) ==
               SkinTelemetryContributionResult::Accepted,
           "each component contributes once in pipeline order");
  }
}

void testDisabledTelemetryAllocatesNothingAndRetainsNothing() {
  const auto allocationsBefore =
      allocation_probe::allocations.load(std::memory_order_relaxed);
  SkinPerformanceTelemetry telemetry;
  SkinFrameTelemetrySample sample;
  sample.frameSerial = 1;
  telemetry.record(sample);
  const auto summary = telemetry.summarize();
  const auto allocationsAfter =
      allocation_probe::allocations.load(std::memory_order_relaxed);

  expect(!telemetry.enabled(), "default telemetry is disabled");
  expect(allocationsAfter == allocationsBefore,
         "disabled construction, record, and summary allocate nothing");
  expect(summary.receivedSampleCount == 0 &&
             summary.retainedSampleCount == 0 &&
             summary.overflowSampleCount == 0,
         "disabled telemetry does no sample work");
}

void testEnabledTelemetryReusesFixedStorageAndNeverEvicts() {
  SkinPerformanceTelemetry telemetry(true);
  const auto allocationsAfterConstruction =
      allocation_probe::allocations.load(std::memory_order_relaxed);
  SkinFrameTelemetrySample sample;
  for (std::size_t index = 0;
       index < SkinPerformanceTelemetry::maxSamples + 2; ++index) {
    sample.frameSerial = index + 1;
    sample.visualTimeMicros = static_cast<std::int64_t>(index);
    sample.evaluationMicros =
        index < SkinPerformanceTelemetry::maxSamples ? 10 : 9'000'000;
    sample.residentBytes =
        index < SkinPerformanceTelemetry::maxSamples ? 1'000 + index
                                                     : 90'000'000;
    telemetry.record(sample);
  }
  const auto summary = telemetry.summarize();
  const auto allocationsAfterUse =
      allocation_probe::allocations.load(std::memory_order_relaxed);

  expect(telemetry.enabled(), "explicit telemetry construction is enabled");
  expect(allocationsAfterUse == allocationsAfterConstruction,
         "recording and summarizing reuse constructor-owned bounded storage");
  expect(summary.receivedSampleCount == 65'538 &&
             summary.retainedSampleCount == 65'536 &&
             summary.overflowSampleCount == 2,
         "overflow is counted as fatal evidence without evicting retained samples");
  expect(summary.peakResidentBytes == 66'535 &&
             summary.residentDriftBytes == 65'535 &&
             summary.p99SkinCpuMicros == 10,
         "retained summaries exclude overflow sentinels and preserve the first accepted sample");
}

void testSummaryUsesNearestRankP99AndAggregatesEveryFact() {
  SkinPerformanceTelemetry telemetry(true);
  for (std::uint64_t index = 0; index < 100; ++index) {
    SkinFrameTelemetrySample sample;
    sample.frameSerial = index + 1;
    sample.evaluationMicros = index == 99 ? 48'000 : 400;
    sample.submissionMicros = index == 99 ? 1'000 : 300;
    sample.callbackMicros = index == 99 ? 1'000 : 200;
    sample.renderIo.filesystemReadsPerformed = 1;
    sample.renderIo.filesystemReadsDenied = 2;
    sample.renderIo.filesystemWritesPerformed = 3;
    sample.renderIo.filesystemWritesDenied = 4;
    sample.renderIo.filesystemDirectoryScansPerformed = 5;
    sample.renderIo.filesystemDirectoryScansDenied = 6;
    sample.renderIo.resourceUploadsPerformed = 7;
    sample.renderIo.resourceUploadsDenied = 8;
    sample.liveTextures = 40 + index;
    sample.liveResources = 80 + index * 2;
    sample.luaAllocatorBytes = 1'000 + index * 3;
    sample.residentBytes = 10'000 + index * 5;
    sample.missedPresentation = index < 2;
    sample.fallback = index < 3;
    telemetry.record(sample);
  }

  const auto summary = telemetry.summarize();
  expect(summary.p99SkinCpuMicros == 700,
         "p99 sums evaluation and submission without double-counting callbacks nested in evaluation");
  expect(std::abs(summary.missedPresentationPercent - 2.0) < 0.000001,
         "summary reports missed presentations as a percent");
  expect(summary.peakLuaAllocatorBytes == 1'297 &&
             summary.peakResidentBytes == 10'495 &&
             summary.residentDriftBytes == 495,
         "summary reports allocator and resident peaks plus signed drift");
  expect(summary.liveTextureDrift == 99 &&
             summary.liveResourceDrift == 198 && summary.fallbackCount == 3,
         "summary reports live-resource drift and fallbacks");
  expect(summary.renderIo.filesystemReadsPerformed == 100 &&
             summary.renderIo.filesystemReadsDenied == 200 &&
             summary.renderIo.filesystemWritesPerformed == 300 &&
             summary.renderIo.filesystemWritesDenied == 400 &&
             summary.renderIo.filesystemDirectoryScansPerformed == 500 &&
             summary.renderIo.filesystemDirectoryScansDenied == 600 &&
             summary.renderIo.resourceUploadsPerformed == 700 &&
             summary.renderIo.resourceUploadsDenied == 800,
         "all eight performed and denied I/O counters aggregate independently");
}

void testContributionMaskRejectsDuplicateOutOfOrderAndInvalidEnvelopes() {
  auto value = envelope(90);
  expect(addSkinTelemetryContribution(value, SkinTelemetryContribution::Runtime) ==
             SkinTelemetryContributionResult::Accepted,
         "runtime starts a valid envelope");
  expect(addSkinTelemetryContribution(value, SkinTelemetryContribution::Runtime) ==
             SkinTelemetryContributionResult::Duplicate,
         "a component cannot contribute twice");
  expect(addSkinTelemetryContribution(value, SkinTelemetryContribution::Renderer) ==
             SkinTelemetryContributionResult::OutOfOrder,
         "a later component cannot skip earlier contributors");
  expect(!isCompleteSkinTelemetryEnvelope(value),
         "a partial contribution prefix is incomplete");

  auto zeroFrame = envelope(0);
  expect(addSkinTelemetryContribution(zeroFrame,
                                      SkinTelemetryContribution::Runtime) ==
             SkinTelemetryContributionResult::InvalidEnvelope,
         "zero frame serials cannot start an envelope");
  auto emptyProfile = envelope();
  emptyProfile.identity.profileId.opaque.clear();
  expect(addSkinTelemetryContribution(emptyProfile,
                                      SkinTelemetryContribution::Runtime) ==
             SkinTelemetryContributionResult::InvalidEnvelope,
         "an incomplete five-field identity cannot start an envelope");

  auto complete = envelope(91);
  contributeCompleteFrame(complete);
  expect(complete.contributions == kCompleteSkinTelemetryContributions &&
             isCompleteSkinTelemetryEnvelope(complete),
         "all seven exact contributors produce the frozen complete mask");
  expect(addSkinTelemetryContribution(complete,
                                      SkinTelemetryContribution::MainLoop) ==
             SkinTelemetryContributionResult::Duplicate,
         "the post-scene finalizer cannot finalize twice");

  auto forgedGap = envelope(92);
  forgedGap.contributions =
      static_cast<std::uint32_t>(SkinTelemetryContribution::Runtime) |
      static_cast<std::uint32_t>(SkinTelemetryContribution::Resources);
  expect(addSkinTelemetryContribution(forgedGap,
                                      SkinTelemetryContribution::FileSystem) ==
             SkinTelemetryContributionResult::InvalidEnvelope,
         "a forged out-of-order contribution history cannot be repaired");
  auto invalidContribution = envelope(93);
  expect(addSkinTelemetryContribution(
             invalidContribution,
             static_cast<SkinTelemetryContribution>(1U << 8U)) ==
             SkinTelemetryContributionResult::InvalidContribution,
         "a contribution outside the frozen seven-bit mask is rejected");
}

void testEnvelopeValidationPinsEveryIdentityFieldAndStrictFrameOrder() {
  auto complete = envelope(20);
  contributeCompleteFrame(complete);
  const auto expected = identity();
  expect(validateSkinTelemetryEnvelope(complete, expected, 19) ==
             SkinTelemetryEnvelopeError::None,
         "a complete matching envelope with increasing serial is valid");
  expect(validateSkinTelemetryEnvelope(complete, expected, 20) ==
             SkinTelemetryEnvelopeError::NonIncreasingFrameSerial,
         "a duplicate frame serial is rejected");
  expect(validateSkinTelemetryEnvelope(complete, expected, 21) ==
             SkinTelemetryEnvelopeError::NonIncreasingFrameSerial,
         "an out-of-order frame serial is rejected");

  auto partial = complete;
  partial.contributions &=
      ~static_cast<std::uint32_t>(SkinTelemetryContribution::Coordinator);
  expect(validateSkinTelemetryEnvelope(partial, expected, 19) ==
             SkinTelemetryEnvelopeError::IncompleteContributions,
         "an incomplete frame is rejected before recording");

  auto wrong = complete;
  wrong.identity.sessionSerial = 18;
  expect(validateSkinTelemetryEnvelope(wrong, expected, 19) ==
             SkinTelemetryEnvelopeError::UnexpectedIdentity,
         "a wrong session serial is rejected");
  wrong = complete;
  wrong.identity.profileId.opaque = "other-profile";
  expect(validateSkinTelemetryEnvelope(wrong, expected, 19) ==
             SkinTelemetryEnvelopeError::UnexpectedIdentity,
         "a wrong profile is rejected");
  wrong = complete;
  wrong.identity.entry.packageRelativePath = "skin/other.luaskin";
  expect(validateSkinTelemetryEnvelope(wrong, expected, 19) ==
             SkinTelemetryEnvelopeError::UnexpectedIdentity,
         "a wrong entry is rejected");
  wrong = complete;
  wrong.identity.revisionDigest = "other-revision";
  expect(validateSkinTelemetryEnvelope(wrong, expected, 19) ==
             SkinTelemetryEnvelopeError::UnexpectedIdentity,
         "a wrong revision is rejected");
  wrong = complete;
  wrong.identity.configurationDigest = "other-configuration";
  expect(validateSkinTelemetryEnvelope(wrong, expected, 19) ==
             SkinTelemetryEnvelopeError::UnexpectedIdentity,
         "a wrong configuration is rejected");
}

SkinPerformanceSummary passingSummary() {
  SkinPerformanceSummary summary;
  summary.receivedSampleCount = 10'800;
  summary.retainedSampleCount = 10'800;
  summary.p99SkinCpuMicros = 15'000;
  summary.missedPresentationPercent = 0.5;
  summary.residentDriftBytes = 32LL * 1024 * 1024;
  return summary;
}

SkinPerformanceRunFacts passingFacts(std::uint16_t hz = 60) {
  return {
      .configuredRefreshHz = hz,
      .warmupStartedVisualTimeMicros = 1'000'000,
      .trustedFirstRecordedVisualTimeMicros = 31'000'000,
      .trustedLastRecordedVisualTimeMicros = 211'000'000,
      .sessionEndedBeforeRecordingComplete = false,
      .envelopeStreamValid = true,
  };
}

void testPerformanceValidationEnforcesRefreshTimingAndBudgets() {
  expect(validateSkinPerformanceRun(passingSummary(), passingFacts()).passed(),
         "the exact 30-second warmup and 180-second passing run validate");
  auto highRefreshSummary = passingSummary();
  highRefreshSummary.p99SkinCpuMicros = 3'750;
  expect(validateSkinPerformanceRun(passingSummary(), passingFacts(1)).passed() &&
             validateSkinPerformanceRun(highRefreshSummary, passingFacts(240))
                 .passed(),
         "configured refresh endpoints 1 and 240 Hz are valid");
  expect(validateSkinPerformanceRun(passingSummary(), passingFacts(0)).failure ==
             SkinPerformanceRunFailure::InvalidRefreshRate &&
             validateSkinPerformanceRun(passingSummary(), passingFacts(241))
                     .failure ==
                 SkinPerformanceRunFailure::InvalidRefreshRate,
         "refresh outside 1 through 240 Hz is rejected");

  auto facts = passingFacts();
  ++facts.trustedFirstRecordedVisualTimeMicros;
  expect(validateSkinPerformanceRun(passingSummary(), facts).failure ==
             SkinPerformanceRunFailure::InvalidTrustedClockWindow,
         "warmup must be exactly thirty trusted visual seconds");
  facts = passingFacts();
  --facts.trustedLastRecordedVisualTimeMicros;
  expect(validateSkinPerformanceRun(passingSummary(), facts).failure ==
             SkinPerformanceRunFailure::InvalidTrustedClockWindow,
         "recording must span exactly one hundred eighty trusted visual seconds");
  facts = passingFacts();
  facts.sessionEndedBeforeRecordingComplete = true;
  expect(validateSkinPerformanceRun(passingSummary(), facts).failure ==
             SkinPerformanceRunFailure::EarlySessionEnd,
         "an early session end invalidates the run");
  facts = passingFacts();
  facts.envelopeStreamValid = false;
  expect(validateSkinPerformanceRun(passingSummary(), facts).failure ==
             SkinPerformanceRunFailure::InvalidEnvelopeStream,
         "an incomplete, mismatched, duplicate, or out-of-order stream is fatal");

  auto summary = passingSummary();
  summary.p99SkinCpuMicros = 15'001;
  expect(validateSkinPerformanceRun(summary, passingFacts()).failure ==
             SkinPerformanceRunFailure::CpuBudgetExceeded,
         "p99 must not exceed ninety percent of the actual refresh interval");
  summary = passingSummary();
  summary.missedPresentationPercent = 0.500001;
  expect(validateSkinPerformanceRun(summary, passingFacts()).failure ==
             SkinPerformanceRunFailure::MissedPresentationBudgetExceeded,
         "missed presentations must not exceed one half percent");
  summary = passingSummary();
  summary.residentDriftBytes = 32LL * 1024 * 1024 + 1;
  expect(validateSkinPerformanceRun(summary, passingFacts()).failure ==
             SkinPerformanceRunFailure::ResidentDriftExceeded,
         "positive resident drift above thirty-two MiB is rejected");
  summary.residentDriftBytes = -(32LL * 1024 * 1024 + 1);
  expect(validateSkinPerformanceRun(summary, passingFacts()).passed(),
         "a resident-memory decrease is not evidence of growth");
  summary = passingSummary();
  summary.missedPresentationPercent =
      std::numeric_limits<double>::quiet_NaN();
  expect(validateSkinPerformanceRun(summary, passingFacts()).failure ==
             SkinPerformanceRunFailure::MissedPresentationBudgetExceeded,
         "a non-finite missed-presentation percentage is rejected");
  summary = passingSummary();
  ++summary.receivedSampleCount;
  summary.overflowSampleCount = 1;
  expect(validateSkinPerformanceRun(summary, passingFacts()).failure ==
             SkinPerformanceRunFailure::SampleOverflow,
         "any sample overflow is acceptance-fatal");
  summary = passingSummary();
  summary.receivedSampleCount = SkinPerformanceTelemetry::maxSamples + 1;
  summary.retainedSampleCount = SkinPerformanceTelemetry::maxSamples + 1;
  expect(validateSkinPerformanceRun(summary, passingFacts()).failure ==
             SkinPerformanceRunFailure::SampleCountMismatch,
         "a forged summary cannot retain more than the fixed sample capacity");
}

void testPerformancePassingRunRequiresEveryIoCounterZero() {
  using Member = std::uint64_t SkinRenderIoCounters::*;
  constexpr Member counters[] = {
      &SkinRenderIoCounters::filesystemReadsPerformed,
      &SkinRenderIoCounters::filesystemReadsDenied,
      &SkinRenderIoCounters::filesystemWritesPerformed,
      &SkinRenderIoCounters::filesystemWritesDenied,
      &SkinRenderIoCounters::filesystemDirectoryScansPerformed,
      &SkinRenderIoCounters::filesystemDirectoryScansDenied,
      &SkinRenderIoCounters::resourceUploadsPerformed,
      &SkinRenderIoCounters::resourceUploadsDenied,
  };
  for (const auto counter : counters) {
    auto summary = passingSummary();
    summary.renderIo.*counter = 1;
    expect(validateSkinPerformanceRun(summary, passingFacts()).failure ==
               SkinPerformanceRunFailure::RenderIoObserved,
           "each performed or denied render I/O counter independently fails a performance run");
  }
}

void testNegativeRunAllowsExactlyTheSelectedDeniedOperation() {
  using Performed = std::uint64_t SkinRenderIoCounters::*;
  constexpr SkinRenderIoOperation operations[] = {
      SkinRenderIoOperation::FilesystemRead,
      SkinRenderIoOperation::FilesystemWrite,
      SkinRenderIoOperation::FilesystemDirectoryScan,
      SkinRenderIoOperation::ResourceUpload,
  };
  constexpr Performed performed[] = {
      &SkinRenderIoCounters::filesystemReadsPerformed,
      &SkinRenderIoCounters::filesystemWritesPerformed,
      &SkinRenderIoCounters::filesystemDirectoryScansPerformed,
      &SkinRenderIoCounters::resourceUploadsPerformed,
  };

  for (std::size_t selected = 0; selected < 4; ++selected) {
    SkinRenderIoCounters counters;
    setDeniedCounter(counters, operations[selected], 3);
    expect(validateNegativeSkinRenderIo(counters, operations[selected]),
           "the selected denied operation may be positive with all performed counters zero");
    expect(!validateNegativeSkinRenderIo(counters,
                                         operations[(selected + 1) % 4]),
           "a denial for an unrelated operation cannot satisfy the negative run");
    counters.*performed[selected] = 1;
    expect(!validateNegativeSkinRenderIo(counters, operations[selected]),
           "any performed operation invalidates a negative run");
  }

  SkinRenderIoCounters none;
  expect(!validateNegativeSkinRenderIo(none,
                                       SkinRenderIoOperation::FilesystemRead),
         "the selected denied counter must be positive");
  setDeniedCounter(none, SkinRenderIoOperation::FilesystemRead, 1);
  setDeniedCounter(none, SkinRenderIoOperation::ResourceUpload, 1);
  expect(!validateNegativeSkinRenderIo(none,
                                       SkinRenderIoOperation::FilesystemRead),
         "an unrelated denial invalidates a negative run");
  SkinRenderIoCounters invalidSelection;
  expect(!validateNegativeSkinRenderIo(
             invalidSelection, static_cast<SkinRenderIoOperation>(99)),
         "an operation outside the closed negative-run set is rejected");
}

} // namespace

int main() {
  testDisabledTelemetryAllocatesNothingAndRetainsNothing();
  testEnabledTelemetryReusesFixedStorageAndNeverEvicts();
  testSummaryUsesNearestRankP99AndAggregatesEveryFact();
  testContributionMaskRejectsDuplicateOutOfOrderAndInvalidEnvelopes();
  testEnvelopeValidationPinsEveryIdentityFieldAndStrictFrameOrder();
  testPerformanceValidationEnforcesRefreshTimingAndBudgets();
  testPerformancePassingRunRequiresEveryIoCounterZero();
  testNegativeRunAllowsExactlyTheSelectedDeniedOperation();
  return failures == 0 ? 0 : 1;
}
