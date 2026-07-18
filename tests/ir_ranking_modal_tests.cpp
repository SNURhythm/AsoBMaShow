#include "ir/IrRankingModal.h"
#include "view/RecyclerView.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0f;
float heightScale = 1.0f;
float ui_scale_x = 1.0f;
float ui_scale_y = 1.0f;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {

#define REQUIRE(condition) require((condition), #condition, __LINE__)

void require(bool condition, const char *expression, int line) {
  if (condition) {
    return;
  }
  std::cerr << "requirement failed at line " << line << ": " << expression
            << '\n';
  std::exit(1);
}

ir::IrRankingRequest request(std::uint64_t generation = 7) {
  return {
      .generation = generation,
      .profileId = "profile-a",
      .providerId = "tachi",
      .serverOrigin = "https://boku.tachi.ac",
      .chart = {.keyMode = 7,
                .chartSha256 = std::string(64, 'a'),
                .totalNotes = 1000},
      .localComparison =
          ir::IrLocalComparison{.label = "Local PB",
                                .score = 1700,
                                .maxScore = 2000,
                                .clearType = kClearTypeHardClearRank,
                                .badPoints = 15,
                                .maxCombo = 731},
  };
}

std::shared_ptr<const ir::IrChartRanking> ranking(bool includeEntries = true) {
  auto value = std::make_shared<ir::IrChartRanking>();
  value->providerId = "tachi";
  value->chart = request().chart;
  value->fetchedAtUnixMillis = 1'700'000'000'000LL;
  if (includeEntries) {
    value->entries = {
        {.rank = 1,
         .playerName = "AAA",
         .score = 1900,
         .maxScore = 2000,
         .clearType = kClearTypeFullComboRank,
         .badPoints = 0,
         .maxCombo = 1000,
         .achievedAtUnixMillis = 1'700'000'000'000LL},
        {.rank = 2,
         .playerName = "PLAYER",
         .score = 1750,
         .maxScore = 2000,
         .clearType = kClearTypeHardClearRank,
         .currentUser = true},
    };
  }
  return value;
}

ir::IrRankingSnapshot snapshot(ir::IrRankingSnapshotState state,
                               std::uint64_t revision = 1) {
  return {.revision = revision,
          .generation = 7,
          .state = state,
          .request = request(),
          .ranking = state == ir::IrRankingSnapshotState::Succeeded ? ranking()
                                                                    : nullptr,
          .diagnostic = "safe detail"};
}

void testModalStateMappingAndActions() {
  struct Expectation {
    ir::IrRankingSnapshotState snapshotState;
    ir::IrRankingModalState modalState;
    bool canRefresh;
    bool canRetry;
  };
  const Expectation expectations[] = {
      {ir::IrRankingSnapshotState::Loading, ir::IrRankingModalState::Loading,
       false, false},
      {ir::IrRankingSnapshotState::ChartNotFound,
       ir::IrRankingModalState::NotFound, true, true},
      {ir::IrRankingSnapshotState::AuthenticationRequired,
       ir::IrRankingModalState::AuthenticationRequired, true, true},
      {ir::IrRankingSnapshotState::TransientFailure,
       ir::IrRankingModalState::TransientFailure, true, true},
      {ir::IrRankingSnapshotState::Unsupported,
       ir::IrRankingModalState::Unsupported, true, false},
      {ir::IrRankingSnapshotState::MalformedResponse,
       ir::IrRankingModalState::Malformed, true, true},
      {ir::IrRankingSnapshotState::OversizedResponse,
       ir::IrRankingModalState::Oversized, true, true},
      {ir::IrRankingSnapshotState::Cancelled,
       ir::IrRankingModalState::Cancelled, true, true},
  };

  for (const auto &expectation : expectations) {
    ir::IrRankingModalModel model;
    model.open(request(), "Test Chart");
    REQUIRE(model.apply(snapshot(expectation.snapshotState)));
    const auto &presentation = model.presentation();
    REQUIRE(presentation.state == expectation.modalState);
    REQUIRE(presentation.canRefresh == expectation.canRefresh);
    REQUIRE(presentation.canRetry == expectation.canRetry);
    REQUIRE(presentation.comparison.has_value());
    REQUIRE(!presentation.comparisonInLeaderboard);
  }

  ir::IrRankingModalModel success;
  success.open(request(), "Test Chart");
  REQUIRE(success.apply(snapshot(ir::IrRankingSnapshotState::Succeeded)));
  REQUIRE(success.presentation().state == ir::IrRankingModalState::Success);
  REQUIRE(success.presentation().entryCount == 2);
  REQUIRE(success.presentation().canRefresh);
  REQUIRE(!success.presentation().canRetry);

  auto emptySnapshot = snapshot(ir::IrRankingSnapshotState::Succeeded);
  emptySnapshot.ranking = ranking(false);
  ir::IrRankingModalModel empty;
  empty.open(request(), "Test Chart");
  REQUIRE(empty.apply(emptySnapshot));
  REQUIRE(empty.presentation().state == ir::IrRankingModalState::Empty);
  REQUIRE(empty.presentation().canRefresh);
  REQUIRE(empty.presentation().canRetry);
}

void testFullRequestIdentityAndRefreshGenerationGuard() {
  ir::IrRankingModalModel model;
  model.open(request(), "Original");

  auto wrongChart = snapshot(ir::IrRankingSnapshotState::Succeeded);
  wrongChart.request->chart.chartSha256 = std::string(64, 'b');
  REQUIRE(!model.apply(wrongChart));
  REQUIRE(model.presentation().state == ir::IrRankingModalState::Loading);

  auto wrongProfile = snapshot(ir::IrRankingSnapshotState::Succeeded);
  wrongProfile.request->profileId = "profile-b";
  REQUIRE(!model.apply(wrongProfile));

  model.refresh(8);
  REQUIRE(model.presentation().state == ir::IrRankingModalState::Loading);
  REQUIRE(model.expectedRequest()->generation == 8);
  REQUIRE(!model.apply(snapshot(ir::IrRankingSnapshotState::Succeeded, 2)));

  auto refreshed = snapshot(ir::IrRankingSnapshotState::Succeeded, 3);
  refreshed.generation = 8;
  refreshed.request = request(8);
  REQUIRE(model.apply(refreshed));
  REQUIRE(model.presentation().generation == 8);
}

void testComparisonStaysSeparateAndYouEntryIsHighlighted() {
  ir::IrRankingModalModel model;
  model.open(request(), "Test Chart");
  REQUIRE(model.apply(snapshot(ir::IrRankingSnapshotState::Succeeded)));
  REQUIRE(model.presentation().comparison->label == "Local PB");
  REQUIRE(model.presentation().ranking->entries.size() == 2);

  const auto first = model.row(0, 1200);
  REQUIRE(!first.highlighted);
  REQUIRE(first.rankText == "#1");
  REQUIRE(first.rateText == "95.00%");
  REQUIRE(first.lampText == "FULL COMBO");
  REQUIRE(first.badPointsText == "0");
  REQUIRE(first.maxComboText == "1000");

  const auto you = model.row(1, 1200);
  REQUIRE(you.highlighted);
  REQUIRE(you.playerText.find("You") != std::string::npos);
  REQUIRE(you.badPointsText == "\xE2\x80\x94");
  REQUIRE(you.maxComboText == "\xE2\x80\x94");
}

void testResponsiveRowsKeepCoreFieldsAndExpandCompactDetails() {
  ir::IrRankingModalModel model;
  model.open(request(), "Test Chart");
  REQUIRE(model.apply(snapshot(ir::IrRankingSnapshotState::Succeeded)));

  const auto wide = model.row(0, 900);
  REQUIRE(!wide.compact);
  REQUIRE(wide.showBadPoints);
  REQUIRE(wide.showMaxCombo);
  REQUIRE(wide.showAchievementTime);

  auto compact = model.row(0, 560);
  REQUIRE(compact.compact);
  REQUIRE(!compact.expanded);
  REQUIRE(!compact.showBadPoints);
  REQUIRE(!compact.showMaxCombo);
  REQUIRE(!compact.showAchievementTime);
  REQUIRE(!compact.rankText.empty());
  REQUIRE(!compact.playerText.empty());
  REQUIRE(!compact.rateText.empty());
  REQUIRE(!compact.lampText.empty());

  model.toggleExpanded(0);
  compact = model.row(0, 560);
  REQUIRE(compact.expanded);
  REQUIRE(compact.showBadPoints);
  REQUIRE(compact.showMaxCombo);
  REQUIRE(compact.showAchievementTime);
  REQUIRE(compact.detailText.find("BP 0") != std::string::npos);
  model.toggleExpanded(0);
  REQUIRE(!model.row(0, 560).expanded);
}

void testTwentyThousandEntriesCreateOnlyVisibleRows() {
  std::vector<ir::IrChartRankingEntry> entries(20'000);
  for (int index = 0; index < static_cast<int>(entries.size()); ++index) {
    entries[index].rank = index + 1;
  }

  RecyclerView<ir::IrChartRankingEntry> recycler(
      [](const auto &left, const auto &right) {
        return left.rank == right.rank;
      });
  recycler.setWidth(800)->setHeight(600)->applyYogaLayout();
  recycler.itemHeight = 64;
  recycler.topMargin = 1;
  recycler.bottomMargin = 1;
  int createdRows = 0;
  recycler.onCreateView = [&](const auto &) {
    ++createdRows;
    return new View();
  };
  recycler.onBind = [](View *, const auto &, int, bool) {};
  recycler.setItemProvider(
      static_cast<int>(entries.size()),
      [&](int index) -> const auto & { return entries[index]; });

  REQUIRE(recycler.size() == 20'000);
  REQUIRE(createdRows > 0);
  REQUIRE(createdRows <= 13);
  REQUIRE(recycler.getViewByIndex(0) != nullptr);
  REQUIRE(recycler.getViewByIndex(15'000) == nullptr);
}

void testRecyclerBindingSeesAppliedRowWidth() {
  RecyclerView<int> recycler(
      [](const int left, const int right) { return left == right; });
  recycler.setWidth(800)->setHeight(200)->applyYogaLayout();
  recycler.itemHeight = 64;
  int boundWidth = -1;
  recycler.onCreateView = [](const int &) { return new View(); };
  recycler.onBind = [&](View *view, const int &, int, bool) {
    boundWidth = view->getWidth();
  };
  recycler.setItems(std::vector<int>{1});

  REQUIRE(boundWidth == 800);
  REQUIRE(recycler.getViewByIndex(0) != nullptr);
  REQUIRE(recycler.getViewByIndex(0)->getWidth() == 800);
}

void testBokutachiEligibilityRequiresSupportedModeNotesAndSha256() {
  bms_parser::ChartMeta meta;
  meta.KeyMode = 7;
  meta.TotalNotes = 1000;
  meta.SHA256 = "  " + std::string(64, 'A') + "\n";
  const auto normalized = ir::makeBokutachiRankingQuery(meta);
  REQUIRE(normalized.value.has_value());
  REQUIRE(normalized.value->chartSha256 == std::string(64, 'a'));

  meta.KeyMode = 5;
  REQUIRE(!ir::makeBokutachiRankingQuery(meta).value.has_value());
  meta.KeyMode = 14;
  REQUIRE(ir::makeBokutachiRankingQuery(meta).value.has_value());
  meta.TotalNotes = 0;
  REQUIRE(!ir::makeBokutachiRankingQuery(meta).value.has_value());
  meta.TotalNotes = 1000;
  meta.SHA256.clear();
  meta.MD5 = std::string(32, 'b');
  REQUIRE(!ir::makeBokutachiRankingQuery(meta).value.has_value());
}

} // namespace

int main() {
  testModalStateMappingAndActions();
  testFullRequestIdentityAndRefreshGenerationGuard();
  testComparisonStaysSeparateAndYouEntryIsHighlighted();
  testResponsiveRowsKeepCoreFieldsAndExpandCompactDetails();
  testTwentyThousandEntriesCreateOnlyVisibleRows();
  testRecyclerBindingSeesAppliedRowWidth();
  testBokutachiEligibilityRequiresSupportedModeNotesAndSha256();
  return 0;
}
