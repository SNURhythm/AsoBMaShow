#pragma once

#include "../rendering/UiSafeArea.h"
#include "../view/BlockingOverlayView.h"
#include "../view/DocumentTextReader.h"
#include "../view/ModalViewHelpers.h"

// The scene owns this full-page view; its source and range are a snapshot of
// the loaded chart, so closing never changes practice markers or playback.
class ChartSourceOverlay : public BlockingOverlayView {
public:
  ChartSourceOverlay() {
    setName("chartSourceOverlay");
    setPositionType(YGPositionTypeAbsolute);
    setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0);
    setPosition(Edge::Right, 0)->setPosition(Edge::Bottom, 0);
    setZIndex(1500);
    setFlexDirection(FlexDirection::Column)->setGap(12);
    setThemedBackgroundColor(app_theme::panelStrong);
    setVisible(false);
    title_ = new TextView(app_theme::boldFont, 24);
    title_->setHeight(32)->setFlexShrink(0);
    title_->setThemedColor(app_theme::textPrimary);
    title_->setOverflow(TextView::TextOverflow::Hidden);
    addView(title_);
    auto *actions = new View();
    actions->setFlexDirection(FlexDirection::Row)->setGap(12)->setFlexShrink(0);
    TextView *highlightLabel = nullptr;
    auto *highlight = modal_view::makeModalButton(
        i18n::message("chart_viewer.source.highlight"), 18, &highlightLabel);
    highlight->setName("chartSourceHighlight");
    highlight->setFlex(1)->setMinWidth(0);
    modal_view::styleThemedActionButton(highlight, highlightLabel, true,
        app_theme::control, app_theme::controlHover, app_theme::controlPressed, app_theme::hairline);
    highlight->setOnClickListener([this] {
      reader_->setHighlights(ranges_, true);
      status_->setVisible(ranges_.empty());
      applyYogaLayout();
    });
    actions->addView(highlight);
    TextView *closeLabel = nullptr;
    auto *close = modal_view::makeModalButton(i18n::message("readme.reader.close"), 18, &closeLabel);
    close->setName("chartSourceClose");
    close->setWidth(100);
    modal_view::styleThemedActionButton(close, closeLabel, true,
        app_theme::control, app_theme::controlHover, app_theme::controlPressed, app_theme::hairline);
    close->setOnClickListener([this] { hide(); });
    actions->addView(close);
    addView(actions);
    status_ = new TextView(app_theme::font, 18);
    status_->setLocalizedText(i18n::message("chart_viewer.source.no_match"));
    status_->setThemedColor(app_theme::textSecondary);
    status_->setHeight(28)->setFlexShrink(0);
    status_->setOverflow(TextView::TextOverflow::Hidden);
    status_->setVisible(false);
    addView(status_);
    reader_ = new DocumentTextReader();
    reader_->setName("chartSourceText");
    reader_->setFlex(1)->setMinHeight(0)->setWidthPercent(100);
    addView(reader_);
  }

  void show(std::string text, std::vector<text_range::Range> ranges,
            const std::string &title) {
    cancelPointerInput();
    ranges_ = std::move(ranges);
    title_->setText(title);
    status_->setVisible(false);
    setVisible(true);
    resize();
    reader_->setDocumentText(std::move(text));
    reader_->setHighlights({});
    applyYogaLayout();
  }
  void hide() {
    cancelPointerInput();
    setVisible(false);
  }
  void resize() {
    const auto safe = rendering::uiSafeAreaInsets();
    setPadding(Edge::Top, safe.top + 12);
    setPadding(Edge::Bottom, safe.bottom + 12);
    setPadding(Edge::Left, safe.left + 16);
    setPadding(Edge::Right, safe.right + 16);
    applyYogaLayout();
  }
  // Called before scene shortcuts, even while hidden, to swallow closing
  // button releases and repeated Escape events from the same key press.
  bool consumeEvent(SDL_Event &event) {
    if (closingEscape_ && (event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_KEY_DOWN) &&
        event.key.key == SDLK_ESCAPE) {
      if (event.type == SDL_EVENT_KEY_UP) closingEscape_ = false;
      return true;
    }
    if (closingGamepad_ && event.type == SDL_EVENT_GAMEPAD_BUTTON_UP &&
        event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
      closingGamepad_ = false;
      return true;
    }
    if (!getVisible()) return false;
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
      closingEscape_ = true;
      hide();
      return true;
    }
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
        event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
      closingGamepad_ = true;
      hide();
      return true;
    }
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
      double offset = reader_->scrollOffset;
      SDL_Keycode key = event.type == SDL_EVENT_KEY_DOWN ? event.key.key : SDLK_UNKNOWN;
      if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) key = SDLK_UP;
        if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) key = SDLK_DOWN;
      }
      switch (key) {
      case SDLK_UP: offset -= reader_->itemHeight; break;
      case SDLK_DOWN: offset += reader_->itemHeight; break;
      case SDLK_PAGEUP: offset -= reader_->getContentHeight(); break;
      case SDLK_PAGEDOWN: offset += reader_->getContentHeight(); break;
      case SDLK_HOME: offset = 0; break;
      case SDLK_END: offset = reader_->size() * reader_->itemHeight; break;
      default: break;
      }
      reader_->scrollOffset = std::clamp(offset, 0.0,
          static_cast<double>(std::max(0, reader_->size() * reader_->itemHeight - reader_->getContentHeight())));
      reader_->rebindVisibleItems();
    }
    (void)handleEvents(event);
    return true;
  }
private:
  DocumentTextReader *reader_ = nullptr;
  TextView *title_ = nullptr;
  TextView *status_ = nullptr;
  std::vector<text_range::Range> ranges_;
  bool closingEscape_ = false;
  bool closingGamepad_ = false;
};
