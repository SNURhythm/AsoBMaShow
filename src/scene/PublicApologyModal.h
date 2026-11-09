#pragma once

#include "PublicApology.h"
#include <tuple>
#include "../ArchiveFile.h"
#include "../rendering/UiSafeArea.h"
#include "../view/BlockingOverlayView.h"
#include "../view/DocumentTextReader.h"
#include "../view/ModalViewHelpers.h"

class PublicApologyModal final : public BlockingOverlayView {
public:
  PublicApologyModal() {
    setName("publicApologyModal");
    setPositionType(YGPositionTypeAbsolute);
    setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0);
    setZIndex(1500);
    setFlexDirection(FlexDirection::Column)->setGap(12);
    setThemedBackgroundColor(app_theme::panelStrong);
    setVisible(false);
    auto *title = new TextView(app_theme::boldFont, 26);
    title->setLocalizedText(i18n::message("intro.apology.label"));
    title->setThemedColor(app_theme::textPrimary);
    title->setAutoFitText(true);
    title->setHeight(38)->setFlexShrink(0);
    addView(title);
    reader_ = new DocumentTextReader();
    reader_->setName("publicApologyText");
    reader_->setFlex(1)->setMinHeight(0)->setWidthPercent(100);
    addView(reader_);
    auto *actions = new View();
    actions->setFlexDirection(FlexDirection::Row)->setGap(12)->setHeight(48)->setFlexShrink(0);
    original_ = modal_view::makeModalButton(i18n::message("intro.apology.original"), 18, &originalLabel_);
    original_->setName("publicApologyOriginal");
    original_->setOnClickListener([this] { toggleOriginal(); });
    close_ = modal_view::makeModalButton(i18n::message("readme.reader.close"), 18, &closeLabel_);
    close_->setName("publicApologyClose");
    close_->setOnClickListener([this] { hide(); });
    for (auto *button : {original_, close_}) {
      button->setWidth(0)->setFlex(1)->setMinWidth(0)->setHeight(48);
      static_cast<TextView *>(button->getContentView())->setAutoFitText(true);
      actions->addView(button);
    }
    addView(actions);
    styleActions();
  }

  void show() {
    cancelPointerInput();
    language_ = i18n::language();
    originalShown_ = language_ == i18n::Language::Korean;
    original_->setVisible(!originalShown_);
    original_->setDisplay(originalShown_ ? YGDisplayNone : YGDisplayFlex);
    originalFocused_ = false;
    styleActions();
    setVisible(true);
    loadText();
  }
  void hide() {
    cancelPointerInput();
    setVisible(false);
  }
  void resize(int width, int height) {
    const auto safe = rendering::uiSafeAreaInsets();
    setSize(width, height);
    // Keep long paragraphs readable on a desktop without shrinking phone text.
    const int available = std::max(0, width - safe.left - safe.right);
    const int inset = std::max(16, (available - 960) / 2);
    setPadding(Edge::Top, safe.top + 12)->setPadding(Edge::Bottom, safe.bottom + 12);
    setPadding(Edge::Left, safe.left + inset)->setPadding(Edge::Right, safe.right + inset);
    applyYogaLayout();
  }
  bool consumeEvent(SDL_Event &event) {
    if (!getVisible()) return false;
    if (event.type == SDL_EVENT_KEY_DOWN) {
      switch (event.key.key) {
      case SDLK_ESCAPE:
      case SDLK_AC_BACK: hide(); return true;
      case SDLK_TAB:
        if (!event.key.repeat && language_ != i18n::Language::Korean) {
          originalFocused_ = !originalFocused_;
          styleActions();
        }
        return true;
      case SDLK_RETURN:
      case SDLK_KP_ENTER:
        if (!event.key.repeat) {
          if (originalFocused_) toggleOriginal(); else hide();
        }
        return true;
      case SDLK_UP: scroll(-reader_->itemHeight); return true;
      case SDLK_DOWN: scroll(reader_->itemHeight); return true;
      case SDLK_PAGEUP: scroll(-reader_->getHeight()); return true;
      case SDLK_PAGEDOWN: scroll(reader_->getHeight()); return true;
      case SDLK_HOME: scroll(-reader_->size() * reader_->itemHeight); return true;
      case SDLK_END: scroll(reader_->size() * reader_->itemHeight); return true;
      default: break;
      }
    }
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
      switch (event.gbutton.button) {
      case SDL_GAMEPAD_BUTTON_EAST: hide(); return true;
      case SDL_GAMEPAD_BUTTON_DPAD_UP: scroll(-reader_->itemHeight); return true;
      case SDL_GAMEPAD_BUTTON_DPAD_DOWN: scroll(reader_->itemHeight); return true;
      case SDL_GAMEPAD_BUTTON_WEST: toggleOriginal(); return true;
      default: break;
      }
    }
    handleEvents(event);
    return true;
  }

private:
  void scroll(double delta) {
    reader_->cancelPointerInput();
    reader_->scrollOffset = std::clamp(reader_->scrollOffset + delta, 0.0,
        static_cast<double>(std::max(0, reader_->size() * reader_->itemHeight - reader_->getContentHeight())));
    reader_->onLayout();
    ui::invalidate();
  }
  void toggleOriginal() {
    if (language_ == i18n::Language::Korean) return;
    originalShown_ = !originalShown_;
    loadText();
  }
  void loadText() {
    originalLabel_->setLocalizedText(i18n::message(
        originalShown_ ? "intro.apology.translation" : "intro.apology.original"));
    const auto path = public_apology::assetPath(originalShown_ ? i18n::Language::Korean : language_);
    std::vector<unsigned char> bytes;
    std::string error;
    if (archive_file::readFileBounded(path, bytes, 256 * 1024, &error))
      reader_->setDocumentText(std::string(bytes.begin(), bytes.end()));
    else
      reader_->setDocumentText(i18n::tr("intro.apology.read_error"));
    applyYogaLayout();
  }
  void styleActions() {
    for (auto [button, label, selected] : {
        std::tuple{original_, originalLabel_, originalFocused_},
        std::tuple{close_, closeLabel_, !originalFocused_}}) {
      button->setSelected(selected);
      modal_view::styleThemedActionButton(button, label, true,
          selected ? app_theme::primaryAction : app_theme::control,
          selected ? app_theme::primaryActionHover : app_theme::controlHover,
          selected ? app_theme::primaryActionPressed : app_theme::controlPressed,
          app_theme::hairlineStrong);
    }
  }
  DocumentTextReader *reader_ = nullptr;
  Button *original_ = nullptr, *close_ = nullptr;
  TextView *originalLabel_ = nullptr, *closeLabel_ = nullptr;
  i18n::Language language_ = i18n::Language::English;
  bool originalShown_ = false, originalFocused_ = false;
};
