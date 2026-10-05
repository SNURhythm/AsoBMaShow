#include "ir/IrRankingModal.h"
#include "i18n/Localization.h"
#include "view/RecyclerView.h"
#include "view/Button.h"
#include "view/TextView.h"
#include "view/ClearLampColors.h"
#include "view/IconText.h"
#include "view/UiTheme.h"
#include "rendering/UniformCache.h"
#include "targets.h"
#include "ir/IrRankingTableViewport.h"
#include <bgfx/bgfx.h>

#include <cmath>
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

#include "ir_ranking_view.inc"

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

void testRankingViewsKeepEveryColumn() {
  struct TextCapture : rendering::UiBatchBackend {
    std::size_t vertices = 0;
    std::vector<rendering::PosTexCoord0Vertex> points;
    bool submit(const rendering::UiBatchSubmission &submission) noexcept override {
      vertices += submission.texturedVertices.size();
      points.insert(points.end(), submission.texturedVertices.begin(), submission.texturedVertices.end());
      return true;
    }
  };
  const auto textQuads = [](View &view) {
    TextCapture capture;
    rendering::UiBatchRenderer batch(capture);
    RenderContext context(batch);
    batch.begin();
    view.render(context);
    batch.end();
    return capture.vertices / 4;
  };
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 1920;
  init.resolution.height = 1080;
  REQUIRE(bgfx::init(init));
  {
    ir::IrRankingModalModel model;
    model.open(request(), "Chart");
    REQUIRE(model.apply(snapshot(ir::IrRankingSnapshotState::Succeeded)));
    ir::RankingRowView row;
    ir::RankingTableHeaderView header;
    for (const int width : {1200, 900, 980, 1200}) {
      row.setSize(width, 92);
      row.bind(model.row(0, width));
      header.setWidth(width);
      header.bind(width);
      constexpr auto expected = 8;
      REQUIRE(textQuads(row) == expected);
      REQUIRE(textQuads(header) == expected);
    }
    row.setSize(900, 74);
    for (const auto &[label, expected] : {
             std::pair{"NORMAL CLEAR", "NORMAL"}, {"EX-HARD CLEAR", "EX-HARD"},
             {"ASSIST EASY CLEAR", "ASSIST EASY"}, {"FULL COMBO", "FULL COMBO"}}) {
      auto presentation = model.row(0, 900);
      presentation.lampText = label;
      row.bind(presentation);
      auto *lamp = dynamic_cast<TextView *>(row.getChildren().front()->getChildren()[4]);
      REQUIRE(lamp && lamp->getText() == expected);
      TextCapture capture;
      rendering::UiBatchRenderer batch(capture);
      RenderContext context(batch);
      batch.begin();
      lamp->render(context);
      batch.end();
      REQUIRE(capture.points.size() == 4);
      for (const auto &point : capture.points) {
        REQUIRE(point.x >= lamp->getX() && point.x <= lamp->getX() + lamp->getWidth());
        REQUIRE(point.y >= lamp->getY() && point.y <= lamp->getY() + lamp->getHeight());
      }
      for (auto *cell : row.getChildren().front()->getChildren()) {
        REQUIRE(cell->getX() >= row.getX());
        REQUIRE(cell->getX() + cell->getWidth() <= row.getX() + row.getWidth());
      }
    }
  }
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
}

void testRankingViewportScrollsOnlyWhenNeededAndKeepsSelection() {
  ir::RankingTableViewport viewport;
  viewport.setSize(600, 400);
  auto *table = new View();
  table->setFlexDirection(FlexDirection::Column);
  auto *header = new View();
  header->setHeight(34)->setFlexShrink(0);
  table->addView(header);
  auto *list = new RecyclerView<int>([](int left, int right) { return left == right; });
  list->setFlex(1)->setMinHeight(0);
  list->itemHeight = 74;
  list->onCreateView = [](const int &) { return new View(); };
  std::vector<int> rows(100);
  for (int i = 0; i < 100; ++i) rows[i] = i;
  list->setItemProvider(100, [&rows](int index) -> const int & { return rows[index]; });
  int selections = 0;
  list->onSelected = [&selections](const int &, int) { ++selections; };
  table->addView(list);
  viewport.setContentView(table);
  const auto finger = [&](Uint32 type, float x, float y) {
    SDL_Event event{};
    event.type = type;
    event.tfinger.touchId = 1;
    event.tfinger.fingerId = 42;
    event.tfinger.x = x / rendering::window_width;
    event.tfinger.y = y / rendering::window_height;
    viewport.handleEvents(event);
  };
  finger(SDL_FINGERDOWN, 500, 150);
  finger(SDL_FINGERMOTION, 300, 150);
  finger(SDL_FINGERUP, 300, 150);
  REQUIRE(table->getX() == -200);
  REQUIRE(header->getX() == list->getX());
  REQUIRE(list->scrollOffset == 0 && selections == 0);
  finger(SDL_FINGERDOWN, 400, 200);
  finger(SDL_FINGERMOTION, 400, 100);
  finger(SDL_FINGERUP, 400, 100);
  REQUIRE(list->scrollOffset > 0 && selections == 0);
  REQUIRE(table->getX() == -200);
  finger(SDL_FINGERDOWN, 400, 100);
  finger(SDL_FINGERUP, 400, 100);
  REQUIRE(selections == 1);
  viewport.setSize(1080, 400);
  REQUIRE(table->getX() == 0 && table->getWidth() == 1080);
  REQUIRE(header->getX() == list->getX());
  viewport.setSize(600, 400);
  SDL_Event mouse{};
  mouse.type = SDL_MOUSEBUTTONDOWN;
  mouse.button.button = SDL_BUTTON_LEFT;
  mouse.button.x = 500;
  mouse.button.y = 150;
  viewport.handleEvents(mouse);
  REQUIRE(selections == 1);
  mouse = {};
  mouse.type = SDL_MOUSEMOTION;
  mouse.motion.x = 300;
  mouse.motion.y = 150;
  viewport.handleEvents(mouse);
  mouse = {};
  mouse.type = SDL_MOUSEBUTTONUP;
  mouse.button.button = SDL_BUTTON_LEFT;
  mouse.button.x = 300;
  mouse.button.y = 150;
  viewport.handleEvents(mouse);
  REQUIRE(selections == 1 && table->getX() == -200);
}

void testRetainedModalTreesRefreshLanguageWhileHidden() {
  struct Caption : View {
    std::string key;
    std::string text;
    int changes = 0;
    explicit Caption(std::string value) : key(std::move(value)), text(i18n::tr(key)) {}
    void onLanguageChanged() override {
      text = i18n::tr(key);
      ++changes;
    }
  };
  i18n::setLanguage(i18n::Language::English);
  View root;
  View scoreDetail;
  auto *tab = new Caption("ir.ranking.nearby_tab.label");
  auto *detail = new Caption("ir.ranking.score_detail.ex_score.label");
  root.addView(tab);
  scoreDetail.addView(detail);
  root.setVisible(false);
  scoreDetail.setVisible(false);
  std::uint64_t revision = 0;
  ir::refreshIrRankingModalLanguage(root, scoreDetail, revision);
  REQUIRE(tab->text == "Near me");
  const int changes = tab->changes;
  ir::refreshIrRankingModalLanguage(root, scoreDetail, revision);
  REQUIRE(tab->changes == changes);
  ir::IrRankingModalModel model;
  model.open(request(), "Raw title");
  const auto source = snapshot(ir::IrRankingSnapshotState::Succeeded);
  REQUIRE(model.apply(source));
  i18n::setLanguage(i18n::Language::Korean);
  REQUIRE(model.apply(source));
  REQUIRE(model.row(1, 1200).playerText == "PLAYER  ·  나");
  REQUIRE(model.scoreDetail(1)->playerText == "PLAYER  ·  나");
  ir::refreshIrRankingModalLanguage(root, scoreDetail, revision);
  REQUIRE(tab->text == "내 주변");
  REQUIRE(detail->text == "EX 점수");
  REQUIRE(tab->changes == changes + 1);
  root.setVisible(true);
  i18n::setLanguage(i18n::Language::Japanese);
  ir::refreshIrRankingModalLanguage(root, scoreDetail, revision);
  REQUIRE(tab->text == "自分の周辺");
  REQUIRE(detail->text == "EX スコア");
  i18n::setLanguage(i18n::Language::English);
}

void testLocalComparisonRetainsLocalizedLabelsAndRawMetrics() {
  i18n::setLanguage(i18n::Language::English);
  auto comparison = *request().localComparison;
  comparison.label = i18n::message("menu.local_pb.label");
  const auto text = ir::formatIrLocalComparison(comparison);
  REQUIRE(text.resolve() ==
          "Local PB   EX 1700 / 2000   85.00%   HARD CLEAR   BP 15   Combo 731");
  i18n::setLanguage(i18n::Language::Korean);
  REQUIRE(text.resolve() ==
          "개인 최고 기록   EX 1700 / 2000   85.00%   HARD CLEAR   BP 15   콤보 731");
  i18n::setLanguage(i18n::Language::Japanese);
  REQUIRE(text.resolve() ==
          "ローカル自己ベスト   EX 1700 / 2000   85.00%   HARD CLEAR   BP 15   コンボ 731");
  comparison.label = "Raw {label} / Local PB";
  comparison.badPoints.reset();
  comparison.maxCombo.reset();
  const auto missing = ir::formatIrLocalComparison(comparison);
  REQUIRE(missing.resolve() ==
          "Raw {label} / Local PB   EX 1700 / 2000   85.00%   HARD CLEAR   BP —   コンボ —");
  i18n::setLanguage(i18n::Language::English);
  REQUIRE(missing.resolve() ==
          "Raw {label} / Local PB   EX 1700 / 2000   85.00%   HARD CLEAR   BP —   Combo —");
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

void testRankingTabsSeparateTopAndNearbyRows() {
  ir::IrRankingModalModel model;
  model.open(request(), "Test Chart");
  auto value = std::make_shared<ir::IrChartRanking>();
  value->totalPlayers = 6000;
  value->nextPageToken = "page-2";
  for (int rank = 1; rank <= 100; ++rank) {
    value->entries.push_back({.rank = rank, .providerEntryId = std::to_string(rank)});
  }
  for (int rank = 4995; rank <= 5005; ++rank) {
    value->nearbyEntries.push_back({.rank = rank,
        .providerEntryId = std::to_string(rank), .currentUser = rank == 5000});
  }
  auto source = snapshot(ir::IrRankingSnapshotState::Succeeded);
  source.ranking = value;
  REQUIRE(model.apply(source));
  const auto &presentation = model.presentation();
  REQUIRE(presentation.activeTab == ir::IrRankingTab::Nearby);
  REQUIRE(presentation.hasNearbyRanking);
  REQUIRE(presentation.entryCount == 11);
  REQUIRE(model.row(0, 1200).rankText == "#4995");
  REQUIRE(model.row(5, 1200).rankText == "#5000");
  REQUIRE(model.row(5, 1200).highlighted);
  REQUIRE(model.row(5, 1200).playerText == "You");
  REQUIRE(model.scoreDetail(5)->highlighted);
  REQUIRE(!presentation.canLoadNextPage);
  REQUIRE(!presentation.ranking->nextPageToken);
  REQUIRE(presentation.paginationStatusText.find("Top rankings") != std::string::npos);

  REQUIRE(model.selectTab(ir::IrRankingTab::Top));
  REQUIRE(presentation.entryCount == 100);
  REQUIRE(model.row(0, 1200).rankText == "#1");
  REQUIRE(model.row(99, 1200).rankText == "#100");
  REQUIRE(presentation.canLoadNextPage);
  REQUIRE(presentation.paginationStatusText.empty());
  REQUIRE(!ir::shouldLoadNextIrRankingPage(presentation.entryCount, 0, 600, 60));
  REQUIRE(ir::shouldLoadNextIrRankingPage(presentation.entryCount, 4900, 600, 60));
  REQUIRE(!model.selectTab(ir::IrRankingTab::Top));
  REQUIRE(model.selectTab(ir::IrRankingTab::Nearby));

  // A previously started top-page request can finish while Near me is selected.
  auto nextPage = std::make_shared<ir::IrChartRanking>(*value);
  for (int rank = 101; rank <= 200; ++rank) {
    nextPage->entries.push_back({.rank = rank, .providerEntryId = std::to_string(rank)});
  }
  source.ranking = nextPage;
  ++source.revision;
  REQUIRE(model.apply(source));
  REQUIRE(presentation.activeTab == ir::IrRankingTab::Nearby);
  REQUIRE(presentation.entryCount == 11);
  REQUIRE(model.row(0, 1200).rankText == "#4995");
  REQUIRE(!presentation.canLoadNextPage);
  REQUIRE(model.selectTab(ir::IrRankingTab::Top));
  REQUIRE(presentation.entryCount == 200);
  REQUIRE(model.row(199, 1200).rankText == "#200");
  REQUIRE(value->entries.size() == 100);
  REQUIRE(value->nearbyEntries.size() == 11);

  // Refresh keeps an explicit tab choice, and opening another chart resets it.
  model.refresh(8);
  source.generation = 8;
  source.request = request(8);
  ++source.revision;
  REQUIRE(model.apply(source));
  REQUIRE(presentation.activeTab == ir::IrRankingTab::Top);
  REQUIRE(model.selectTab(ir::IrRankingTab::Nearby));
  model.refresh(9);
  source.generation = 9;
  source.request = request(9);
  ++source.revision;
  REQUIRE(model.apply(source));
  REQUIRE(presentation.activeTab == ir::IrRankingTab::Nearby);
  model.open(request(), "Another chart");
  REQUIRE(model.apply(snapshot(ir::IrRankingSnapshotState::Succeeded)));
  REQUIRE(presentation.activeTab == ir::IrRankingTab::Top);
}

void testNearbyTabUsesOwnRowFromLoadedTopPages() {
  ir::IrRankingModalModel model;
  model.open(request(), "Test Chart");
  auto source = snapshot(ir::IrRankingSnapshotState::Succeeded);
  REQUIRE(model.apply(source));
  REQUIRE(model.presentation().hasNearbyRanking);
  REQUIRE(model.selectTab(ir::IrRankingTab::Nearby));
  REQUIRE(model.row(1, 1200).highlighted);
  REQUIRE(!model.presentation().canLoadNextPage);

  auto completed = std::make_shared<ir::IrChartRanking>();
  completed->totalPlayers = 200;
  for (int rank = 1; rank <= 200; ++rank) {
    completed->entries.push_back({.rank = rank,
        .providerEntryId = std::to_string(rank), .currentUser = rank == 105});
  }
  source.ranking = completed;
  ++source.revision;
  REQUIRE(model.apply(source));
  REQUIRE(model.presentation().activeTab == ir::IrRankingTab::Nearby);
  REQUIRE(model.presentation().entryCount == 11);
  REQUIRE(model.row(0, 1200).rankText == "#100");
  REQUIRE(model.row(5, 1200).rankText == "#105");
  REQUIRE(model.row(10, 1200).rankText == "#110");
  REQUIRE(!model.presentation().canLoadNextPage);

  auto anonymous = std::make_shared<ir::IrChartRanking>(*source.ranking);
  for (auto &entry : anonymous->entries) entry.currentUser = false;
  source.ranking = anonymous;
  ++source.revision;
  REQUIRE(model.apply(source));
  REQUIRE(model.presentation().activeTab == ir::IrRankingTab::Top);
  REQUIRE(!model.presentation().hasNearbyRanking);
  REQUIRE(!model.selectTab(ir::IrRankingTab::Nearby));
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
  REQUIRE(constrained.showBadPoints);
  REQUIRE(constrained.showMaxCombo);
  REQUIRE(constrained.showAchievementTime);

  auto compact = model.row(0, 560);
  REQUIRE(compact.compact);
  REQUIRE(compact.showBadPoints);
  REQUIRE(compact.showMaxCombo);
  REQUIRE(compact.showAchievementTime);
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

void testRecyclerUsesPreciseWheelDeltaAndNaturalDirection() {
  RecyclerView<int> recycler(
      [](const int left, const int right) { return left == right; });
  recycler.setWidth(800)->setHeight(200)->applyYogaLayout();
  recycler.itemHeight = 64;
  recycler.onCreateView = [](const int &) { return new View(); };
  recycler.onBind = [](View *, const int &, int, bool) {};
  recycler.setItems(std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10});

  SDL_Event normal{};
  normal.type = SDL_MOUSEWHEEL;
  normal.wheel.type = SDL_MOUSEWHEEL;
  normal.wheel.y = 0;
  normal.wheel.preciseY = 0.25F;
  normal.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  recycler.scrollOffset = 100.0F;
  recycler.handleEvents(normal);
  REQUIRE(std::abs(recycler.scrollOffset - 96.25F) < 0.001F);

  SDL_Event natural = normal;
  natural.wheel.preciseY = -0.25F;
  natural.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
  recycler.scrollOffset = 100.0F;
  recycler.handleEvents(natural);
  REQUIRE(std::abs(recycler.scrollOffset - 103.75F) < 0.001F);
}

void testRecyclerLanguageRefreshKeepsBoundRowsAndSelection() {
  struct Row : View {
    int languageChanges = 0;
    void onLanguageChanged() override { ++languageChanges; }
  };
  RecyclerView<int> recycler([](int left, int right) { return left == right; });
  recycler.setWidth(800)->setHeight(200)->applyYogaLayout();
  recycler.itemHeight = 64;
  int bindings = 0;
  recycler.onCreateView = [](const int &) { return new Row(); };
  recycler.onBind = [&](View *, const int &, int, bool) { ++bindings; };
  recycler.setItems(std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8});
  recycler.selectedIndex = 1;
  recycler.scrollOffset = 64.0F;
  recycler.rebindVisibleItems();
  auto *row = static_cast<Row *>(recycler.getViewByIndex(1));
  REQUIRE(row != nullptr);
  const int bindingsBefore = bindings;
  recycler.propagateLanguageChange();
  REQUIRE(row == recycler.getViewByIndex(1));
  REQUIRE(row->languageChanges == 1);
  REQUIRE(bindings == bindingsBefore);
  REQUIRE(recycler.selectedIndex == 1);
  REQUIRE(recycler.scrollOffset == 64.0F);
}

void testLanguageChangeRefreshesAcceptedSnapshotWithoutNewRequest() {
  i18n::setLanguage(i18n::Language::English);
  ir::IrRankingModalModel model;
  model.open(request(), "Raw chart title");
  ir::IrRankingSnapshot snapshot;
  snapshot.request = request();
  snapshot.generation = request().generation;
  snapshot.revision = 4;
  snapshot.state = ir::IrRankingSnapshotState::ChartNotFound;
  snapshot.diagnostic = "Raw provider diagnostic";
  REQUIRE(model.apply(snapshot));
  const std::string before = model.presentation().statusText;
  i18n::setLanguage(i18n::Language::Korean);
  REQUIRE(model.apply(snapshot));
  REQUIRE(model.presentation().statusText != before);
  REQUIRE(model.presentation().chartTitle == "Raw chart title");
  REQUIRE(model.presentation().detailText == "Raw provider diagnostic");
  REQUIRE(model.presentation().generation == snapshot.generation);
  REQUIRE(model.presentation().revision == snapshot.revision);
  REQUIRE(!model.apply(snapshot));
  i18n::setLanguage(i18n::Language::Japanese);
  --snapshot.revision;
  REQUIRE(!model.apply(snapshot));
  i18n::setLanguage(i18n::Language::English);
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
  testRankingViewsKeepEveryColumn();
  testRankingViewportScrollsOnlyWhenNeededAndKeepsSelection();
  testRetainedModalTreesRefreshLanguageWhileHidden();
  testLocalComparisonRetainsLocalizedLabelsAndRawMetrics();
  testLanguageChangeRefreshesAcceptedSnapshotWithoutNewRequest();
  testRecyclerLanguageRefreshKeepsBoundRowsAndSelection();
  testModalStateMappingAndActions();
  testFullRequestIdentityAndRefreshGenerationGuard();
  testComparisonStaysSeparateAndYouEntryIsHighlighted();
  testRankingTabsSeparateTopAndNearbyRows();
  testNearbyTabUsesOwnRowFromLoadedTopPages();
  testResponsiveRowsKeepFixedHeightCoreFields();
  testScoreDetailFormatsCompleteAndMissingData();
  testPaginationPresentationKeepsSuccessfulListVisible();
  testTwentyThousandEntriesCreateOnlyVisibleRows();
  testVirtualizedPaginationThresholdAndScrollRetention();
  testRecyclerBindingSeesAppliedRowWidth();
  testRecyclerIgnoresMouseSynthesizedTouchSelection();
  testRecyclerUsesPreciseWheelDeltaAndNaturalDirection();
  testBokutachiEligibilityRequiresSupportedModeNotesAndSha256();
  return 0;
}
