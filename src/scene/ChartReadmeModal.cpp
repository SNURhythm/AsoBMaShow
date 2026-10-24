#include "ChartReadmeModal.h"
#include "../library/ChartReadme.h"
#include "../path.h"
#include "../i18n/Localization.h"
#include "../rendering/UiSafeArea.h"
#include "../view/AppTheme.h"
#include "../view/BlockingOverlayView.h"
#include "../view/DropdownView.h"
#include "../view/ModalViewHelpers.h"
#include "../view/OverlayPortal.h"
#include "../view/RecyclerView.h"
#include <algorithm>

namespace {
class ReadmeMeasurer : public TextView {
public:
  ReadmeMeasurer() : TextView(app_theme::font, 20) { setDeferredTextureMaterialization(true); }
  int lineHeight() const { return textLineHeight(); }
};
i18n::Text errorText(chart_readme::Error error) {
  using E = chart_readme::Error;
  switch (error) {
  case E::TooLarge: return i18n::message("readme.reader.too_large");
  case E::TooManyDocuments: return i18n::message("readme.reader.too_many");
  case E::Encoding: return i18n::message("readme.reader.encoding_error");
  default: return i18n::message("readme.reader.read_error");
  }
}
}

struct ChartReadmeModal::Impl {
  chart_readme::Loader loader;
  BlockingOverlayView *root = nullptr;
  View *panel = nullptr;
  TextView *title = nullptr;
  TextView *status = nullptr;
  DropdownView *selector = nullptr;
  DropdownView *encodingSelector = nullptr;
  OverlayPortal *portal = nullptr;
  RecyclerView<std::string> *lines = nullptr;
  Button *close = nullptr;
  ReadmeMeasurer measure;
  DropdownView::State selection;
  DropdownView::State encodingSelection;
  chart_readme::Encoding encoding = chart_readme::Encoding::Auto;
  std::vector<std::filesystem::path> documents;
  std::string text;
  int width = rendering::window_width, height = rendering::window_height;
  int textWidth = 0;
  bool loading = false;
  SDL_Keycode closingKey = 0;
  bool closingGamepad = false;

  void reflow(bool resetScroll) {
    const int available = std::max(1, lines->getVisibleItemWidth() - 4);
    if (available == textWidth && !resetScroll) return;
    textWidth = available;
    const double oldOffset = resetScroll ? 0 : lines->scrollOffset;
    auto wrapped = text.empty() ? std::vector<std::string>{} : chart_readme::wrapLines(
        text, available, [&](const auto &line) { return measure.measureTextWidth(line); });
    lines->itemHeight = std::max(30, measure.lineHeight() + 4);
    lines->setItems(std::move(wrapped));
    lines->scrollOffset = std::clamp(oldOffset, 0.0,
        static_cast<double>(std::max(0, lines->size() * lines->itemHeight - lines->getHeight())));
    lines->rebindVisibleItems();
  }
  void loadSelected() {
    const auto found = std::ranges::find_if(selection.options,
        [&](const auto &option) { return option.id == selection.selectedId; });
    if (found == selection.options.end()) return;
    const auto index = static_cast<std::size_t>(found - selection.options.begin());
    beginLoad();
    loader.select(documents.at(index), encoding);
  }
  void beginLoad() {
    loading = true;
    text.clear();
    lines->clear();
    status->setLocalizedText(i18n::message("readme.reader.loading"));
    status->setVisible(true);
    ui::requestAnimationFrame();
  }
};

ChartReadmeModal::ChartReadmeModal(View *parent) : impl_(std::make_unique<Impl>()) {
  auto &v = *impl_;
  v.root = new BlockingOverlayView();
  v.root->setPositionType(YGPositionTypeAbsolute);
  v.root->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0);
  v.root->setZIndex(1000);
  v.root->setFlexDirection(FlexDirection::Column);
  v.root->setAlignItems(YGAlignCenter)->setJustifyContent(YGJustifyCenter);
  v.root->setThemedBackgroundColor(app_theme::scrim);
  v.root->setVisible(false);
  parent->addView(v.root);
  v.panel = new View();
  v.panel->setFlexDirection(FlexDirection::Column)->setGap(12)->setPadding(Edge::All, 16);
  v.panel->setThemedBackgroundColor(app_theme::panelStrong);
  v.panel->setCornerRadius(app_theme::panelRadius());
  v.root->addView(v.panel);
  v.title = new TextView(app_theme::boldFont, 24);
  v.title->setHeight(36)->setFlexShrink(0);
  v.title->setThemedColor(app_theme::textPrimary);
  v.title->setOverflow(TextView::TextOverflow::Hidden);
  v.panel->addView(v.title);
  v.portal = new OverlayPortal();
  v.portal->setPositionType(YGPositionTypeAbsolute);
  v.portal->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0);
  v.portal->setZIndex(2000);
  v.root->addView(v.portal);
  v.selector = new DropdownView({
      .onOpenChanged = [this](bool open) { impl_->selection.open = open; },
      .onOptionSelected = [this](const std::string &id) {
        auto &v = *impl_;
        const auto found = std::ranges::find_if(v.selection.options,
            [&](const auto &option) { return option.id == id; });
        if (found == v.selection.options.end()) return;
        v.selection.selectedId = id;
        v.selection.open = false;
        v.selector->refresh(v.selection);
        v.loadSelected();
      }}, v.portal);
  v.selector->setHeight(44)->setFlexShrink(0);
  v.panel->addView(v.selector);
  v.encodingSelector = new DropdownView({
      .onOpenChanged = [this](bool open) { impl_->encodingSelection.open = open; },
      .onOptionSelected = [this](const std::string &id) {
        auto &v = *impl_;
        v.encoding = id == "utf8" ? chart_readme::Encoding::Utf8 :
                     id == "japanese" ? chart_readme::Encoding::Japanese :
                     id == "korean" ? chart_readme::Encoding::Korean : chart_readme::Encoding::Auto;
        v.encodingSelection.selectedId = id;
        v.encodingSelection.open = false;
        v.encodingSelector->refresh(v.encodingSelection);
        v.loadSelected();
      }}, v.portal);
  v.encodingSelector->setHeight(44)->setFlexShrink(0);
  v.panel->addView(v.encodingSelector);
  v.status = new TextView(app_theme::font, 18);
  v.status->setThemedColor(app_theme::textSecondary);
  v.status->setWrap(true);
  v.status->setMinHeight(28)->setFlexShrink(0);
  v.panel->addView(v.status);
  v.lines = new RecyclerView<std::string>([](const auto &a, const auto &b) { return a == b; });
  v.lines->setName("chartReadmeLines");
  v.lines->setWidthPercent(100)->setFlex(1)->setMinHeight(0);
  v.lines->reserveScrollbarGutter = true;
  v.lines->isSelectable = [](const auto &, int) { return false; };
  v.lines->onCreateView = [](const std::string &) {
    auto *line = new TextView(app_theme::font, 20);
    line->setDeferredTextureMaterialization(true);
    line->setThemedColor(app_theme::textPrimary);
    line->setOverflow(TextView::TextOverflow::Hidden);
    return line;
  };
  v.lines->onBind = [](View *view, const std::string &line, int, bool) {
    static_cast<TextView *>(view)->setText(line);
  };
  v.panel->addView(v.lines);
  v.close = modal_view::makeModalButton(i18n::message("readme.reader.close"), 20);
  modal_view::styleThemedActionButton(v.close, static_cast<TextView *>(v.close->getContentView()),
      true, app_theme::control, app_theme::controlHover, app_theme::controlPressed, app_theme::hairlineStrong);
  v.close->setHeight(44)->setFlexShrink(0)->setWidthPercent(100);
  v.close->setOnClickListener([this] { hide(); });
  v.panel->addView(v.close);
}
std::unique_ptr<ChartReadmeModal> ChartReadmeModal::Create(View *parent) {
  return parent ? std::unique_ptr<ChartReadmeModal>(new ChartReadmeModal(parent)) : nullptr;
}
ChartReadmeModal::~ChartReadmeModal() {
  hide();
  impl_->close->setOnClickListener(nullptr);
}
void ChartReadmeModal::show(const std::filesystem::path &chart, const std::string &title) {
  auto &v = *impl_;
  v.documents.clear();
  v.selection = {.label = i18n::message("readme.reader.file"), .enabled = false};
  v.selector->refresh(v.selection);
  v.encoding = chart_readme::Encoding::Auto;
  v.encodingSelection = {
      .label = i18n::message("readme.reader.encoding"), .selectedId = "auto",
      .options = {{"auto", i18n::message("readme.reader.encoding_auto")},
                  {"utf8", "UTF-8"},
                  {"japanese", i18n::message("readme.reader.encoding_japanese")},
                  {"korean", i18n::message("readme.reader.encoding_korean")}}, .enabled = false};
  v.encodingSelector->refresh(v.encodingSelection);
  v.title->setLocalizedText(i18n::message("readme.reader.title", {{"title", title}}));
  v.root->setVisible(true);
  v.beginLoad();
  resize(v.width, v.height);
  v.loader.open(chart);
}
void ChartReadmeModal::hide() {
  auto &v = *impl_;
  v.loader.cancel();
  v.loading = false;
  v.selection.open = false;
  v.selector->refresh(v.selection);
  v.encodingSelection.open = false;
  v.encodingSelector->refresh(v.encodingSelection);
  v.root->setVisible(false);
}
bool ChartReadmeModal::isVisible() const { return impl_->root->getVisible(); }
void ChartReadmeModal::resize(int width, int height) {
  auto &v = *impl_;
  v.width = width; v.height = height;
  if (!isVisible()) return;
  const auto safe = rendering::uiSafeAreaInsets();
  const int panelWidth = std::max(1, std::min(1000, width - safe.left - safe.right - 24));
  const int panelHeight = std::max(1, std::min(900, height - safe.top - safe.bottom - 24));
  v.root->setSize(width, height);
  v.root->setPadding(Edge::Left, safe.left)->setPadding(Edge::Right, safe.right);
  v.root->setPadding(Edge::Top, safe.top)->setPadding(Edge::Bottom, safe.bottom);
  v.panel->setSize(panelWidth, panelHeight);
  v.portal->setSize(width, height);
  v.selector->setTriggerWidth(std::max(1, panelWidth - 32));
  v.encodingSelector->setTriggerWidth(std::max(1, panelWidth - 32));
  v.root->applyYogaLayout();
  v.reflow(false);
}
void ChartReadmeModal::update() {
  auto &v = *impl_;
  if (!isVisible()) return;
  if (auto result = v.loader.takeResult()) {
    v.loading = false;
    if (result->discovered) {
      v.documents = std::move(result->documents.paths);
      v.selection.options.clear();
      for (std::size_t i = 0; i < v.documents.size(); ++i)
        v.selection.options.push_back({.id = std::to_string(i), .label = fspath_to_utf8(v.documents[i].filename())});
      v.selection.selectedId = v.documents.empty() ? "" : "0";
      v.selection.enabled = !v.documents.empty();
      v.selector->refresh(v.selection);
      v.encodingSelection.enabled = !v.documents.empty();
      v.encodingSelector->refresh(v.encodingSelection);
    }
    const auto error = result->documents.error != chart_readme::Error::None
        ? result->documents.error : result->content.error;
    if (error != chart_readme::Error::None) v.status->setLocalizedText(errorText(error));
    else if (v.documents.empty()) v.status->setLocalizedText(i18n::message("readme.reader.none"));
    else if (result->content.text.empty()) v.status->setLocalizedText(i18n::message("readme.reader.empty"));
    else {
      v.text = std::move(result->content.text);
      v.status->setVisible(false);
    }
    if (error != chart_readme::Error::None || v.text.empty()) v.status->setVisible(true);
    v.root->applyYogaLayout();
    v.reflow(true);
  }
  if (v.loading) ui::requestAnimationFrame();
}
bool ChartReadmeModal::handleEvents(SDL_Event &event) {
  auto &v = *impl_;
  if (event.type == SDL_EVENT_KEY_UP && v.closingKey && event.key.key == v.closingKey) {
    v.closingKey = 0; return false;
  }
  if (event.type == SDL_EVENT_GAMEPAD_BUTTON_UP && v.closingGamepad && event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
    v.closingGamepad = false; return false;
  }
  if (!isVisible()) return true;
  if (event.type == SDL_EVENT_KEY_DOWN && (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_AC_BACK)) {
    v.closingKey = event.key.key;
    if (v.selection.open) { v.selection.open = false; v.selector->refresh(v.selection); }
    else if (v.encodingSelection.open) { v.encodingSelection.open = false; v.encodingSelector->refresh(v.encodingSelection); }
    else hide();
    return false;
  }
  if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
    v.closingGamepad = true; hide(); return false;
  }
  if (event.type == SDL_EVENT_KEY_DOWN && !v.selection.open && !v.encodingSelection.open) {
    double offset = v.lines->scrollOffset;
    switch (event.key.key) {
    case SDLK_UP: offset -= v.lines->itemHeight; break;
    case SDLK_DOWN: offset += v.lines->itemHeight; break;
    case SDLK_PAGEUP: offset -= v.lines->getHeight(); break;
    case SDLK_PAGEDOWN: offset += v.lines->getHeight(); break;
    case SDLK_HOME: offset = 0; break;
    case SDLK_END: offset = v.lines->size() * v.lines->itemHeight; break;
    default: (void)v.root->handleEvents(event); return false;
    }
    v.lines->scrollOffset = std::clamp(offset, 0.0,
        static_cast<double>(std::max(0, v.lines->size() * v.lines->itemHeight - v.lines->getHeight())));
    v.lines->rebindVisibleItems();
    return false;
  }
  (void)v.root->handleEvents(event);
  return false;
}
