#include "ResultImageExporter.h"

#include "rendering/UniformCache.h"
#include "scene/ResultPresentationModel.h"
#include "skin/DefaultSkin.h"
#include "view/ClearLampColors.h"
#include "view/TextView.h"
#include "view/UiTheme.h"

#include <bgfx/bgfx.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>

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

void SceneManager::changeScene(const std::string &, bool) {}

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool sameColor(const Color &left, const Color &right) {
  return left.r == right.r && left.g == right.g && left.b == right.b &&
         left.a == right.a;
}

ir::IrRemoteScore completeRemoteScore() {
  return {
      .remoteUserId = 72,
      .game = "bms-7k",
      .remoteScoreId = "remote-score-72",
      .remoteChartId = "remote-chart-72",
      .chartMd5 = std::string(32, 'a'),
      .chartSha256 = std::string(64, 'b'),
      .title = "Remote / Result .. Complete",
      .artist = "Remote Artist",
      .service = "service-secret-must-not-leak",
      .difficulty = "ANOTHER",
      .level = "12",
      .noteCount = 1'000,
      .score = 1'800,
      .lampRank = kClearTypeHardClearRank,
      .timeAchievedUnixMillis = 1'721'377'845'000LL,
      .timeAddedUnixMillis = 1'721'377'846'000LL,
      .judgements =
          {.pGreat = 850, .great = 80, .good = 30, .bad = 20, .poor = 20},
      .timing = {.earlyPGreat = 400,
                 .latePGreat = 450,
                 .earlyGreat = 35,
                 .lateGreat = 45,
                 .earlyGood = 12,
                 .lateGood = 18,
                 .earlyBad = 8,
                 .lateBad = 12,
                 .earlyPoor = 9,
                 .latePoor = 11},
      .fast = 464,
      .slow = 536,
      .maxCombo = 760,
      .badPoints = 40,
      .finalGauge = 78.0F,
      .gaugeHistory = {20.0F, 48.0F, std::nullopt, 0.0F, 78.0F},
      .random = "RANDOM",
      .gauge = "HARD",
      .inputDevice = "KEYBOARD",
      .client = "client-secret-must-not-leak",
  };
}

std::unique_ptr<View> buildLayout(ResultSkinData data, bool sceneControls) {
  rendering::window_width = 1920;
  rendering::window_height = 1080;
  rendering::render_width = 1920;
  rendering::render_height = 1080;
  rendering::ui_view_width = 1920;
  rendering::ui_view_height = 1080;
  data.showControls = sceneControls;

  auto root = std::make_unique<View>(0, 0, 1920, 1080);
  DefaultSkin skin;
  skin.buildLayout("Result", root.get(), &data);
  root->applyYogaLayout();
  return root;
}

bool isCardOrGraphName(std::string_view name) {
  constexpr std::string_view prefixes[]{
      "resultSummaryCard:",
      "resultInfoTile:",
      "resultJudgementTile:",
      "resultMetricTile:",
  };
  return name == "graph" ||
         std::ranges::any_of(prefixes, [name](std::string_view prefix) {
           return name.starts_with(prefix);
         });
}

std::set<std::string> namedCardAndGraphSet(View &root) {
  std::set<std::string> result;
  const auto collect = [&result](const auto &self, View &view) -> void {
    if (isCardOrGraphName(view.getName())) {
      result.insert(view.getName());
    }
    for (View *child : view.getChildren()) {
      self(self, *child);
    }
  };
  collect(collect, root);
  return result;
}

const TextView *textView(View *root, std::string_view name) {
  return root == nullptr ? nullptr
                         : dynamic_cast<const TextView *>(
                               root->findViewByName(std::string(name)));
}

std::string readSource(const std::filesystem::path &relative) {
  const auto path = std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / relative;
  std::ifstream input(path);
  expect(input.good(), "Task 8 source contract is readable");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-result-export-" + std::to_string(nonce));
    std::error_code error;
    std::filesystem::create_directories(path, error);
    expect(!error, "temporary export directory is created");
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }

  std::filesystem::path path;
};

struct ControlledExportBackend {
  int calls = 0;
  std::filesystem::path outputPath;
  std::set<std::string> cards;
  std::optional<result_gauge_history::ResultGaugeGraph> gauge;
  bool succeed = true;

  ResultImageExportResult operator()(
      ResultSkinData skinData,
      std::optional<result_gauge_history::ResultGaugeGraph> selectedGauge,
      const std::filesystem::path &path) {
    ++calls;
    outputPath = path;
    gauge = std::move(selectedGauge);
    const auto root = buildLayout(skinData, false);
    cards = namedCardAndGraphSet(*root);
    if (!succeed) {
      return {.success = false,
              .outputPath = path,
              .message = "controlled render failure"};
    }
    std::ofstream artifact(path, std::ios::binary);
    artifact << "controlled-result-image";
    if (!artifact.good()) {
      return {.success = false,
              .outputPath = path,
              .message = "controlled artifact write failed"};
    }
    return {.success = true, .outputPath = path, .message = "Exported"};
  }
};

void testCompletePresentationUsesSceneCardsAndGaugePlan() {
  const ResultPresentationModel presentation =
      makeRemoteResultPresentation(completeRemoteScore());
  const auto exportPlan =
      result_image_export::presentationPlanFor(presentation, "20260719_123456");

  expect(exportPlan.skinData.presentation == &presentation &&
             !exportPlan.skinData.showControls &&
             exportPlan.skinData.showResultGraph,
         "export plan passes the immutable presentation through the "
         "authoritative skin path");
  expect(exportPlan.filename == "Remote_Result_Complete_20260719_123456.png",
         "export filename is derived only from sanitized title and timestamp");
  expect(exportPlan.filename.find("service-secret") == std::string::npos &&
             exportPlan.filename.find("client-secret") == std::string::npos &&
             exportPlan.filename.find('/') == std::string::npos &&
             exportPlan.filename.find("..") == std::string::npos,
         "export filename excludes service/client data and traversal tokens");

  ResultSkinData sceneData{};
  sceneData.presentation = &presentation;
  sceneData.showResultGraph = !presentation.gaugeSeries.empty();
  const auto sceneRoot = buildLayout(sceneData, true);
  const auto exportRoot = buildLayout(exportPlan.skinData, false);
  const auto sceneCards = namedCardAndGraphSet(*sceneRoot);
  const auto exportCards = namedCardAndGraphSet(*exportRoot);

  expect(sceneCards == exportCards,
         "scene and export build the exact same named card and graph set");
  for (const char *required : {
           "resultSummaryCard:grade",
           "resultSummaryCard:score",
           "resultSummaryCard:lamp",
           "resultSummaryCard:combo",
           "resultInfoTile:total-notes",
           "resultInfoTile:bp",
           "resultInfoTile:service",
           "resultInfoTile:client",
           "resultInfoTile:input-device",
           "resultInfoTile:random",
           "resultInfoTile:gauge-type",
           "resultInfoTile:level",
           "resultJudgementTile:pgreat",
           "resultJudgementTile:great",
           "resultJudgementTile:good",
           "resultJudgementTile:bad",
           "resultJudgementTile:poor",
           "resultMetricTile:break",
           "resultMetricTile:fast",
           "resultMetricTile:slow",
           "graph",
       }) {
    expect(exportCards.contains(required),
           "complete export retains every supplied named card");
  }
  expect(!exportCards.contains("resultJudgementTile:kpoor") &&
             exportRoot->findViewByName("timingAnalytics") == nullptr,
         "remote export never invents KPOOR or replay analytics");

  const auto sceneGraph =
      result_gauge_history::graphFor(presentation.gaugeSeries, 0);
  expect(sceneGraph.has_value() && exportPlan.gauge.has_value(),
         "scene and export both select the supplied first gauge series");
  if (sceneGraph && exportPlan.gauge) {
    expect(sceneGraph->geometry.strips.size() == 2 &&
               sceneGraph->geometry.segments.size() == 2 &&
               sceneGraph->geometry.markers.size() ==
                   exportPlan.gauge->geometry.markers.size() &&
               sceneGraph->geometry.strips.size() ==
                   exportPlan.gauge->geometry.strips.size() &&
               sceneGraph->geometry.segments.size() ==
                   exportPlan.gauge->geometry.segments.size(),
           "nullable gauge runs remain disconnected with shared markers");
    expect(sceneGraph->label && exportPlan.gauge->label &&
               sceneGraph->label->text == "HARD" &&
               exportPlan.gauge->label->text == sceneGraph->label->text &&
               sameColor(exportPlan.gauge->label->background,
                         sceneGraph->label->background) &&
               sameColor(exportPlan.gauge->label->background,
                         clearLampColorForRank(kClearTypeHardClearRank)),
           "export preserves the scene gauge label and semantic color");
  }
}

void testSparseAndExplicitZeroPresentationPhysicallyOmitMissingCards() {
  ResultPresentationModel sparse;
  sparse.title = "Sparse Score Lamp";
  sparse.score = 0;
  sparse.maxScore = 2'000;
  sparse.scoreComparison = ResultComparisonCard{
      .title = "SCORE",
      .current = {.label = "CURRENT",
                  .value = "0",
                  .detail = "MAX 2000",
                  .accent = ui_theme::textPrimary()},
  };
  sparse.lampRank = kClearTypeFailedRank;
  sparse.lampComparison = ResultComparisonCard{
      .title = "CLEAR LAMP",
      .current = {.label = "CURRENT",
                  .value = "FAILED",
                  .detail = {},
                  .accent = clearLampColorForRank(kClearTypeFailedRank)},
  };
  sparse.readOnlyIrUploaded = true;

  const auto plan =
      result_image_export::presentationPlanFor(sparse, "20260719_234500");
  const auto root = buildLayout(plan.skinData, false);
  const auto cards = namedCardAndGraphSet(*root);
  expect(cards == std::set<std::string>({"resultSummaryCard:grade",
                                         "resultSummaryCard:lamp",
                                         "resultSummaryCard:score"}),
         "score/lamp-only export consumes no unavailable card space");
  expect(textView(root.get(), "grade") &&
             textView(root.get(), "grade")->getText() == "F" &&
             textView(root.get(), "resultSummaryValueText:score:current") &&
             textView(root.get(), "resultSummaryValueText:score:current")
                     ->getText() == "0",
         "explicit score zero remains visible in the exported layout");
  expect(!root->findViewByName("resultInfoGrid") &&
             !root->findViewByName("detailsGrid") &&
             !root->findViewByName("resultSummaryCard:combo") &&
             !root->findViewByName("graph") &&
             !root->findViewByName("timingAnalytics") && !plan.gauge,
         "missing combo, BP, gauge, metadata, judgements, and analytics are "
         "physically omitted");
}

void testExplicitRemoteZerosRemainSuppliedExportValues() {
  auto score = completeRemoteScore();
  score.score = 0;
  score.judgements = {.pGreat = 0, .great = 0, .good = 0, .bad = 0, .poor = 0};
  score.timing = {.earlyPGreat = 0,
                  .latePGreat = 0,
                  .earlyGreat = 0,
                  .lateGreat = 0,
                  .earlyGood = 0,
                  .lateGood = 0,
                  .earlyBad = 0,
                  .lateBad = 0,
                  .earlyPoor = 0,
                  .latePoor = 0};
  score.fast = 0;
  score.slow = 0;
  score.maxCombo = 0;
  score.badPoints = 0;
  score.finalGauge = 0.0F;
  score.gaugeHistory = {0.0F};
  const ResultPresentationModel presentation =
      makeRemoteResultPresentation(score);
  const auto plan =
      result_image_export::presentationPlanFor(presentation, "20260719_234501");
  const auto root = buildLayout(plan.skinData, false);

  for (const char *name :
       {"grade", "resultSummaryCard:combo", "BP", "pgreat", "pgreatFast",
        "pgreatSlow", "break", "fast", "slow", "graph"}) {
    expect(root->findViewByName(name) != nullptr,
           "explicit remote zero keeps its supplied exported view");
  }
  for (const char *name :
       {"resultSummaryValueText:score:current", "BP", "pgreat", "pgreatFast",
        "pgreatSlow", "break", "fast", "slow"}) {
    expect(textView(root.get(), name) &&
               textView(root.get(), name)->getText() == "0",
           "explicit remote zero is rendered as zero text");
  }
  expect(plan.gauge && plan.gauge->geometry.markers.size() == 1 &&
             plan.gauge->geometry.markers.front().value == 0.0F,
         "explicit zero gauge remains a present exported marker");
}

void testFilenameFallbackAndRemoteSceneExportContract() {
  ResultPresentationModel reserved;
  reserved.title = "CON";
  const auto reservedPlan =
      result_image_export::presentationPlanFor(reserved, "20260719_010203");
  expect(reservedPlan.filename == "result_CON_20260719_010203.png",
         "reserved device title receives a bounded safe prefix");

  ResultPresentationModel traversal;
  traversal.title = std::string("../\\\n\r\t") + std::string(200, '*');
  const auto fallback =
      result_image_export::presentationPlanFor(traversal, "20260719_010204");
  expect(fallback.filename == "result_20260719_010204.png" &&
             fallback.filename.size() < 120,
         "control-only traversal title uses the bounded result fallback");

  ResultPresentationModel longTitle;
  longTitle.title = std::string(200, 'A');
  const auto bounded =
      result_image_export::presentationPlanFor(longTitle, "20260719_010205");
  expect(bounded.filename.size() == 100 &&
             bounded.filename.ends_with("_20260719_010205.png"),
         "sanitized presentation filename keeps the existing 80-byte title "
         "bound and timestamp policy");

  const std::string scene = readSource("src/scene/ResultScene.cpp");
  expect(scene.find("ResultImageExporter::Export(context, "
                    "remote->presentation)") != std::string::npos,
         "remote ResultScene passes its immutable presentation to export");
  expect(scene.find("Export Unavailable") == std::string::npos,
         "temporary Task 7 unavailable branch is removed");
  expect(scene.find("local->meta, local->resultState") != std::string::npos,
         "local ResultScene keeps the legacy export path");
}

void testProductionPresentationExportWritesCompleteAndSparseArtifacts() {
  TemporaryDirectory temporary;
  const result_image_export::PresentationExportDestination destination{
      .outputDirectory = temporary.path,
      .timestamp = "20260719_123456",
  };

  ControlledExportBackend completeBackend;
  const ResultPresentationModel complete =
      makeRemoteResultPresentation(completeRemoteScore());
  const ResultImageExportResult completeResult = ResultImageExporter::Export(
      complete, destination, std::ref(completeBackend));
  expect(completeResult.success && completeBackend.calls == 1 &&
             completeResult.outputPath == completeBackend.outputPath &&
             completeResult.outputPath.filename() ==
                 "Remote_Result_Complete_20260719_123456.png" &&
             std::filesystem::is_regular_file(completeResult.outputPath),
         "production presentation export writes one safe complete artifact");
  expect(completeBackend.cards.contains("resultSummaryCard:combo") &&
             completeBackend.cards.contains("resultJudgementTile:poor") &&
             completeBackend.cards.contains("resultInfoTile:service") &&
             completeBackend.cards.contains("graph") &&
             !completeBackend.cards.contains("resultJudgementTile:kpoor"),
         "production export backend receives the complete remote card set");
  expect(completeBackend.gauge.has_value() &&
             completeBackend.gauge->geometry.strips.size() == 2 &&
             completeBackend.gauge->geometry.segments.size() == 2,
         "production export preserves nullable gauge gaps");

  ResultPresentationModel sparse;
  sparse.title = "../Sparse / Score Lamp";
  sparse.score = 0;
  sparse.maxScore = 2'000;
  sparse.scoreComparison = ResultComparisonCard{
      .title = "SCORE",
      .current = {.label = "CURRENT",
                  .value = "0",
                  .detail = "MAX 2000",
                  .accent = ui_theme::textPrimary()},
  };
  sparse.lampRank = kClearTypeFailedRank;
  sparse.lampComparison = ResultComparisonCard{
      .title = "CLEAR LAMP",
      .current = {.label = "CURRENT",
                  .value = "FAILED",
                  .detail = {},
                  .accent = clearLampColorForRank(kClearTypeFailedRank)},
  };
  sparse.readOnlyIrUploaded = true;

  ControlledExportBackend sparseBackend;
  const auto sparseDestination =
      result_image_export::PresentationExportDestination{
          .outputDirectory = temporary.path,
          .timestamp = "20260719_234500",
      };
  const ResultImageExportResult sparseResult = ResultImageExporter::Export(
      sparse, sparseDestination, std::ref(sparseBackend));
  expect(sparseResult.success && sparseBackend.calls == 1 &&
             sparseResult.outputPath.parent_path() == temporary.path &&
             sparseResult.outputPath.filename() ==
                 "Sparse_Score_Lamp_20260719_234500.png" &&
             std::filesystem::is_regular_file(sparseResult.outputPath),
         "production sparse export remains inside its destination");
  expect(sparseBackend.cards ==
                 std::set<std::string>({"resultSummaryCard:grade",
                                        "resultSummaryCard:lamp",
                                        "resultSummaryCard:score"}) &&
             !sparseBackend.gauge.has_value(),
         "production sparse export omits unavailable cards and gauge");
}

void testProductionPresentationExportPropagatesFailures() {
  TemporaryDirectory temporary;
  const result_image_export::PresentationExportDestination destination{
      .outputDirectory = temporary.path,
      .timestamp = "20260719_123457",
  };
  ControlledExportBackend failingBackend;
  failingBackend.succeed = false;
  ResultPresentationModel presentation;
  presentation.title = "Failure Case";
  const ResultImageExportResult failed = ResultImageExporter::Export(
      presentation, destination, std::ref(failingBackend));
  expect(!failed.success && failingBackend.calls == 1 &&
             failed.message == "controlled render failure" &&
             !std::filesystem::exists(failed.outputPath),
         "production presentation export propagates renderer failure");

  const auto blockedDirectory = temporary.path / "not-a-directory";
  {
    std::ofstream file(blockedDirectory);
    file << "blocking file";
  }
  ControlledExportBackend unreachableBackend;
  const ResultImageExportResult directoryFailure = ResultImageExporter::Export(
      presentation,
      {.outputDirectory = blockedDirectory, .timestamp = "20260719_123458"},
      std::ref(unreachableBackend));
  expect(!directoryFailure.success && unreachableBackend.calls == 0 &&
             directoryFailure.message.find("directory") != std::string::npos,
         "production presentation export fails before rendering when its "
         "destination cannot be created");
}
} // namespace

int main() {
  static_assert(requires(ApplicationContext &context,
                         const ResultPresentationModel &presentation) {
    {
      ResultImageExporter::Export(context, presentation)
    } -> std::same_as<ResultImageExportResult>;
  });

  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  if (!bgfx::init(init)) {
    std::cerr << "FAIL: headless bgfx initialization failed\n";
    return 1;
  }
  ui_theme::setActiveMode(ui_theme::ThemeMode::Dark);
  testCompletePresentationUsesSceneCardsAndGaugePlan();
  testSparseAndExplicitZeroPresentationPhysicallyOmitMissingCards();
  testExplicitRemoteZerosRemainSuppliedExportValues();
  testFilenameFallbackAndRemoteSceneExportContract();
  testProductionPresentationExportWritesCompleteAndSparseArtifacts();
  testProductionPresentationExportPropagatesFailures();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  if (failures != 0) {
    std::cerr << failures << " partial image exporter test(s) failed\n";
    return 1;
  }
  std::cout << "Partial result image exporter tests passed\n";
  return 0;
}
