#include "SkinAcceptanceRecorder.h"

#include "../../FileChecksum.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace skin {
namespace {

constexpr std::int64_t kWarmupMicros = 30'000'000;
constexpr std::int64_t kMeasurementMicros = 180'000'000;
constexpr std::uint32_t kLifecycleCycles = 10;
constexpr std::uint32_t kMaximumRefreshHz = 240;
constexpr std::size_t kMaximumDiagnosticHistoryEntries = 256;
// Acceptance exports intentionally expose only app-authored, stable gameplay
// diagnostic identifiers. Adding a value requires a source and privacy audit;
// namespace-shaped input is not proof that a code is internal.
constexpr std::array<std::string_view, 28>
    kAcceptanceDiagnosticHistoryCodeAllowlist{
        "custom_object_order_authored_divergence",
        "skin.play_state.custom_event_binding_missing",
        "skin.play_state.custom_event_callback_failed",
        "skin.play_state.custom_event_clock_failed",
        "skin.play_state.custom_event_condition_failed",
        "skin.play_state.custom_event_condition_missing",
        "skin.play_state.custom_event_condition_type",
        "skin.play_state.custom_timer_binding_missing",
        "skin.play_state.custom_timer_cache_failed",
        "skin.play_state.custom_timer_callback_failed",
        "skin.play_state.custom_timer_type",
        "skin.play_state.diagnostics_truncated",
        "skin.play_state.frame_already_closed",
        "skin.play_state.frame_inactive",
        "skin.play_state.frame_serial_invalid",
        "skin.play_state.frame_serial_not_increasing",
        "skin.play_state.mutation_limit_exceeded",
        "skin.play_state.unsupported",
        "skin.play_state.writer_builtin_unsupported",
        "skin.play_state.writer_callback_failed",
        "skin.play_state.writer_missing",
        "skin.play_state.writer_reentrant",
        "skin.play_state.writer_value_nonfinite",
        "skin_file_render_phase_denied",
        "skin_lua_event_execution_failed",
        "skin_lua_event_executor_unavailable",
        "skin_lua_model_authored_note_visual_ignored",
        "skin_overlay_identity_invalid",
    };

enum class FatalReason : std::uint8_t {
  None,
  InvalidContract,
  InvalidSession,
  InvalidEnvelope,
  ClockReversal,
  EarlySessionEnd,
  SampleOverflow,
  InvalidPerformance,
  InvalidLifecycle,
  OverlayDigest,
  InvalidNegativeEvidence,
  ExportFailed,
  Shutdown,
};

std::string_view codeFor(FatalReason reason) noexcept {
  switch (reason) {
  case FatalReason::None:
    return "skin_acceptance_failed";
  case FatalReason::InvalidContract:
    return "skin_acceptance_contract_invalid";
  case FatalReason::InvalidSession:
    return "skin_acceptance_session_mismatch";
  case FatalReason::InvalidEnvelope:
    return "skin_acceptance_envelope_invalid";
  case FatalReason::ClockReversal:
    return "skin_acceptance_clock_reversed";
  case FatalReason::EarlySessionEnd:
    return "skin_acceptance_session_ended_early";
  case FatalReason::SampleOverflow:
    return "skin_acceptance_sample_overflow";
  case FatalReason::InvalidPerformance:
    return "skin_acceptance_performance_invalid";
  case FatalReason::InvalidLifecycle:
    return "skin_acceptance_lifecycle_invalid";
  case FatalReason::OverlayDigest:
    return "skin_acceptance_overlay_digest_invalid";
  case FatalReason::InvalidNegativeEvidence:
    return "skin_acceptance_negative_evidence_invalid";
  case FatalReason::ExportFailed:
    return "skin_acceptance_export_failed";
  case FatalReason::Shutdown:
    return "skin_acceptance_shutdown";
  }
  return "skin_acceptance_failed";
}

bool isLowerHex(std::string_view value, std::size_t size) noexcept {
  if (value.size() != size) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

bool isOpaque(std::string_view value) noexcept {
  if (value.size() < 4 || value.size() > 128 ||
      !((value.front() >= 'a' && value.front() <= 'z') ||
        (value.front() >= '0' && value.front() <= '9'))) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == ':' ||
           character == '-';
  });
}

bool isSafeToken(std::string_view value, std::size_t maximum = 128) noexcept {
  if (value.empty() || value.size() > maximum) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == ':' ||
           character == '-' || character == '_' || character == '.';
  });
}

bool isAllowedDiagnosticHistoryCode(std::string_view value) noexcept {
  return std::ranges::find(kAcceptanceDiagnosticHistoryCodeAllowlist, value) !=
         kAcceptanceDiagnosticHistoryCodeAllowlist.end();
}

bool validEntry(const SkinEntryId &entry) noexcept {
  return !entry.package.directoryName.empty() &&
         !entry.package.collisionKey.empty() &&
         !entry.packageRelativePath.empty() && !entry.collisionKey.empty();
}

bool validActivation(const SkinAcceptanceActivationKey &key) noexcept {
  return !key.profileId.opaque.empty() && validEntry(key.entry) &&
         isLowerHex(key.revisionDigest, 64) &&
         isLowerHex(key.configurationDigest, 64);
}

bool sameActivation(const PlaySkinSessionIdentity &identity,
                    const SkinAcceptanceActivationKey &key) noexcept {
  return identity.sessionSerial != 0 && identity.profileId == key.profileId &&
         identity.entry == key.entry &&
         identity.revisionDigest == key.revisionDigest &&
         identity.configurationDigest == key.configurationDigest;
}

bool sameIdentity(const PlaySkinSessionIdentity &left,
                  const PlaySkinSessionIdentity &right) noexcept {
  return left.sessionSerial == right.sessionSerial &&
         left.profileId == right.profileId && left.entry == right.entry &&
         left.revisionDigest == right.revisionDigest &&
         left.configurationDigest == right.configurationDigest;
}

std::optional<std::int64_t> checkedTimestampAdd(std::int64_t base,
                                                std::int64_t delta) noexcept {
  if (delta < 0 || base > std::numeric_limits<std::int64_t>::max() - delta) {
    return std::nullopt;
  }
  return base + delta;
}

bool validContract(const SkinAcceptanceScenarioContract &contract,
                   std::string_view requestedId) noexcept {
  if (contract.scenarioId != requestedId || !isSafeToken(contract.scenarioId) ||
      !isLowerHex(contract.expectedChartSha256, 64) ||
      !isSafeToken(contract.expectedLayoutId, 64) ||
      !isLowerHex(contract.expectedOpaqueGuardVectorSha256, 64) ||
      contract.warmupMicros != kWarmupMicros ||
      contract.measurementMicros != kMeasurementMicros ||
      contract.requiredExitCycles != kLifecycleCycles ||
      contract.expectedRefreshHz == 0 ||
      contract.expectedRefreshHz > kMaximumRefreshHz ||
      contract.maximumRefreshHz != kMaximumRefreshHz) {
    return false;
  }
  if (contract.kind != SkinAcceptanceRunKind::RenderIoNegative) {
    return !contract.expectedDiagnosticCode &&
           !contract.expectedFallbackAction &&
           !contract.expectedDeniedOperation;
  }
  return contract.expectedDiagnosticCode &&
         isSafeToken(*contract.expectedDiagnosticCode) &&
         contract.expectedFallbackAction &&
         isSafeToken(*contract.expectedFallbackAction) &&
         contract.expectedDeniedOperation.has_value();
}

SkinDiagnostic failureDiagnostic(FatalReason reason) {
  return {.code = std::string(codeFor(reason)),
          .message = "Acceptance capture failed; no evidence was accepted.",
          .severity = DiagnosticSeverity::Error};
}

std::uint64_t nextExportTicketValue() noexcept {
  static std::atomic<std::uint64_t> next{1};
  auto candidate = next.load(std::memory_order_relaxed);
  while (candidate != std::numeric_limits<std::uint64_t>::max()) {
    if (next.compare_exchange_weak(candidate, candidate + 1,
                                   std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return candidate;
    }
  }
  return 0;
}

std::uint64_t nextBoundScopeSerial() noexcept {
  static std::atomic<std::uint64_t> next{1};
  auto candidate = next.load(std::memory_order_relaxed);
  while (candidate != std::numeric_limits<std::uint64_t>::max()) {
    if (next.compare_exchange_weak(candidate, candidate + 1,
                                   std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return candidate;
    }
  }
  return 0;
}

struct AcceptanceBoundScopeKey {
  std::uint64_t recorderSerial = 0;
  std::uint64_t runSerial = 0;
  std::uint64_t bindingSerial = 0;
  PlaySkinSessionIdentity identity;
};

struct AcceptanceBoundScopeView {
  std::uint64_t recorderSerial = 0;
  std::uint64_t runSerial = 0;
  std::uint64_t bindingSerial = 0;
  const PlaySkinSessionIdentity &identity;
};

void appendUnsigned(std::string &output, std::uint64_t value) {
  std::array<char, 32> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (error == std::errc{}) {
    output.append(buffer.data(), end);
  }
}

void appendSigned(std::string &output, std::int64_t value) {
  std::array<char, 32> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (error == std::errc{}) {
    output.append(buffer.data(), end);
  }
}

void appendDouble(std::string &output, double value) {
  if (!std::isfinite(value)) {
    throw std::runtime_error("non-finite acceptance metric");
  }
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream << std::setprecision(8) << std::defaultfloat << value;
  if (!stream) {
    throw std::runtime_error("acceptance metric formatting failed");
  }
  output += stream.str();
}

void appendQuoted(std::string &output, std::string_view value) {
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20) {
        constexpr char hex[] = "0123456789abcdef";
        output += "\\u00";
        output.push_back(hex[character >> 4U]);
        output.push_back(hex[character & 0x0fU]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  output.push_back('"');
}

void appendRenderIo(std::string &output, const SkinRenderIoCounters &counters) {
  output += "{\"filesystemReadsPerformed\":";
  appendUnsigned(output, counters.filesystemReadsPerformed);
  output += ",\"filesystemReadsDenied\":";
  appendUnsigned(output, counters.filesystemReadsDenied);
  output += ",\"filesystemWritesPerformed\":";
  appendUnsigned(output, counters.filesystemWritesPerformed);
  output += ",\"filesystemWritesDenied\":";
  appendUnsigned(output, counters.filesystemWritesDenied);
  output += ",\"filesystemDirectoryScansPerformed\":";
  appendUnsigned(output, counters.filesystemDirectoryScansPerformed);
  output += ",\"filesystemDirectoryScansDenied\":";
  appendUnsigned(output, counters.filesystemDirectoryScansDenied);
  output += ",\"resourceUploadsPerformed\":";
  appendUnsigned(output, counters.resourceUploadsPerformed);
  output += ",\"resourceUploadsDenied\":";
  appendUnsigned(output, counters.resourceUploadsDenied);
  output.push_back('}');
}

void appendGroupedIo(std::string &output, const SkinRenderIoCounters &counters,
                     bool denied) {
  output += "{\"filesystemReads\":";
  appendUnsigned(output, denied ? counters.filesystemReadsDenied
                                : counters.filesystemReadsPerformed);
  output += ",\"filesystemWrites\":";
  appendUnsigned(output, denied ? counters.filesystemWritesDenied
                                : counters.filesystemWritesPerformed);
  output += ",\"filesystemDirectoryScans\":";
  appendUnsigned(output, denied ? counters.filesystemDirectoryScansDenied
                                : counters.filesystemDirectoryScansPerformed);
  output += ",\"resourceUploads\":";
  appendUnsigned(output, denied ? counters.resourceUploadsDenied
                                : counters.resourceUploadsPerformed);
  output.push_back('}');
}

} // namespace

class SkinAcceptanceRecorder::Impl {
public:
  explicit Impl(SkinAcceptanceRecorderDependencies dependencies)
      : dependencies_(std::move(dependencies)),
        recorderScopeSerial_(nextBoundScopeSerial()) {}

  ~Impl() { shutdown(); }

  bool arm(std::string opaqueRunId, std::string scenarioId,
           SkinAcceptanceActivationKey activation) {
    if (closed_ ||
        state_.load(std::memory_order_acquire) !=
            SkinAcceptanceCaptureState::Idle ||
        currentExportTicket().has_value() || recorderScopeSerial_ == 0 ||
        !isOpaque(opaqueRunId) ||
        !validActivation(activation) ||
        !dependencies_.buildIdentity.validForAcceptance() ||
        !isSafeToken(dependencies_.buildIdentity.configuration, 64) ||
        dependencies_.documentsRoot.empty() || !dependencies_.resolveScenario ||
        !dependencies_.writeAtomic) {
      return false;
    }

    std::optional<SkinAcceptanceScenarioContract> resolved;
    try {
      resolved = dependencies_.resolveScenario(scenarioId);
    } catch (...) {
      return false;
    }
    if (!resolved || !validContract(*resolved, scenarioId) ||
        (resolved->kind == SkinAcceptanceRunKind::RenderIoNegative &&
         dependencies_.overlayDigests == nullptr)) {
      return false;
    }

    const auto runScopeSerial = nextBoundScopeSerial();
    if (runScopeSerial == 0) {
      return false;
    }

    resetRun();
    currentRunScopeSerial_ = runScopeSerial;
    runId_ = std::move(opaqueRunId);
    contract_ = std::move(*resolved);
    activation_ = std::move(activation);
    state_.store(SkinAcceptanceCaptureState::Armed, std::memory_order_release);

    if (contract_->kind == SkinAcceptanceRunKind::RenderIoNegative) {
      try {
        const auto ticket =
            dependencies_.overlayDigests->beginDigest(*activation_);
        if (!ticket) {
          fail(FatalReason::OverlayDigest);
          return false;
        }
        beforeDigestTicket_ = ticket;
      } catch (...) {
        fail(FatalReason::OverlayDigest);
        return false;
      }
    }
    return true;
  }

  std::optional<AcceptanceBoundScopeKey>
  bindSession(const SkinAcceptanceSessionFacts &facts) {
    if (closed_ || !contract_ || !activation_) {
      return std::nullopt;
    }
    const auto currentState = state_.load(std::memory_order_acquire);
    if (contract_->kind == SkinAcceptanceRunKind::RenderIoNegative &&
        !beforeDigest_.has_value()) {
      return std::nullopt;
    }
    if (currentState != SkinAcceptanceCaptureState::Armed) {
      if (currentState != SkinAcceptanceCaptureState::Failed) {
        fail(FatalReason::InvalidSession);
      }
      return std::nullopt;
    }
    if (!sameActivation(facts.identity, *activation_) ||
        facts.chartSha256 != contract_->expectedChartSha256 ||
        facts.layoutId != contract_->expectedLayoutId ||
        facts.actualRefreshHz != contract_->expectedRefreshHz ||
        facts.observedOpaqueGuardVectorSha256 !=
            contract_->expectedOpaqueGuardVectorSha256) {
      fail(FatalReason::InvalidSession);
      return std::nullopt;
    }

    if (contract_->kind == SkinAcceptanceRunKind::ResourceLifecycle) {
      if (!lifecycleBaseline_ || currentSession_ ||
          facts.identity.sessionSerial <= lastLifecycleSessionSerial_) {
        fail(FatalReason::InvalidLifecycle);
        return std::nullopt;
      }
    } else if (boundOnce_) {
      fail(FatalReason::InvalidSession);
      return std::nullopt;
    }

    const auto bindingSerial = nextBoundScopeSerial();
    if (bindingSerial == 0) {
      fail(FatalReason::InvalidSession);
      return std::nullopt;
    }

    currentSession_ = facts.identity;
    currentBoundScopeSerial_ = bindingSerial;
    sessionFacts_ = facts;
    sessionEnded_ = false;
    pendingLifecycleSample_.reset();
    previousFrameSerial_ = 0;
    previousVisualTime_.reset();
    boundOnce_ = true;

    if (contract_->kind != SkinAcceptanceRunKind::ResourceLifecycle) {
      try {
        telemetry_ = SkinPerformanceTelemetry(true);
      } catch (...) {
        fail(FatalReason::SampleOverflow);
        return std::nullopt;
      }
    }
    state_.store(contract_->kind == SkinAcceptanceRunKind::Performance
                     ? SkinAcceptanceCaptureState::WarmingUp
                     : SkinAcceptanceCaptureState::Recording,
                 std::memory_order_release);
    try {
      return AcceptanceBoundScopeKey{.recorderSerial = recorderScopeSerial_,
                                     .runSerial = currentRunScopeSerial_,
                                     .bindingSerial = bindingSerial,
                                     .identity = facts.identity};
    } catch (...) {
      fail(FatalReason::InvalidSession);
      return std::nullopt;
    }
  }

  void record(SkinFrameTelemetryEnvelope &&envelope) noexcept {
    if (closed_ || !contract_) {
      return;
    }
    const auto currentState = state_.load(std::memory_order_acquire);
    if (contract_->kind == SkinAcceptanceRunKind::RenderIoNegative &&
        sessionEnded_ &&
        currentState == SkinAcceptanceCaptureState::Recording) {
      fail(FatalReason::InvalidNegativeEvidence);
      return;
    }
    if (!currentSession_ ||
        (currentState != SkinAcceptanceCaptureState::WarmingUp &&
         currentState != SkinAcceptanceCaptureState::Recording) ||
        contract_->kind == SkinAcceptanceRunKind::ResourceLifecycle) {
      return;
    }

    const auto envelopeError = validateSkinTelemetryEnvelope(
        envelope, *currentSession_, previousFrameSerial_);
    if (envelopeError != SkinTelemetryEnvelopeError::None) {
      fail(FatalReason::InvalidEnvelope);
      return;
    }
    if (previousFrameSerial_ != 0 &&
        (previousFrameSerial_ == std::numeric_limits<std::uint64_t>::max() ||
         envelope.sample.frameSerial != previousFrameSerial_ + 1)) {
      fail(FatalReason::InvalidEnvelope);
      return;
    }
    if (previousVisualTime_ &&
        envelope.sample.visualTimeMicros < *previousVisualTime_) {
      fail(FatalReason::ClockReversal);
      return;
    }
    previousFrameSerial_ = envelope.sample.frameSerial;
    previousVisualTime_ = envelope.sample.visualTimeMicros;

    if (contract_->kind == SkinAcceptanceRunKind::RenderIoNegative) {
      fallbackObserved_ = fallbackObserved_ || envelope.sample.fallback;
      if (retainedFrameCount_ == SkinPerformanceTelemetry::maxSamples) {
        fail(FatalReason::SampleOverflow);
        return;
      }
      telemetry_.record(envelope.sample);
      ++retainedFrameCount_;
      return;
    }

    const auto visualTime = envelope.sample.visualTimeMicros;
    if (!warmupStartVisualTime_) {
      warmupStartVisualTime_ = visualTime;
      expectedRecordingStartVisualTime_ =
          checkedTimestampAdd(visualTime, contract_->warmupMicros);
      if (!expectedRecordingStartVisualTime_) {
        fail(FatalReason::InvalidPerformance);
        return;
      }
      expectedRecordingEndVisualTime_ = checkedTimestampAdd(
          *expectedRecordingStartVisualTime_, contract_->measurementMicros);
      if (!expectedRecordingEndVisualTime_) {
        fail(FatalReason::InvalidPerformance);
      }
      return;
    }
    if (visualTime < *expectedRecordingStartVisualTime_) {
      return;
    }
    if (!firstRetainedVisualTime_ &&
        visualTime != *expectedRecordingStartVisualTime_) {
      fail(FatalReason::InvalidPerformance);
      return;
    }
    if (visualTime > *expectedRecordingEndVisualTime_) {
      fail(FatalReason::InvalidPerformance);
      return;
    }
    state_.store(SkinAcceptanceCaptureState::Recording,
                 std::memory_order_release);
    if (retainedFrameCount_ == SkinPerformanceTelemetry::maxSamples) {
      fail(FatalReason::SampleOverflow);
      return;
    }
    telemetry_.record(envelope.sample);
    ++retainedFrameCount_;
    if (!firstRetainedVisualTime_) {
      firstRetainedVisualTime_ = visualTime;
    }
    lastRetainedVisualTime_ = visualTime;
    if (visualTime == *expectedRecordingEndVisualTime_) {
      try {
        (void)beginStopAndExport();
      } catch (...) {
        fail(FatalReason::ExportFailed);
      }
    }
  }

  void recordDiagnosticEvidence(const AcceptanceBoundScopeView &boundScope,
                                SkinDiagnostic diagnostic) noexcept {
    if (!acceptsNegativeEvidence(boundScope) || observedDiagnostic_ ||
        !isSafeToken(diagnostic.code)) {
      fail(FatalReason::InvalidNegativeEvidence);
      return;
    }
    try {
      // Messages and source paths may contain private package data. The typed
      // run/identity/code evidence is sufficient and is the only retained
      // value that may later cross into the export worker.
      diagnostic.message.clear();
      diagnostic.virtualPath.clear();
      diagnostic.source.reset();
      observedDiagnostic_ = std::move(diagnostic);
    } catch (...) {
      fail(FatalReason::InvalidNegativeEvidence);
    }
  }

  void recordFallbackAction(const AcceptanceBoundScopeView &boundScope,
                            std::string action) noexcept {
    if (!acceptsNegativeEvidence(boundScope) || observedFallbackAction_ ||
        !isSafeToken(action)) {
      fail(FatalReason::InvalidNegativeEvidence);
      return;
    }
    try {
      observedFallbackAction_ = std::move(action);
    } catch (...) {
      fail(FatalReason::InvalidNegativeEvidence);
    }
  }

  void recordResourceLifecycle(SkinResourceLifecycleSample sample) noexcept {
    if (closed_ || !contract_ ||
        contract_->kind != SkinAcceptanceRunKind::ResourceLifecycle) {
      return;
    }
    if (sample.phase == SkinResourceLifecyclePhase::BeforeFirstEntry) {
      if (state_.load(std::memory_order_acquire) !=
              SkinAcceptanceCaptureState::Armed ||
          lifecycleBaseline_ || currentSession_ || sample.cycleIndex != 0) {
        fail(FatalReason::InvalidLifecycle);
        return;
      }
      lifecycleBaseline_ = sample;
      return;
    }
    if (!currentSession_ || !sessionEnded_ || pendingLifecycleSample_ ||
        sample.cycleIndex != lifecycleSampleCount_ + 1 ||
        lifecycleSampleCount_ >= lifecycleSamples_.size()) {
      fail(FatalReason::InvalidLifecycle);
      return;
    }
    pendingLifecycleSample_ = sample;
  }

  void sessionEnded(const PlaySkinSessionIdentity &identity) noexcept {
    if (closed_ || !contract_ || !currentSession_ ||
        !sameIdentity(identity, *currentSession_)) {
      if (contract_ &&
          state_.load(std::memory_order_acquire) !=
              SkinAcceptanceCaptureState::Exporting &&
          state_.load(std::memory_order_acquire) !=
              SkinAcceptanceCaptureState::Exported) {
        fail(FatalReason::InvalidSession);
      }
      return;
    }
    if (sessionEnded_) {
      fail(FatalReason::InvalidSession);
      return;
    }
    sessionEnded_ = true;
    if (contract_->kind == SkinAcceptanceRunKind::Performance &&
        state_.load(std::memory_order_acquire) !=
            SkinAcceptanceCaptureState::Exporting &&
        state_.load(std::memory_order_acquire) !=
            SkinAcceptanceCaptureState::Exported) {
      fail(FatalReason::EarlySessionEnd);
    }
  }

  void
  sessionTeardownComplete(const PlaySkinSessionIdentity &identity) noexcept {
    if (closed_ || !contract_ || !currentSession_ || !sessionEnded_ ||
        !sameIdentity(identity, *currentSession_)) {
      fail(FatalReason::InvalidSession);
      return;
    }
    if (contract_->kind == SkinAcceptanceRunKind::Performance) {
      currentSession_.reset();
      return;
    }
    if (contract_->kind == SkinAcceptanceRunKind::ResourceLifecycle) {
      if (!lifecycleBaseline_ || !pendingLifecycleSample_) {
        fail(FatalReason::InvalidLifecycle);
        return;
      }
      if (pendingLifecycleSample_->liveTextures !=
              lifecycleBaseline_->liveTextures ||
          pendingLifecycleSample_->liveResources !=
              lifecycleBaseline_->liveResources) {
        fail(FatalReason::InvalidLifecycle);
        return;
      }
      lifecycleSamples_[lifecycleSampleCount_++] = *pendingLifecycleSample_;
      lastLifecycleSessionSerial_ = identity.sessionSerial;
      pendingLifecycleSample_.reset();
      currentSession_.reset();
      sessionEnded_ = false;
      if (lifecycleSampleCount_ == contract_->requiredExitCycles) {
        try {
          (void)beginStopAndExport();
        } catch (...) {
          fail(FatalReason::ExportFailed);
        }
      } else {
        state_.store(SkinAcceptanceCaptureState::Armed,
                     std::memory_order_release);
      }
      return;
    }

    try {
      const auto ticket =
          dependencies_.overlayDigests->beginDigest(*activation_);
      if (!ticket) {
        fail(FatalReason::OverlayDigest);
        return;
      }
      afterDigestTicket_ = ticket;
      currentSession_.reset();
    } catch (...) {
      fail(FatalReason::OverlayDigest);
    }
  }

  void pollAsyncDependencies() noexcept {
    if (closed_ || !contract_ ||
        contract_->kind != SkinAcceptanceRunKind::RenderIoNegative ||
        state_.load(std::memory_order_acquire) ==
            SkinAcceptanceCaptureState::Failed) {
      return;
    }
    try {
      if (beforeDigestTicket_) {
        const auto result =
            dependencies_.overlayDigests->pollDigest(*beforeDigestTicket_);
        if (result.state == SkinOverlayDigestPollState::Unknown ||
            (result.state == SkinOverlayDigestPollState::Ready &&
             (result.failure || !isLowerHex(result.lowercaseSha256, 64)))) {
          fail(FatalReason::OverlayDigest);
          return;
        }
        if (result.state == SkinOverlayDigestPollState::Ready) {
          beforeDigest_ = result.lowercaseSha256;
          beforeDigestTicket_.reset();
        }
      }
      if (afterDigestTicket_) {
        const auto result =
            dependencies_.overlayDigests->pollDigest(*afterDigestTicket_);
        if (result.state == SkinOverlayDigestPollState::Unknown ||
            (result.state == SkinOverlayDigestPollState::Ready &&
             (result.failure || !isLowerHex(result.lowercaseSha256, 64)))) {
          fail(FatalReason::OverlayDigest);
          return;
        }
        if (result.state == SkinOverlayDigestPollState::Ready) {
          afterDigest_ = result.lowercaseSha256;
          afterDigestTicket_.reset();
          if (!beforeDigest_ || *beforeDigest_ != *afterDigest_) {
            fail(FatalReason::OverlayDigest);
            return;
          }
          (void)beginStopAndExport();
        }
      }
    } catch (...) {
      fail(FatalReason::OverlayDigest);
    }
  }

  SkinAcceptanceExportTicket beginStopAndExport() {
    {
      std::lock_guard lock(exportMutex_);
      if (exportTicket_) {
        return *exportTicket_;
      }
    }

    if (closed_) {
      fail(FatalReason::Shutdown);
    }
    if (fatalReason_ == FatalReason::None && !validateReadyForExport()) {
      // validateReadyForExport records the precise failure.
    }

    // The terminal failure must be fully value-owned before a ticket becomes
    // observable. Every later failure path can then publish it with only
    // atomic state changes, even when allocation or thread construction has
    // already failed.
    SkinAcceptanceExportResult terminalFailure;
    terminalFailure.failure = failureDiagnostic(
        fatalReason_ == FatalReason::None ? FatalReason::ExportFailed
                                          : fatalReason_);
    const auto ticket = SkinAcceptanceExportTicket{nextExportTicketValue()};
    if (!ticket) {
      fail(FatalReason::ExportFailed);
      return {};
    }
    {
      std::lock_guard lock(exportMutex_);
      if (exportTicket_) {
        return *exportTicket_;
      }
      terminalResult_ = std::move(terminalFailure);
      terminalResultReady_.store(false, std::memory_order_release);
      exportTicket_ = ticket;
    }

    if (fatalReason_ != FatalReason::None) {
      publishTerminalFailure();
      return ticket;
    }

    std::vector<std::string> diagnosticHistoryCodes;
    try {
      if (dependencies_.snapshotDiagnostics) {
        auto diagnostics = dependencies_.snapshotDiagnostics();
        if (diagnostics.size() > kMaximumDiagnosticHistoryEntries) {
          fail(FatalReason::ExportFailed);
          publishTerminalFailure();
          return ticket;
        }
        diagnosticHistoryCodes.reserve(diagnostics.size());
        for (const auto &diagnostic : diagnostics) {
          if (isAllowedDiagnosticHistoryCode(diagnostic.code)) {
            diagnosticHistoryCodes.push_back(diagnostic.code);
          }
        }
      }
    } catch (...) {
      fail(FatalReason::ExportFailed);
      publishTerminalFailure();
      return ticket;
    }

    try {
      auto payload = serialize(diagnosticHistoryCodes);
      const auto relativePath =
          std::filesystem::path("SkinAcceptance") / (runId_ + ".json");
      const auto absolutePath = dependencies_.documentsRoot / relativePath;
      const auto digest = file_checksum::sha256(payload);
      auto writer = dependencies_.writeAtomic;
      state_.store(SkinAcceptanceCaptureState::Exporting,
                   std::memory_order_release);

      if (writerThread_.joinable()) {
        writerThread_.join();
      }
      writerThread_ = std::thread(
          [this, writer = std::move(writer), payload = std::move(payload),
           relativePath, absolutePath, digest]() mutable {
            bool exported = false;
            try {
              std::string ignoredError;
              const auto bytes =
                  std::as_bytes(std::span(payload.data(), payload.size()));
              if (writer(absolutePath, bytes, ignoredError)) {
                SkinAcceptanceExportResult result;
                result.exported = true;
                result.documentsRelativePath = std::move(relativePath);
                result.lowercaseSha256 = std::move(digest);
                {
                  std::lock_guard lock(exportMutex_);
                  terminalResult_ = std::move(result);
                }
                exported = true;
              }
            } catch (...) {
            }
            state_.store(exported ? SkinAcceptanceCaptureState::Exported
                                  : SkinAcceptanceCaptureState::Failed,
                         std::memory_order_release);
            terminalResultReady_.store(true, std::memory_order_release);
          });
    } catch (...) {
      fail(FatalReason::ExportFailed);
      publishTerminalFailure();
    }
    return ticket;
  }

  SkinAcceptanceExportPollResult
  pollExport(SkinAcceptanceExportTicket ticket) const {
    std::lock_guard lock(exportMutex_);
    if (!ticket || !exportTicket_ || ticket != *exportTicket_) {
      return {};
    }
    if (!terminalResultReady_.load(std::memory_order_acquire)) {
      return {.state = SkinAcceptanceExportPollState::Pending};
    }
    return {.state = SkinAcceptanceExportPollState::Ready,
            .result = terminalResult_};
  }

  bool acknowledgeExport(SkinAcceptanceExportTicket ticket) noexcept {
    try {
      {
        std::lock_guard lock(exportMutex_);
        if (!ticket || !exportTicket_ || ticket != *exportTicket_ ||
            !terminalResultReady_.load(std::memory_order_acquire)) {
          return false;
        }
      }
      if (writerThread_.joinable()) {
        writerThread_.join();
      }
      {
        std::lock_guard lock(exportMutex_);
        terminalResult_.reset();
        terminalResultReady_.store(false, std::memory_order_release);
        exportTicket_.reset();
      }
      resetRun();
      if (!closed_) {
        state_.store(SkinAcceptanceCaptureState::Idle,
                     std::memory_order_release);
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  std::optional<SkinAcceptanceExportTicket>
  currentExportTicket() const noexcept {
    try {
      std::lock_guard lock(exportMutex_);
      return exportTicket_;
    } catch (...) {
      return std::nullopt;
    }
  }

  SkinAcceptanceCaptureState state() const noexcept {
    return state_.load(std::memory_order_acquire);
  }

  void shutdown() noexcept {
    if (closed_) {
      return;
    }
    closed_ = true;
    try {
      if (dependencies_.overlayDigests) {
        if (beforeDigestTicket_) {
          dependencies_.overlayDigests->cancelDigest(*beforeDigestTicket_);
          beforeDigestTicket_.reset();
        }
        if (afterDigestTicket_) {
          dependencies_.overlayDigests->cancelDigest(*afterDigestTicket_);
          afterDigestTicket_.reset();
        }
      }
      if (writerThread_.joinable()) {
        writerThread_.join();
      }
    } catch (...) {
    }
    if (state_.load(std::memory_order_acquire) !=
            SkinAcceptanceCaptureState::Exported &&
        state_.load(std::memory_order_acquire) !=
            SkinAcceptanceCaptureState::Failed) {
      fatalReason_ = FatalReason::Shutdown;
      state_.store(SkinAcceptanceCaptureState::Failed,
                   std::memory_order_release);
    }
  }

private:
  bool acceptsNegativeEvidence(
      const AcceptanceBoundScopeView &boundScope) const noexcept {
    return !closed_ && contract_ &&
           contract_->kind == SkinAcceptanceRunKind::RenderIoNegative &&
           state_.load(std::memory_order_acquire) ==
               SkinAcceptanceCaptureState::Recording &&
           currentSession_ && !sessionEnded_ &&
           boundScope.recorderSerial == recorderScopeSerial_ &&
           boundScope.runSerial == currentRunScopeSerial_ &&
           boundScope.bindingSerial == currentBoundScopeSerial_ &&
           sameIdentity(boundScope.identity, *currentSession_);
  }

  void fail(FatalReason reason) noexcept {
    if (fatalReason_ == FatalReason::None) {
      fatalReason_ = reason;
    }
    state_.store(SkinAcceptanceCaptureState::Failed, std::memory_order_release);
  }

  bool validateReadyForExport() noexcept {
    if (!contract_ || !activation_ || !sessionFacts_) {
      fail(FatalReason::InvalidSession);
      return false;
    }
    if (contract_->kind == SkinAcceptanceRunKind::Performance) {
      if (!warmupStartVisualTime_ || !firstRetainedVisualTime_ ||
          !lastRetainedVisualTime_ || !expectedRecordingStartVisualTime_ ||
          !expectedRecordingEndVisualTime_ ||
          *firstRetainedVisualTime_ != *expectedRecordingStartVisualTime_ ||
          *lastRetainedVisualTime_ != *expectedRecordingEndVisualTime_) {
        fail(FatalReason::InvalidPerformance);
        return false;
      }
      const auto summary = telemetry_.summarize();
      const SkinPerformanceRunFacts runFacts{
          .configuredRefreshHz =
              static_cast<std::uint16_t>(sessionFacts_->actualRefreshHz),
          .warmupStartedVisualTimeMicros = *warmupStartVisualTime_,
          .trustedFirstRecordedVisualTimeMicros = *firstRetainedVisualTime_,
          .trustedLastRecordedVisualTimeMicros = *lastRetainedVisualTime_,
          .sessionEndedBeforeRecordingComplete = false,
          .envelopeStreamValid = fatalReason_ == FatalReason::None,
      };
      if (!validateSkinPerformanceRun(summary, runFacts).passed()) {
        fail(summary.overflowSampleCount != 0
                 ? FatalReason::SampleOverflow
                 : FatalReason::InvalidPerformance);
        return false;
      }
      return true;
    }
    if (contract_->kind == SkinAcceptanceRunKind::ResourceLifecycle) {
      if (!lifecycleBaseline_ ||
          lifecycleSampleCount_ != contract_->requiredExitCycles ||
          currentSession_ || pendingLifecycleSample_) {
        fail(FatalReason::InvalidLifecycle);
        return false;
      }
      return true;
    }
    const auto summary = telemetry_.summarize();
    if (!beforeDigest_ || !afterDigest_ || *beforeDigest_ != *afterDigest_ ||
        !sessionEnded_ || currentSession_ || !fallbackObserved_ ||
        !observedDiagnostic_ || !observedFallbackAction_ ||
        !contract_->expectedDiagnosticCode ||
        observedDiagnostic_->code != *contract_->expectedDiagnosticCode ||
        !contract_->expectedFallbackAction ||
        *observedFallbackAction_ != *contract_->expectedFallbackAction ||
        !contract_->expectedDeniedOperation ||
        !validateNegativeSkinRenderIo(summary.renderIo,
                                      *contract_->expectedDeniedOperation)) {
      fail(FatalReason::InvalidNegativeEvidence);
      return false;
    }
    return true;
  }

  std::string
  serialize(std::span<const std::string> diagnosticHistoryCodes) const {
    const auto summary = telemetry_.summarize();
    std::string output;
    output.reserve(4096);
    output += "{\"schemaVersion\":1,\"recordType\":\"skinAcceptanceScenario\","
              "\"recordId\":";
    appendQuoted(output, runId_);
    output += ",\"buildIdentity\":{\"commit\":";
    appendQuoted(output, dependencies_.buildIdentity.commit);
    output += ",\"configuration\":";
    appendQuoted(output, dependencies_.buildIdentity.configuration);
    output += ",\"sourceClean\":true},\"scenario\":{\"scenarioId\":";
    appendQuoted(output, contract_->scenarioId);
    output += ",\"chartSha256\":";
    appendQuoted(output, sessionFacts_->chartSha256);
    output += ",\"layoutId\":";
    appendQuoted(output, sessionFacts_->layoutId);
    output += ",\"configuredRefreshHz\":";
    appendUnsigned(output, sessionFacts_->actualRefreshHz);
    output += ",\"activatedRevisionSha256\":";
    appendQuoted(output, activation_->revisionDigest);
    output += ",\"configurationSha256\":";
    appendQuoted(output, activation_->configurationDigest);
    output += ",\"expectedGuardVectorSha256\":";
    appendQuoted(output, contract_->expectedOpaqueGuardVectorSha256);
    output += ",\"observedGuardVectorSha256\":";
    appendQuoted(output, sessionFacts_->observedOpaqueGuardVectorSha256);
    output += "},";

    if (contract_->kind == SkinAcceptanceRunKind::Performance) {
      const auto separator = sessionFacts_->layoutId.rfind('-');
      const auto aspect = sessionFacts_->layoutId.substr(0, separator);
      const auto mode = separator == std::string::npos
                            ? std::string{}
                            : sessionFacts_->layoutId.substr(separator + 1);
      output += "\"performanceRuns\":[{\"scenario\":";
      appendQuoted(output, contract_->scenarioId);
      output += ",\"aspect\":";
      appendQuoted(output, aspect);
      output += ",\"mode\":";
      appendQuoted(output, mode);
      output += ",\"chartSha256\":";
      appendQuoted(output, sessionFacts_->chartSha256);
      output += ",\"activatedRevisionSha256\":";
      appendQuoted(output, activation_->revisionDigest);
      output += ",\"configurationSha256\":";
      appendQuoted(output, activation_->configurationDigest);
      output += ",\"guardVectorSha256\":";
      appendQuoted(output, sessionFacts_->observedOpaqueGuardVectorSha256);
      output += ",\"warmupStartMicros\":";
      appendSigned(output, *warmupStartVisualTime_);
      output += ",\"recordingStartMicros\":";
      appendSigned(output, *firstRetainedVisualTime_);
      output += ",\"recordingEndMicros\":";
      appendSigned(output, *lastRetainedVisualTime_);
      output += ",\"configuredRefreshHz\":";
      appendUnsigned(output, sessionFacts_->actualRefreshHz);
      output += ",\"p99SkinCpuMicros\":";
      appendUnsigned(output, summary.p99SkinCpuMicros);
      output += ",\"missedPresentationPercent\":";
      appendDouble(output, summary.missedPresentationPercent);
      output += ",\"residentDriftBytes\":";
      appendSigned(output, summary.residentDriftBytes);
      output += ",\"telemetry\":{\"receivedSampleCount\":";
      appendUnsigned(output, summary.receivedSampleCount);
      output += ",\"retainedSampleCount\":";
      appendUnsigned(output, summary.retainedSampleCount);
      output += ",\"overflowSampleCount\":";
      appendUnsigned(output, summary.overflowSampleCount);
      output += ",\"incompleteSampleCount\":0,\"mismatchedSampleCount\":0},"
                "\"renderIo\":";
      appendRenderIo(output, summary.renderIo);
      output += "}]";
    } else if (contract_->kind == SkinAcceptanceRunKind::ResourceLifecycle) {
      output += "\"resourceLifecycle\":{\"baseline\":{\"liveTextures\":";
      appendUnsigned(output, lifecycleBaseline_->liveTextures);
      output += ",\"liveResources\":";
      appendUnsigned(output, lifecycleBaseline_->liveResources);
      output += ",\"residentBytes\":";
      appendUnsigned(output, lifecycleBaseline_->residentBytes);
      output += "},\"postDestruction\":[";
      for (std::size_t index = 0; index < lifecycleSampleCount_; ++index) {
        if (index != 0) {
          output.push_back(',');
        }
        output += "{\"cycle\":";
        appendUnsigned(output, lifecycleSamples_[index].cycleIndex);
        output += ",\"liveTextures\":";
        appendUnsigned(output, lifecycleSamples_[index].liveTextures);
        output += ",\"liveResources\":";
        appendUnsigned(output, lifecycleSamples_[index].liveResources);
        output += ",\"residentBytes\":";
        appendUnsigned(output, lifecycleSamples_[index].residentBytes);
        output.push_back('}');
      }
      output += "]}";
    } else {
      output += "\"negativeScenario\":{\"scenarioId\":";
      appendQuoted(output, contract_->scenarioId);
      output += ",\"activatedRevisionSha256\":";
      appendQuoted(output, activation_->revisionDigest);
      output += ",\"configurationSha256\":";
      appendQuoted(output, activation_->configurationDigest);
      output += ",\"guardVectorSha256\":";
      appendQuoted(output, sessionFacts_->observedOpaqueGuardVectorSha256);
      output += ",\"diagnostic\":";
      appendQuoted(output, observedDiagnostic_->code);
      output += ",\"action\":";
      appendQuoted(output, *observedFallbackAction_);
      output += ",\"overlayDigestBefore\":";
      appendQuoted(output, *beforeDigest_);
      output += ",\"overlayDigestAfter\":";
      appendQuoted(output, *afterDigest_);
      output += ",\"performedCounters\":";
      appendGroupedIo(output, summary.renderIo, false);
      output += ",\"deniedCounters\":";
      appendGroupedIo(output, summary.renderIo, true);
      output += ",\"observedDiagnosticCodes\":[";
      appendQuoted(output, observedDiagnostic_->code);
      output += "]}";
    }
    output += ",\"diagnosticHistoryCodes\":[";
    for (std::size_t index = 0; index < diagnosticHistoryCodes.size();
         ++index) {
      if (index != 0) {
        output.push_back(',');
      }
      appendQuoted(output, diagnosticHistoryCodes[index]);
    }
    output += "]}";
    return output;
  }

  void publishTerminalFailure() noexcept {
    state_.store(SkinAcceptanceCaptureState::Failed, std::memory_order_release);
    terminalResultReady_.store(true, std::memory_order_release);
  }

  void resetRun() noexcept {
    contract_.reset();
    activation_.reset();
    sessionFacts_.reset();
    currentSession_.reset();
    currentRunScopeSerial_ = 0;
    currentBoundScopeSerial_ = 0;
    runId_.clear();
    telemetry_ = SkinPerformanceTelemetry{};
    retainedFrameCount_ = 0;
    previousFrameSerial_ = 0;
    previousVisualTime_.reset();
    warmupStartVisualTime_.reset();
    expectedRecordingStartVisualTime_.reset();
    expectedRecordingEndVisualTime_.reset();
    firstRetainedVisualTime_.reset();
    lastRetainedVisualTime_.reset();
    lifecycleBaseline_.reset();
    lifecycleSamples_ = {};
    lifecycleSampleCount_ = 0;
    pendingLifecycleSample_.reset();
    lastLifecycleSessionSerial_ = 0;
    beforeDigestTicket_.reset();
    afterDigestTicket_.reset();
    beforeDigest_.reset();
    afterDigest_.reset();
    boundOnce_ = false;
    sessionEnded_ = false;
    fallbackObserved_ = false;
    observedDiagnostic_.reset();
    observedFallbackAction_.reset();
    fatalReason_ = FatalReason::None;
  }

  SkinAcceptanceRecorderDependencies dependencies_;
  std::atomic<SkinAcceptanceCaptureState> state_{
      SkinAcceptanceCaptureState::Idle};
  bool closed_ = false;
  FatalReason fatalReason_ = FatalReason::None;

  std::string runId_;
  std::optional<SkinAcceptanceScenarioContract> contract_;
  std::optional<SkinAcceptanceActivationKey> activation_;
  std::optional<SkinAcceptanceSessionFacts> sessionFacts_;
  std::optional<PlaySkinSessionIdentity> currentSession_;
  bool boundOnce_ = false;
  bool sessionEnded_ = false;
  const std::uint64_t recorderScopeSerial_ = 0;
  std::uint64_t currentRunScopeSerial_ = 0;
  std::uint64_t currentBoundScopeSerial_ = 0;

  SkinPerformanceTelemetry telemetry_;
  std::size_t retainedFrameCount_ = 0;
  std::uint64_t previousFrameSerial_ = 0;
  std::optional<std::int64_t> previousVisualTime_;
  std::optional<std::int64_t> warmupStartVisualTime_;
  std::optional<std::int64_t> expectedRecordingStartVisualTime_;
  std::optional<std::int64_t> expectedRecordingEndVisualTime_;
  std::optional<std::int64_t> firstRetainedVisualTime_;
  std::optional<std::int64_t> lastRetainedVisualTime_;
  bool fallbackObserved_ = false;
  std::optional<SkinDiagnostic> observedDiagnostic_;
  std::optional<std::string> observedFallbackAction_;

  std::optional<SkinResourceLifecycleSample> lifecycleBaseline_;
  std::array<SkinResourceLifecycleSample, kLifecycleCycles> lifecycleSamples_{};
  std::size_t lifecycleSampleCount_ = 0;
  std::optional<SkinResourceLifecycleSample> pendingLifecycleSample_;
  std::uint64_t lastLifecycleSessionSerial_ = 0;

  std::optional<SkinOverlayDigestTicket> beforeDigestTicket_;
  std::optional<SkinOverlayDigestTicket> afterDigestTicket_;
  std::optional<std::string> beforeDigest_;
  std::optional<std::string> afterDigest_;

  mutable std::mutex exportMutex_;
  std::optional<SkinAcceptanceExportTicket> exportTicket_;
  std::optional<SkinAcceptanceExportResult> terminalResult_;
  std::atomic<bool> terminalResultReady_{false};
  std::thread writerThread_;
};

SkinAcceptanceRecorder::SkinAcceptanceRecorder(
    SkinAcceptanceRecorderDependencies dependencies)
    : impl_(std::make_unique<Impl>(std::move(dependencies))) {}

SkinAcceptanceRecorder::~SkinAcceptanceRecorder() { shutdown(); }

bool SkinAcceptanceRecorder::arm(std::string opaqueRunId,
                                 std::string scenarioId,
                                 SkinAcceptanceActivationKey activation) {
  return impl_->arm(std::move(opaqueRunId), std::move(scenarioId),
                    std::move(activation));
}

std::optional<SkinAcceptanceBoundScope>
SkinAcceptanceRecorder::bindSession(const SkinAcceptanceSessionFacts &facts) {
  auto boundScope = impl_->bindSession(facts);
  if (!boundScope) {
    return std::nullopt;
  }
  return SkinAcceptanceBoundScope(
      boundScope->recorderSerial, boundScope->runSerial,
      boundScope->bindingSerial, std::move(boundScope->identity));
}

void SkinAcceptanceRecorder::record(
    SkinFrameTelemetryEnvelope &&envelope) noexcept {
  impl_->record(std::move(envelope));
}

void SkinAcceptanceRecorder::recordDiagnosticEvidence(
    const SkinAcceptanceBoundScope &boundScope,
    SkinDiagnostic diagnostic) noexcept {
  impl_->recordDiagnosticEvidence(
      {.recorderSerial = boundScope.recorderSerial_,
       .runSerial = boundScope.runSerial_,
       .bindingSerial = boundScope.bindingSerial_,
       .identity = boundScope.identity_},
      std::move(diagnostic));
}

void SkinAcceptanceRecorder::recordFallbackAction(
    const SkinAcceptanceBoundScope &boundScope, std::string action) noexcept {
  impl_->recordFallbackAction(
      {.recorderSerial = boundScope.recorderSerial_,
       .runSerial = boundScope.runSerial_,
       .bindingSerial = boundScope.bindingSerial_,
       .identity = boundScope.identity_},
      std::move(action));
}

void SkinAcceptanceRecorder::recordResourceLifecycle(
    SkinResourceLifecycleSample sample) noexcept {
  impl_->recordResourceLifecycle(sample);
}

void SkinAcceptanceRecorder::sessionEnded(
    const PlaySkinSessionIdentity &identity) noexcept {
  impl_->sessionEnded(identity);
}

void SkinAcceptanceRecorder::sessionTeardownComplete(
    const PlaySkinSessionIdentity &identity) noexcept {
  impl_->sessionTeardownComplete(identity);
}

void SkinAcceptanceRecorder::pollAsyncDependencies() noexcept {
  impl_->pollAsyncDependencies();
}

SkinAcceptanceExportTicket SkinAcceptanceRecorder::beginStopAndExport() {
  return impl_->beginStopAndExport();
}

SkinAcceptanceExportPollResult
SkinAcceptanceRecorder::pollExport(SkinAcceptanceExportTicket ticket) const {
  return impl_->pollExport(ticket);
}

bool SkinAcceptanceRecorder::acknowledgeExport(
    SkinAcceptanceExportTicket ticket) noexcept {
  return impl_->acknowledgeExport(ticket);
}

std::optional<SkinAcceptanceExportTicket>
SkinAcceptanceRecorder::currentExportTicket() const noexcept {
  return impl_->currentExportTicket();
}

SkinAcceptanceCaptureState SkinAcceptanceRecorder::state() const noexcept {
  return impl_->state();
}

void SkinAcceptanceRecorder::shutdown() noexcept {
  if (impl_) {
    impl_->shutdown();
  }
}

} // namespace skin
