#include "../src/view/Button.h"
#include "../src/view/IconText.h"
#include "../src/view/ReplaySummaryListView.h"
#include "../src/rendering/UniformCache.h"

#include <SDL2/SDL.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

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

ReplaySummary summary(int id,
                      ir::IrRecordState state = ir::IrRecordState::Eligible) {
  ReplaySummary value;
  value.id = id;
  value.createdAt = "2026-07-19 12:00 UTC";
  value.finalScore = 1500;
  value.maxScore = 2000;
  value.maxCombo = 700;
  value.irRecordState = state;
  return value;
}

Button *uploadButton(ReplaySummaryListView &list, int index) {
  auto *row = list.getViewByIndex(index);
  return row == nullptr
             ? nullptr
             : dynamic_cast<Button *>(row->findViewByName("irUploadBadge"));
}

TextView *badgeText(Button &button, const std::string &name) {
  auto *content = button.getContentView();
  return content == nullptr
             ? nullptr
             : dynamic_cast<TextView *>(content->findViewByName(name));
}

void clickThroughList(ReplaySummaryListView &list, const Button &button) {
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
  require(bgfx::init(init), "headless bgfx initializes for replay list");

  {
    ReplaySummaryListView list;
    list.setSize(700, 160);
    list.applyYogaLayout();

    int uploads = 0;
    int uploadedReplayId = 0;
    int statusFeedback = 0;
    ir::IrRecordState feedbackState = ir::IrRecordState::Hidden;
    int selections = 0;
    list.onIrUploadRequested = [&](const ReplaySummary &value) {
      ++uploads;
      uploadedReplayId = value.id;
    };
    list.onIrStatusFeedbackRequested = [&](const ReplaySummary &value) {
      ++statusFeedback;
      feedbackState = value.irRecordState;
    };
    list.onSelectionChanged = [&](int) { ++selections; };
    constexpr std::array expected{
        std::pair{ir::IrRecordState::Eligible, 0xf0ee},
        std::pair{ir::IrRecordState::Queued, 0xf017},
        std::pair{ir::IrRecordState::Uploading, 0xf2f1},
        std::pair{ir::IrRecordState::AwaitingRemote, 0xf252},
        std::pair{ir::IrRecordState::Blocked, 0xf084},
        std::pair{ir::IrRecordState::Failed, 0xf071},
        std::pair{ir::IrRecordState::Uploaded, 0xf00c},
    };

    ReplaySummaryListItemView *reusedRow = nullptr;
    for (const auto &[state, codepoint] : expected) {
      list.setReplaySummaries({summary(11, state)});
      auto *row = dynamic_cast<ReplaySummaryListItemView *>(
          list.getViewByIndex(0));
      require(row != nullptr, "IR state row is bound");
      if (reusedRow == nullptr) {
        reusedRow = row;
      }
      require(row == reusedRow,
              "IR state matrix reuses one virtualized row view");

      auto *button = uploadButton(list, 0);
      require(button != nullptr && button->getVisible(),
              "non-hidden IR state exposes a badge");
      require(button->isEnabled(),
              "non-hidden IR badge remains a pointer event sink");
      auto *label = badgeText(*button, "irBadgeLabel");
      auto *icon = badgeText(*button, "irBadgeIcon");
      require(label != nullptr && label->getText() == "IR",
              "IR badge label uses normal-font text");
      require(icon != nullptr &&
                  icon->getText() == ui_icons::textForCodepoint(codepoint),
              "IR badge icon matches the semantic state codepoint");
      require(row->irBadgeIconFontPath() ==
                  std::string(ui_icons::kFontAwesomeSolidPath),
              "IR badge icon uses the FontAwesome solid font");

      const int previousUploads = uploads;
      const int previousFeedback = statusFeedback;
      clickThroughList(list, *button);
      const bool actionable = state == ir::IrRecordState::Eligible ||
                              state == ir::IrRecordState::Failed;
      require(uploads == previousUploads + (actionable ? 1 : 0),
              "only eligible and failed IR badges request upload");
      if (actionable) {
        require(uploadedReplayId == 11,
                "actionable badge dispatches its bound replay");
        require(statusFeedback == previousFeedback,
                "actionable badge does not emit status-only feedback");
      } else {
        require(statusFeedback == previousFeedback + 1 &&
                    feedbackState == state,
                "non-actionable badge emits bounded status feedback");
      }
      require(selections == 0 && list.selectedReplayIndex() == -1,
              "every visible IR badge consumes row-selection events");
    }

    list.setReplaySummaries(
        {summary(11, ir::IrRecordState::AwaitingRemote)});
    auto *button = uploadButton(list, 0);
    require(button != nullptr,
            "awaiting-remote badge is available before hiding");
    const int uploadsBeforeHidden = uploads;
    list.setReplaySummaries({summary(11, ir::IrRecordState::Hidden)});
    require(dynamic_cast<ReplaySummaryListItemView *>(list.getViewByIndex(0)) ==
                reusedRow,
            "remote-to-hidden transition reuses the same row view");
    button = uploadButton(list, 0);
    require(button != nullptr && !button->getVisible() &&
                !button->isEnabled(),
            "hidden IR state clears badge visibility and clickability");
    auto *label = badgeText(*button, "irBadgeLabel");
    auto *icon = badgeText(*button, "irBadgeIcon");
    require(label != nullptr && label->getText().empty(),
            "hidden rebind clears the normal-font badge text");
    require(icon != nullptr && icon->getText().empty(),
            "hidden rebind clears the FontAwesome icon");
    clickThroughList(list, *button);
    require(uploads == uploadsBeforeHidden,
            "hidden rebind does not leak the prior badge callback");
  }

  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
