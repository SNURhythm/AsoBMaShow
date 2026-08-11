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
         .pGreat = 930,
         .great = 40,
         .good = 20,
         .bad = 6,
         .poor = 4,
         .earlyPGreat = 430,
         .latePGreat = 500,
         .earlyGreat = 18,
         .lateGreat = 22,
         .earlyGood = 12,
         .lateGood = 8,
         .earlyBad = 4,
         .lateBad = 2,
         .earlyPoor = 1,
         .latePoor = 3,
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

void testResponsiveRowsKeepFixedHeightCoreFields() {
  ir::IrRankingModalModel model;
  model.open(request(), "Test Chart");
  REQUIRE(model.apply(snapshot(ir::IrRankingSnapshotState::Succeeded)));

  const auto wide = model.row(0, 1100);
  REQUIRE(!wide.compact);
  REQUIRE(wide.showBadPoints);
  REQUIRE(wide.showMaxCombo);
  REQUIRE(wide.showAchievementTime);

  const auto constrained = model.row(0, 900);
  REQUIRE(constrained.compact);
  REQUIRE(!constrained.showBadPoints);
  REQUIRE(!constrained.showMaxCombo);
  REQUIRE(!constrained.showAchievementTime);

  auto compact = model.row(0, 560);
  REQUIRE(compact.compact);
  REQUIRE(!compact.showBadPoints);
  REQUIRE(!compact.showMaxCombo);
  REQUIRE(!compact.showAchievementTime);
  REQUIRE(!compact.rankText.empty());
  REQUIRE(!compact.playerText.empty());
  REQUIRE(!compact.rateText.empty());
  REQUIRE(!compact.lampText.empty());
}

void testScoreDetailFormatsCompleteAndMissingData() {
  ir::IrRankingModalModel model;
  model.open(request(), "Test Chart");
  REQUIRE(model.apply(snapshot(ir::IrRankingSnapshotState::Succeeded)));

  const auto detail = model.scoreDetail(0);
  REQUIRE(detail.has_value());
  REQUIRE(detail->rankText == "#1");
  REQUIRE(detail->playerText == "AAA");
  REQUIRE(detail->scoreText == "1900 / 2000");
  REQUIRE(detail->rateText == "95.00%");
  REQUIRE(detail->lampText == "FULL COMBO");
  REQUIRE(detail->totalPGreatText == "930");
  REQUIRE(detail->totalGreatText == "40");
  REQUIRE(detail->totalGoodText == "20");
  REQUIRE(detail->totalBadText == "6");
  REQUIRE(detail->totalPoorText == "4");
  REQUIRE(detail->earlyPGreatText == "430");
  REQUIRE(detail->latePGreatText == "500");
  REQUIRE(detail->earlyGreatText == "18");
  REQUIRE(detail->lateGreatText == "22");
  REQUIRE(detail->earlyGoodText == "12");
  REQUIRE(detail->lateGoodText == "8");
  REQUIRE(detail->earlyBadText == "4");
  REQUIRE(detail->lateBadText == "2");
  REQUIRE(detail->earlyPoorText == "1");
  REQUIRE(detail->latePoorText == "3");
  REQUIRE(detail->judgementBreakdownAvailable);
  REQUIRE(detail->badPointsText == "0");
  REQUIRE(detail->maxComboText == "1000");
  REQUIRE(detail->achievementTimeText != "\xE2\x80\x94");
  REQUIRE(detail->clearType == kClearTypeFullComboRank);
  REQUIRE(!detail->highlighted);

  const auto missing = model.scoreDetail(1);
  REQUIRE(missing.has_value());
  REQUIRE(missing->totalPGreatText == "\xE2\x80\x94");
  REQUIRE(missing->totalGreatText == "\xE2\x80\x94");
  REQUIRE(missing->totalGoodText == "\xE2\x80\x94");
  REQUIRE(missing->totalBadText == "\xE2\x80\x94");
  REQUIRE(missing->totalPoorText == "\xE2\x80\x94");
  REQUIRE(missing->earlyPGreatText == "\xE2\x80\x94");
  REQUIRE(missing->latePGreatText == "\xE2\x80\x94");
  REQUIRE(missing->earlyGreatText == "\xE2\x80\x94");
  REQUIRE(missing->lateGreatText == "\xE2\x80\x94");
  REQUIRE(missing->earlyGoodText == "\xE2\x80\x94");
  REQUIRE(missing->lateGoodText == "\xE2\x80\x94");
  REQUIRE(missing->earlyBadText == "\xE2\x80\x94");
  REQUIRE(missing->lateBadText == "\xE2\x80\x94");
  REQUIRE(missing->earlyPoorText == "\xE2\x80\x94");
  REQUIRE(missing->latePoorText == "\xE2\x80\x94");
  REQUIRE(!missing->judgementBreakdownAvailable);
  REQUIRE(missing->badPointsText == "\xE2\x80\x94");
  REQUIRE(missing->maxComboText == "\xE2\x80\x94");
  REQUIRE(missing->achievementTimeText == "\xE2\x80\x94");
  REQUIRE(missing->highlighted);

  REQUIRE(!model.scoreDetail(-1).has_value());
  REQUIRE(!model.scoreDetail(2).has_value());

  auto totalsOnlySnapshot = snapshot(ir::IrRankingSnapshotState::Succeeded);
  auto totalsOnlyRanking =
      std::make_shared<ir::IrChartRanking>(*totalsOnlySnapshot.ranking);
  totalsOnlyRanking->entries.front().earlyPGreat.reset();
  totalsOnlyRanking->entries.front().latePGreat.reset();
  totalsOnlyRanking->entries.front().earlyGreat.reset();
  totalsOnlyRanking->entries.front().lateGreat.reset();
  totalsOnlyRanking->entries.front().earlyGood.reset();
  totalsOnlyRanking->entries.front().lateGood.reset();
  totalsOnlyRanking->entries.front().earlyBad.reset();
  totalsOnlyRanking->entries.front().lateBad.reset();
  totalsOnlyRanking->entries.front().earlyPoor.reset();
  totalsOnlyRanking->entries.front().latePoor.reset();
  totalsOnlySnapshot.ranking = totalsOnlyRanking;
  ir::IrRankingModalModel totalsOnly;
  totalsOnly.open(request(), "Test Chart");
  REQUIRE(totalsOnly.apply(totalsOnlySnapshot));
  REQUIRE(totalsOnly.scoreDetail(0)->judgementBreakdownAvailable);
}

void testPaginationPresentationKeepsSuccessfulListVisible() {
  ir::IrRankingModalModel model;
  model.open(request(), "Test Chart");
  auto loading = snapshot(ir::IrRankingSnapshotState::Succeeded, 1);
  auto page = std::make_shared<ir::IrChartRanking>(*loading.ranking);
  page->nextPageToken = "page-2";
  loading.ranking = page;
  loading.loadingNextPage = true;
  REQUIRE(model.apply(loading));
  REQUIRE(model.presentation().state == ir::IrRankingModalState::Success);
  REQUIRE(model.presentation().entryCount == 2);
  REQUIRE(model.presentation().loadingNextPage);
  REQUIRE(!model.presentation().canLoadNextPage);
  REQUIRE(model.presentation().paginationStatusText ==
          "Loading more rankings...");

  auto blocked = loading;
  blocked.revision = 2;
  blocked.loadingNextPage = false;
  blocked.paginationBlocked = true;
  blocked.diagnostic = "offline";
  REQUIRE(model.apply(blocked));
  REQUIRE(model.presentation().state == ir::IrRankingModalState::Success);
  REQUIRE(model.presentation().entryCount == 2);
  REQUIRE(model.presentation().paginationBlocked);
  REQUIRE(!model.presentation().canLoadNextPage);
  REQUIRE(model.presentation().detailText == "offline");
  REQUIRE(model.presentation().paginationStatusText.find("offline") !=
          std::string::npos);
  REQUIRE(model.presentation().paginationStatusText.find("Refresh") !=
          std::string::npos);
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

void testVirtualizedPaginationThresholdAndScrollRetention() {
  REQUIRE(!ir::shouldLoadNextIrRankingPage(100, 0.0f, 600.0f, 60, 10));
  REQUIRE(ir::shouldLoadNextIrRankingPage(100, 4'900.0f, 600.0f, 60, 10));
  REQUIRE(ir::shouldLoadNextIrRankingPage(5, 0.0f, 600.0f, 60, 10));
  REQUIRE(!ir::shouldLoadNextIrRankingPage(0, 0.0f, 600.0f, 60, 10));
  REQUIRE(!ir::shouldLoadNextIrRankingPage(100, -1.0f, 600.0f, 60, 10));

  std::vector<int> entries(100);
  RecyclerView<int> recycler(
      [](const int left, const int right) { return left == right; });
  recycler.setWidth(800)->setHeight(600)->applyYogaLayout();
  recycler.itemHeight = 60;
  recycler.onCreateView = [](const int &) { return new View(); };
  recycler.onBind = [](View *, const int &, int, bool) {};
  recycler.setItemProvider(50,
                           [&](int index) -> const int & { return entries[index]; });
  recycler.scrollOffset = 1'800.0f;
  recycler.updateItemProvider(
      100, [&](int index) -> const int & { return entries[index]; });
  REQUIRE(recycler.scrollOffset == 1'800.0f);
  REQUIRE(recycler.size() == 100);
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

void testRecyclerIgnoresMouseSynthesizedTouchSelection() {
  RecyclerView<int> recycler(
      [](const int left, const int right) { return left == right; });
  recycler.setWidth(800)->setHeight(200)->applyYogaLayout();
  recycler.itemHeight = 64;
  recycler.onCreateView = [](const int &) { return new View(); };
  recycler.onBind = [](View *, const int &, int, bool) {};
  recycler.setItems(std::vector<int>{1, 2});
  int selectionCount = 0;
  recycler.onSelected = [&](const int &, int) { ++selectionCount; };

  SDL_Event down{};
  down.type = SDL_FINGERDOWN;
  down.tfinger.type = SDL_FINGERDOWN;
  down.tfinger.touchId = SDL_MOUSE_TOUCHID;
  down.tfinger.fingerId = 0;
  down.tfinger.x = 0.005F;
  down.tfinger.y = 0.01F;
  SDL_Event up = down;
  up.type = SDL_FINGERUP;
  up.tfinger.type = SDL_FINGERUP;
  recycler.handleEvents(down);
  recycler.handleEvents(up);

  REQUIRE(selectionCount == 0);
  REQUIRE(recycler.selectedIndex == -1);
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
  testResponsiveRowsKeepFixedHeightCoreFields();
  testScoreDetailFormatsCompleteAndMissingData();
  testPaginationPresentationKeepsSuccessfulListVisible();
  testTwentyThousandEntriesCreateOnlyVisibleRows();
  testVirtualizedPaginationThresholdAndScrollRetention();
  testRecyclerBindingSeesAppliedRowWidth();
  testRecyclerIgnoresMouseSynthesizedTouchSelection();
  testBokutachiEligibilityRequiresSupportedModeNotesAndSha256();
  return 0;
}
