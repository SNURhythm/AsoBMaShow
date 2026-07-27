#include "../src/rendering/UniformCache.h"
#include "../src/view/Button.h"
#include "../src/view/IconText.h"
#include "../src/view/ResultRecordListView.h"

#include <SDL2/SDL.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

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

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool sameColor(const Color &left, const Color &right) {
  return left.r == right.r && left.g == right.g && left.b == right.b &&
         left.a == right.a;
}

ResultRecordSummary localRecord(int id, ir::IrRecordState state,
                                std::string createdAt, int score,
                                int maxCombo, int clearRank,
                                std::optional<std::string> option) {
  ReplaySummary replay;
  replay.id = id;
  replay.createdAt = std::move(createdAt);
  replay.finalScore = score;
  replay.maxScore = 2'000;
  replay.maxCombo = maxCombo;
  replay.clearType = clearRank;
  replay.playOption = std::move(option);
  replay.initialGaugeType = GaugeType::Hard;
  replay.finalGauge = 76.5F;
  replay.irRecordState = state;
  return makeLocalResultRecord(std::move(replay));
}

ResultRecordSummary remoteRecord() {
  return {
      .identity =
          IrRemoteRecordId{
              .providerId = "tachi",
              .serverOrigin = "https://boku.tachi.ac",
              .remoteScoreId = "remote-score-44",
          },
      .capabilities =
          {
              .watch = false,
              .gBattle = false,
              .resultRecall = true,
              .videoExport = false,
              .irUpload = false,
          },
      .course = false,
      .autoPlay = false,
      .score = 1'777,
      .maxScore = 2'000,
      .maxCombo = 888,
      .clearRank = kClearTypeExHardClearRank,
      .displayedTimeUnixMillis = 1'721'377'845'000LL,
      .displayedTime = "2024-07-19 12:30:45.000",
      .playOption = "RANDOM",
      .irState = ir::IrRecordState::Uploaded,
      .local = std::nullopt,
      .remote = std::nullopt,
  };
}

Button *badge(ResultRecordListView &list) {
  auto *row = list.getViewByIndex(0);
  return row == nullptr
             ? nullptr
             : dynamic_cast<Button *>(row->findViewByName("irUploadBadge"));
}

TextView *rowText(ResultRecordListItemView &row, const std::string &name) {
  return dynamic_cast<TextView *>(row.findViewByName(name));
}

TextView *badgeText(Button &button, const std::string &name) {
  auto *content = button.getContentView();
  return content == nullptr
             ? nullptr
             : dynamic_cast<TextView *>(content->findViewByName(name));
}

void clickThroughList(ResultRecordListView &list, const Button &button) {
  SDL_Event down{};
  down.type = SDL_MOUSEBUTTONDOWN;
  down.button.type = SDL_MOUSEBUTTONDOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.which = 1;
  down.button.x = button.getX() + button.getWidth() / 2;
  down.button.y = button.getY() + button.getHeight() / 2;
  SDL_Event up = down;
  up.type = SDL_MOUSEBUTTONUP;
  up.button.type = SDL_MOUSEBUTTONUP;
  list.handleEvents(down);
  list.handleEvents(up);
}

} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for Records list");

  {
    ResultRecordListView list;
    list.setSize(700, 160);
    list.applyYogaLayout();

    int uploads = 0;
    std::optional<std::string> uploadedKey;
    int feedback = 0;
    std::optional<std::string> feedbackKey;
    int selections = 0;
    std::optional<std::string> selectedKey;
    list.onIrUploadRequested = [&](const ResultRecordSummary &record) {
      ++uploads;
      uploadedKey = record.stableKey();
    };
    list.onIrStatusFeedbackRequested =
        [&](const ResultRecordSummary &record) {
          ++feedback;
          feedbackKey = record.stableKey();
        };
    list.onSelectionChanged = [&](int index) {
      ++selections;
      selectedKey = list.get(index).stableKey();
    };

    ResultRecordSummary eligible = localRecord(
        11, ir::IrRecordState::Eligible, "2026-07-19 12:00:00", 1'500,
        700, kClearTypeHardClearRank, "MIRROR");
    list.setResultRecords({eligible});
    auto *reusedRow = dynamic_cast<ResultRecordListItemView *>(
        list.getViewByIndex(0));
    require(reusedRow != nullptr, "local eligible record binds a row");
    require(reusedRow->boundStableKey() == eligible.stableKey(),
            "eligible bind installs its stable row identity");
    require(rowText(*reusedRow, "recordTitle")->getText() ==
                eligible.displayedTime &&
                rowText(*reusedRow, "recordScore")->getText() == "1500" &&
                rowText(*reusedRow, "recordRank")->getText() == "A",
            "eligible bind installs every visible label");
    require(rowText(*reusedRow, "recordDetail")->getText().find("MIRROR") !=
                std::string::npos,
            "eligible bind installs local detail text");

    auto *irBadge = badge(list);
    require(irBadge != nullptr && irBadge->getVisible() &&
                irBadge->isEnabled(),
            "eligible local record exposes an event-consuming badge");
    require(badgeText(*irBadge, "irBadgeLabel")->getText() == "IR" &&
                badgeText(*irBadge, "irBadgeIcon")->getText() ==
                    ui_icons::textForCodepoint(0xf0ee),
            "eligible badge resets semantic text and upload glyph");
    require(reusedRow->irBadgeIconFontPath() ==
                    std::string(ui_icons::kFontAwesomeSolidPath) &&
                sameColor(reusedRow->currentIrBadgeAccent(),
                          ui_theme::amber()),
            "eligible badge resets FontAwesome font and amber color");
    require(reusedRow->irBadgeCallbackStableKey() == eligible.stableKey(),
            "eligible badge callback is bound to the current stable key");
    clickThroughList(list, *irBadge);
    require(uploads == 1 && uploadedKey == eligible.stableKey() &&
                feedback == 0 && selections == 0,
            "eligible badge uploads only its bound local row and consumes tap");

    ResultRecordSummary uploading = localRecord(
        12, ir::IrRecordState::Uploading, "2026-07-19 12:01:00", 1'200,
        500, kClearTypeEasyClearRank, "R-RANDOM");
    reusedRow->setSummary(uploading);
    auto *row = reusedRow;
    require(row == reusedRow && row->boundStableKey() == uploading.stableKey(),
            "uploading rebind reuses the row and replaces stable identity");
    irBadge = badge(list);
    require(rowText(*row, "recordTitle")->getText() ==
                    uploading.displayedTime &&
                rowText(*row, "recordScore")->getText() == "1200" &&
                rowText(*row, "recordRank")->getText() == "B" &&
                badgeText(*irBadge, "irBadgeIcon")->getText() ==
                    ui_icons::textForCodepoint(0xf2f1) &&
                sameColor(row->currentIrBadgeAccent(), ui_theme::cyan()),
            "uploading rebind replaces labels, glyph, and color");
    clickThroughList(list, *irBadge);
    require(uploads == 1 && feedback == 1 &&
                feedbackKey == uploading.stableKey() && selections == 0,
            "uploading badge clears upload action and consumes tap as status");

    ResultRecordSummary uploaded = localRecord(
        13, ir::IrRecordState::Uploaded, "2026-07-19 12:02:00", 1'900,
        900, kClearTypeFullComboRank, std::nullopt);
    reusedRow->setSummary(uploaded);
    row = reusedRow;
    irBadge = badge(list);
    require(row == reusedRow && row->boundStableKey() == uploaded.stableKey() &&
                row->irBadgeCallbackStableKey() == uploaded.stableKey() &&
                badgeText(*irBadge, "irBadgeIcon")->getText() ==
                    ui_icons::textForCodepoint(0xf00c) &&
                sameColor(row->currentIrBadgeAccent(), ui_theme::lime()),
            "uploaded local rebind replaces identity, callback, glyph, color");
    clickThroughList(list, *irBadge);
    require(uploads == 1 && feedback == 2 &&
                feedbackKey == uploaded.stableKey(),
            "uploaded local badge cannot retain prior upload callback");

    ResultRecordSummary remote = remoteRecord();
    reusedRow->setSummary(remote);
    row = reusedRow;
    irBadge = badge(list);
    require(row == reusedRow && row->boundStableKey() == remote.stableKey() &&
                row->irBadgeCallbackStableKey() == remote.stableKey(),
            "remote rebind replaces local row and badge stable identities");
    require(rowText(*row, "recordTitle")->getText() ==
                    remote.displayedTime &&
                rowText(*row, "recordScore")->getText() == "1777" &&
                rowText(*row, "recordRank")->getText() == "AA",
            "remote rebind replaces every prior local label");
    require(irBadge->getVisible() && irBadge->isEnabled() &&
                badgeText(*irBadge, "irBadgeLabel")->getText() == "IR" &&
                badgeText(*irBadge, "irBadgeIcon")->getText() ==
                    ui_icons::textForCodepoint(0xf00c) &&
                sameColor(row->currentIrBadgeAccent(), ui_theme::lime()),
            "remote row always renders the semantic uploaded IR badge");
    clickThroughList(list, *irBadge);
    require(uploads == 1 && feedback == 3 &&
                feedbackKey == remote.stableKey() && selections == 0,
            "remote badge consumes taps and can never upload a local replay");

    ResultRecordSummary hidden = localRecord(
        55, ir::IrRecordState::Hidden, {}, 0, 0, kClearTypeFailedRank,
        std::nullopt);
    reusedRow->setSummary(hidden);
    row = reusedRow;
    irBadge = badge(list);
    require(row == reusedRow && row->boundStableKey() == hidden.stableKey(),
            "hidden rebind replaces the remote stable row identity");
    require(rowText(*row, "recordTitle")->getText() == "Replay #55" &&
                rowText(*row, "recordScore")->getText() == "0" &&
                rowText(*row, "recordRank")->getText() == "F",
            "hidden rebind replaces all remote labels");
    require(irBadge != nullptr && !irBadge->getVisible() &&
                !irBadge->isEnabled() &&
                badgeText(*irBadge, "irBadgeLabel")->getText().empty() &&
                badgeText(*irBadge, "irBadgeIcon")->getText().empty() &&
                !row->irBadgeCallbackStableKey().has_value(),
            "hidden rebind clears visibility, glyphs, and badge identity");
    clickThroughList(list, *irBadge);
    require(uploads == 1 && feedback == 3,
            "hidden row cannot invoke the recycled remote callback");

    ResultRecordSummary denied = localRecord(
        66, ir::IrRecordState::Eligible, "2026-07-19 12:03:00", 1'000,
        400, kClearTypeNormalClearRank, "RANDOM");
    denied.capabilities.irUpload = false;
    reusedRow->setSummary(denied);
    row = reusedRow;
    irBadge = badge(list);
    clickThroughList(list, *irBadge);
    require(uploads == 1 && feedback == 4 &&
                feedbackKey == denied.stableKey() &&
                row->irBadgeCallbackStableKey() == denied.stableKey(),
            "eligible-looking local badge obeys explicit upload capability");

    ResultRecordListView remoteList;
    remoteList.setSize(700, 160);
    remoteList.applyYogaLayout();
    remoteList.onSelectionChanged = [&](int index) {
      ++selections;
      selectedKey = remoteList.get(index).stableKey();
    };
    remoteList.setResultRecords({remote});
    const int selectionsBeforeRemote = selections;
    remoteList.selectedIndex = 0;
    remoteList.onSelected(remoteList.get(0), 0);
    require(selections == selectionsBeforeRemote + 1,
            "remote row selection invokes the Records selection callback");
    require(selectedKey == remote.stableKey(),
            "remote row selection reports its stable identity");
    require(remoteList.selectedResultRecordIndex() == 0,
            "remote row selection retains its selected index");
    require(remoteList.get(0).capabilities.resultRecall,
            "remote row selection remains available for View Result");

    ResultRecordSummary modern;
    modern.identity = ModernChartRecordId{
        .attemptId = "123e4567-e89b-42d3-a456-426614174000"};
    modern.capabilities.irUpload = true;
    modern.irState = ir::IrRecordState::Eligible;
    modern.displayedTime = "2026-07-19 12:04:00";
    modern.score = 1'800;
    modern.maxScore = 2'000;
    ResultRecordListView modernList;
    modernList.setSize(700, 160);
    modernList.applyYogaLayout();
    std::optional<std::string> modernUpload;
    modernList.onIrUploadRequested =
        [&](const ResultRecordSummary &record) {
          modernUpload = record.stableKey();
        };
    modernList.setResultRecords({modern});
    auto *modernBadge = badge(modernList);
    require(modernBadge != nullptr && modernBadge->isEnabled(),
            "modern snapshot-backed result exposes its IR action");
    clickThroughList(modernList, *modernBadge);
    require(modernUpload == modern.stableKey(),
            "modern IR action binds the durable attempt identity");
  }

  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
