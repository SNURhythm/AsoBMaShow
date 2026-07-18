#pragma once

#include "IrRankingModels.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

class OverlayPortal;

namespace ir {

class IrRankingService;

enum class IrRankingModalState {
  Loading,
  Success,
  Empty,
  NotFound,
  AuthenticationRequired,
  TransientFailure,
  Unsupported,
  Malformed,
  Oversized,
  Cancelled,
};

struct IrRankingRowPresentation {
  std::string rankText;
  std::string playerText;
  std::string scoreText;
  std::string rateText;
  std::string lampText;
  std::string badPointsText;
  std::string maxComboText;
  std::string achievementTimeText;
  std::string detailText;
  int clearType = kClearTypeFailedRank;
  bool highlighted = false;
  bool compact = false;
  bool expanded = false;
  bool showBadPoints = false;
  bool showMaxCombo = false;
  bool showAchievementTime = false;
};

struct IrRankingScoreDetailPresentation {
  std::string rankText;
  std::string playerText;
  std::string scoreText;
  std::string rateText;
  std::string lampText;
  std::string earlyPGreatText;
  std::string latePGreatText;
  std::string earlyGreatText;
  std::string lateGreatText;
  std::string badPointsText;
  std::string maxComboText;
  std::string achievementTimeText;
  int clearType = kClearTypeFailedRank;
  bool highlighted = false;
};

struct IrRankingModalPresentation {
  IrRankingModalState state = IrRankingModalState::Loading;
  std::string chartTitle;
  std::string statusText = "Loading rankings...";
  std::string detailText;
  std::string fetchedAtText;
  bool canRefresh = false;
  bool canRetry = false;
  bool comparisonInLeaderboard = false;
  int entryCount = 0;
  std::uint64_t revision = 0;
  std::uint64_t generation = 0;
  std::optional<IrLocalComparison> comparison;
  std::shared_ptr<const IrChartRanking> ranking;
};

struct IrRankingPanelLayoutInput {
  int viewportWidth = 0;
  int viewportHeight = 0;
  int safeTop = 0;
  int safeLeft = 0;
  int safeBottom = 0;
  int safeRight = 0;
  int margin = 24;
  int maximumWidth = 1180;
  int maximumHeight = 840;
};

struct IrRankingPanelGeometry {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool compact = false;
};

class IrRankingModalModel {
public:
  void open(IrRankingRequest request, std::string chartTitle);
  void refresh(std::uint64_t generation);
  [[nodiscard]] bool apply(const IrRankingSnapshot &snapshot);
  void toggleExpanded(int index);

  [[nodiscard]] const IrRankingModalPresentation &presentation() const {
    return presentation_;
  }
  [[nodiscard]] const std::optional<IrRankingRequest> &expectedRequest() const {
    return expectedRequest_;
  }
  [[nodiscard]] IrRankingRowPresentation row(int index, int width) const;
  [[nodiscard]] std::optional<IrRankingScoreDetailPresentation>
  scoreDetail(int index) const;

private:
  std::optional<IrRankingRequest> expectedRequest_;
  IrRankingModalPresentation presentation_;
  std::optional<int> expandedIndex_;
};

class IrRankingModal {
public:
  IrRankingModal(OverlayPortal &portal, IrRankingService &service);
  ~IrRankingModal();

  IrRankingModal(const IrRankingModal &) = delete;
  IrRankingModal &operator=(const IrRankingModal &) = delete;

  void open(IrRankingRequest request, std::string chartTitle);
  void update();
  void close();
  [[nodiscard]] bool isOpen() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string formatIrRankingRate(int score, int maxScore);
[[nodiscard]] std::string
formatIrRankingTimestamp(std::optional<std::int64_t> unixMillis);
[[nodiscard]] IrChartQueryBuildOutcome
makeBokutachiRankingQuery(const bms_parser::ChartMeta &meta) noexcept;
[[nodiscard]] IrRankingPanelGeometry
layoutIrRankingPanel(const IrRankingPanelLayoutInput &input) noexcept;

} // namespace ir
