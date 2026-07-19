#pragma once

#include "ir/IrProfileSettings.h"
#include "repositories/ReplayRepository.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

struct LocalReplayRecordId {
  int replayId = 0;

  bool operator==(const LocalReplayRecordId &) const = default;
};

struct IrRemoteRecordId {
  std::string providerId;
  std::string serverOrigin;
  std::string remoteScoreId;

  bool operator==(const IrRemoteRecordId &) const = default;
};

using ResultRecordIdentity =
    std::variant<LocalReplayRecordId, IrRemoteRecordId>;

struct ResultRecordIdentityHash {
  [[nodiscard]] std::size_t
  operator()(const ResultRecordIdentity &identity) const noexcept;
};

struct ResultRecordCapabilities {
  bool watch = false;
  bool gBattle = false;
  bool resultRecall = false;
  bool videoExport = false;
  bool irUpload = false;

  bool operator==(const ResultRecordCapabilities &) const = default;
};

// Stable keys use length-framed, validated identity components. This bound
// keeps every factory-produced key suitable for RecyclerView identity use
// without introducing collision-prone hashes or sentinel replay IDs.
inline constexpr std::size_t kMaximumResultRecordStableKeyBytes =
    ir::kMaximumIrProviderIdBytes + ir::kMaximumServerOriginBytes +
    ir::kMaximumIrRemoteScoreIdBytes +
    3 * (std::numeric_limits<std::size_t>::digits10 + 1) + 16;

struct ResultRecordSummary {
  ResultRecordIdentity identity;
  ResultRecordCapabilities capabilities;
  bool course = false;
  bool autoPlay = false;
  int score = 0;
  int maxScore = 0;
  std::optional<int> maxCombo;
  int clearRank = kClearTypeFailedRank;
  std::int64_t displayedTimeUnixMillis = 0;
  std::string displayedTime;
  std::optional<std::string> playOption;
  ir::IrRecordState irState = ir::IrRecordState::Hidden;
  std::optional<ReplaySummary> local;
  std::optional<ir::IrRemoteScore> remote;

  [[nodiscard]] bool isLocal() const noexcept;
  [[nodiscard]] bool isRemote() const noexcept;
  [[nodiscard]] std::optional<int> localReplayId() const noexcept;
  [[nodiscard]] std::optional<std::string_view>
  remoteScoreId() const noexcept;
  [[nodiscard]] std::string stableKey() const;
};

[[nodiscard]] ResultRecordSummary
makeLocalResultRecord(ReplaySummary summary);

// Throws std::invalid_argument when the provider/origin identity is not
// canonical or when the copied remote score fails stored-model validation.
[[nodiscard]] ResultRecordSummary makeRemoteResultRecord(
    std::string_view providerId, std::string_view serverOrigin,
    ir::IrRemoteScore score);
