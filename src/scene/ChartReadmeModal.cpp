#include "ChartReadmeModal.h"
#include "../library/ChartReadme.h"
#include "../library/ReadmeTranslation.h"
#include "../platform/SDLMainThread.h"
#include "../path.h"
#include "../i18n/Localization.h"
#include "../rendering/UiSafeArea.h"
#include "../view/AppTheme.h"
#include "../view/BlockingOverlayView.h"
#include "../view/DropdownView.h"
#include "../view/ModalViewHelpers.h"
#include "../view/OverlayPortal.h"
#include "../view/DocumentTextReader.h"
#include "../PlatformOpen.h"
#include <algorithm>
#if TARGET_OS_ANDROID
#include "../view/ImageView.h"
#endif

namespace {
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
  readme_translation::Request translation;
  std::string originalText, translatedText;
  bool translating = false, showingTranslation = false;
  Button *copy = nullptr, *translate = nullptr, *original = nullptr, *translated = nullptr;
  View *modes = nullptr;
  View *quality = nullptr;
  Button *standard = nullptr, *highFidelity = nullptr;
  bool preferHighFidelity = false;
  View *attribution = nullptr;
  DropdownView *targetSelector = nullptr, *sourceSelector = nullptr;
  DropdownView::State sourceSelection;
  DropdownView::State targetSelection;
  BlockingOverlayView *root = nullptr;
  View *panel = nullptr;
  TextView *title = nullptr;
  TextView *status = nullptr;
  DropdownView *selector = nullptr;
  DropdownView *encodingSelector = nullptr;
  OverlayPortal *portal = nullptr;
  DocumentTextReader *lines = nullptr;
  Button *close = nullptr;
  DropdownView::State selection;
  DropdownView::State encodingSelection;
  chart_readme::Encoding encoding = chart_readme::Encoding::Auto;
  std::vector<std::filesystem::path> documents;
  std::string text;
  int width = rendering::window_width, height = rendering::window_height;
  bool loading = false;
  SDL_Keycode closingKey = 0;
  bool closingGamepad = false;

  void setStatusVisible(bool visible) {
    status->setVisible(visible);
    status->setDisplay(visible ? YGDisplayFlex : YGDisplayNone);
  }
  void cancelTranslation() {
    const auto id = translation.id();
    translation.cancel();
    readme_translation::stop(id);
    translating = false;
  }
  void refreshActions() {
    copy->setEnabled(!loading && !text.empty());
    translate->setEnabled(!loading && !originalText.empty());
    static_cast<TextView *>(translate->getContentView())->setLocalizedText(i18n::message(
        translating ? "readme.translation.cancel" :
#if TARGET_OS_ANDROID
        "readme.translation.google"
#else
        "readme.translation.translate"
#endif
    ));
    modes->setVisible(!translatedText.empty());
    modes->setDisplay(translatedText.empty() ? YGDisplayNone : YGDisplayFlex);
    original->setSelected(!showingTranslation);
    translated->setSelected(showingTranslation);
    standard->setSelected(!preferHighFidelity);
    highFidelity->setSelected(preferHighFidelity);
    for (auto *button : {original, translated, standard, highFidelity}) {
      modal_view::styleThemedActionButton(button, static_cast<TextView *>(button->getContentView()), true,
          button->isSelected() ? app_theme::primaryAction : app_theme::control,
          button->isSelected() ? app_theme::primaryActionHover : app_theme::controlHover,
          button->isSelected() ? app_theme::primaryActionPressed : app_theme::controlPressed,
          app_theme::hairlineStrong);
    }
    attribution->setVisible(showingTranslation);
    attribution->setDisplay(showingTranslation ? YGDisplayFlex : YGDisplayNone);
  }
  void showText(bool translatedMode) {
    showingTranslation = translatedMode;
    text = translatedMode ? translatedText : originalText;
    setStatusVisible(false);
    refreshActions();
    root->applyYogaLayout();
    reflow(true);
  }
  void reflow(bool resetScroll) {
    lines->setDocumentText(text, resetScroll);
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
    cancelTranslation();
    originalText.clear(); translatedText.clear(); showingTranslation = false;
    loading = true;
    text.clear();
    refreshActions();
    lines->setDocumentText("");
    status->setLocalizedText(i18n::message("readme.reader.loading"));
    setStatusVisible(true);
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
  v.panel->setFlexDirection(FlexDirection::Column)->setGap(8)->setPadding(Edge::All, 12);
  v.panel->setThemedBackgroundColor(app_theme::panelStrong);
  v.panel->setCornerRadius(app_theme::panelRadius());
  v.root->addView(v.panel);
  v.title = new TextView(app_theme::boldFont, 24);
  v.title->setHeight(32)->setFlexShrink(0);
  v.title->setThemedColor(app_theme::textPrimary);
  v.title->setOverflow(TextView::TextOverflow::Hidden);
  v.panel->addView(v.title);
  v.portal = new OverlayPortal();
  v.portal->setPositionType(YGPositionTypeAbsolute);
  v.portal->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0);
  v.portal->setZIndex(2000);
  v.root->addView(v.portal);
  auto *settings = new View();
  settings->setFlexDirection(FlexDirection::Row)->setGap(8)->setFlexShrink(0);
  v.panel->addView(settings);
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
  v.selector->setHeight(40)->setFlex(1)->setMinWidth(0);
  settings->addView(v.selector);
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
  v.encodingSelector->setName("chartReadmeEncoding");
  v.encodingSelector->setHeight(40)->setFlex(1)->setMinWidth(0);
  settings->addView(v.encodingSelector);
  auto *languages = new View();
  languages->setFlexDirection(FlexDirection::Row)->setGap(8)->setFlexShrink(0);
  v.panel->addView(languages);
  v.sourceSelector = new DropdownView({
      .onOpenChanged = [this](bool open) { impl_->sourceSelection.open = open; },
      .onOptionSelected = [this](const std::string &id) {
        auto &v = *impl_;
        v.cancelTranslation();
        v.translatedText.clear();
        v.sourceSelection.selectedId = id;
        v.sourceSelection.open = false;
        v.sourceSelector->refresh(v.sourceSelection);
        v.showText(false);
      }}, v.portal);
  v.sourceSelector->setName("chartReadmeSource");
  v.sourceSelector->setHeight(40)->setFlex(1)->setMinWidth(0);
  languages->addView(v.sourceSelector);
  v.targetSelector = new DropdownView({
      .onOpenChanged = [this](bool open) { impl_->targetSelection.open = open; },
      .onOptionSelected = [this](const std::string &id) {
        auto &v = *impl_;
        v.cancelTranslation();
        v.translatedText.clear();
        v.targetSelection.selectedId = id;
        v.targetSelection.open = false;
        v.targetSelector->refresh(v.targetSelection);
        v.showText(false);
      }}, v.portal);
  v.targetSelector->setHeight(40)->setFlex(1)->setMinWidth(0);
  languages->addView(v.targetSelector);
  const auto button = [](const char *key, const char *name) {
    auto *result = modal_view::makeModalButton(i18n::message(key), 18);
    result->setName(name);
    modal_view::styleThemedActionButton(result, static_cast<TextView *>(result->getContentView()),
        true, app_theme::control, app_theme::controlHover, app_theme::controlPressed, app_theme::hairlineStrong);
    result->setHeight(40)->setFlex(1)->setMinWidth(0);
    return result;
  };
  v.quality = new View();
  v.quality->setName("chartReadmeQuality");
  v.quality->setFlexDirection(FlexDirection::Row)->setGap(8)->setHeight(40)->setFlexShrink(0);
  const bool qualitySupported = readme_translation::qualitySelectionSupported();
  v.quality->setVisible(qualitySupported);
  v.quality->setDisplay(qualitySupported ? YGDisplayFlex : YGDisplayNone);
  v.standard = button("readme.translation.standard", "chartReadmeStandard");
  v.highFidelity = button("readme.translation.high_fidelity", "chartReadmeHighFidelity");
  v.quality->addView(v.standard); v.quality->addView(v.highFidelity);
  v.panel->addView(v.quality);
  const auto selectQuality = [this](bool highFidelity) {
    auto &v = *impl_;
    if (v.preferHighFidelity == highFidelity) return;
    v.cancelTranslation();
    v.translatedText.clear();
    v.preferHighFidelity = highFidelity;
    v.showText(false);
  };
  v.standard->setOnClickListener([selectQuality] { selectQuality(false); });
  v.highFidelity->setOnClickListener([selectQuality] { selectQuality(true); });
  auto *actions = new View();
  actions->setFlexDirection(FlexDirection::Row)->setGap(8)->setHeight(40)->setFlexShrink(0);
  v.panel->addView(actions);
  v.copy = button("readme.translation.copy", "chartReadmeCopy");
  v.translate = button("readme.translation.translate", "chartReadmeTranslate");
  actions->addView(v.copy); actions->addView(v.translate);
  v.copy->setOnClickListener([this] {
    auto &v = *impl_;
    const bool copied = platform::sdlMain<SDL_SetClipboardText>(v.text.c_str());
    v.status->setLocalizedText(i18n::message(copied ? "readme.translation.copied" : "readme.translation.copy_error"));
    v.setStatusVisible(true);
    v.root->applyYogaLayout(); v.reflow(false);
  });
  v.translate->setOnClickListener([this] {
    auto &v = *impl_;
    if (v.translating) {
      v.cancelTranslation(); v.setStatusVisible(false);
    } else if (!readme_translation::supported()) {
      v.status->setLocalizedText(i18n::message("readme.translation.unavailable")); v.setStatusVisible(true);
    } else {
      v.showText(false);
      v.translating = true;
      v.status->setLocalizedText(i18n::message("readme.translation.preparing")); v.setStatusVisible(true);
      readme_translation::start(v.translation.begin(), v.originalText, v.targetSelection.selectedId, v.sourceSelection.selectedId, v.preferHighFidelity);
      ui::requestAnimationFrame();
    }
    v.refreshActions(); v.root->applyYogaLayout(); v.reflow(false);
  });
  v.modes = new View();
  v.modes->setFlexDirection(FlexDirection::Row)->setGap(8)->setHeight(40)->setFlexShrink(0);
  v.panel->addView(v.modes);
  v.original = button("readme.translation.original", "chartReadmeOriginal");
  v.translated = button("readme.translation.translated", "chartReadmeTranslated");
  v.modes->addView(v.original); v.modes->addView(v.translated);
  v.original->setOnClickListener([this] { impl_->showText(false); });
  v.translated->setOnClickListener([this] { impl_->showText(true); });
#if TARGET_OS_ANDROID
  v.attribution = new View();
  v.attribution->setSize(130, 24);
  v.attribution->setPadding(Edge::All, 4)->setFlexShrink(0);
  v.attribution->setBackgroundColor(Color(255, 255, 255, 255));
  auto *badge = new ImageView(0, 0, 122, 16, "assets/translation/google-translate.png");
  badge->setContentMode(ImageView::ContentMode::Contain);
  v.attribution->addView(badge);
#else
  auto *attribution = new TextView(app_theme::font, 14);
  attribution->setLocalizedText(i18n::message("readme.translation.machine"));
  attribution->setThemedColor(app_theme::textSecondary);
  attribution->setHeight(20)->setFlexShrink(0);
  v.attribution = attribution;
#endif
  v.panel->addView(v.attribution);
  v.status = new TextView(app_theme::font, 18);
  v.status->setThemedColor(app_theme::textSecondary);
  v.status->setWrap(true);
  v.status->setMinHeight(28)->setFlexShrink(0);
  v.panel->addView(v.status);
  v.lines = new DocumentTextReader();
  v.lines->setName("chartReadmeLines");
  v.lines->setWidthPercent(100)->setFlex(1)->setMinHeight(0);
  v.lines->onOpenUrl = [this](const std::string &url) {
    std::string error;
    if (!platform_open::openExternalUrl(url, error)) {
      auto &v = *impl_;
      v.status->setLocalizedText(i18n::message("reader.link.open_error"));
      v.setStatusVisible(true);
      v.root->applyYogaLayout();
    }
  };
  v.panel->addView(v.lines);
  v.close = modal_view::makeModalButton(i18n::message("readme.reader.close"), 20);
  modal_view::styleThemedActionButton(v.close, static_cast<TextView *>(v.close->getContentView()),
      true, app_theme::control, app_theme::controlHover, app_theme::controlPressed, app_theme::hairlineStrong);
  v.close->setHeight(40)->setFlexShrink(0)->setWidthPercent(100);
  v.close->setOnClickListener([this] { hide(); });
  v.panel->addView(v.close);
}
std::unique_ptr<ChartReadmeModal> ChartReadmeModal::Create(View *parent) {
  return parent ? std::unique_ptr<ChartReadmeModal>(new ChartReadmeModal(parent)) : nullptr;
}
ChartReadmeModal::~ChartReadmeModal() {
  hide();
  for (auto *button : {impl_->close, impl_->copy, impl_->translate, impl_->original, impl_->translated, impl_->standard, impl_->highFidelity})
    button->setOnClickListener(nullptr);
}
void ChartReadmeModal::show(const std::filesystem::path &chart, const std::string &title) {
  auto &v = *impl_;
  v.cancelTranslation();
  v.documents.clear();
  const char *target = "en";
  switch (i18n::language()) {
  case i18n::Language::Korean: target = "ko"; break;
  case i18n::Language::Japanese: target = "ja"; break;
  case i18n::Language::SimplifiedChinese: target = "zh-Hans"; break;
  case i18n::Language::TraditionalChinese: target = "zh-Hant"; break;
  default: break;
  }
  v.targetSelection = {.label = i18n::message("readme.translation.target"), .selectedId = target,
      .options = {{"en", "English"}, {"ko", "한국어"}, {"ja", "日本語"},
                  {"zh-Hans", "简体中文"}, {"zh-Hant", "繁體中文"}}};
#if TARGET_OS_ANDROID
  v.targetSelection.options = {{"en", "English"}, {"ko", "한국어"}, {"ja", "日本語"}, {"zh", "中文"}};
  if (std::string_view(target).starts_with("zh")) v.targetSelection.selectedId = "zh";
#endif
  v.targetSelector->refresh(v.targetSelection);
  v.sourceSelection = {.label = i18n::message("readme.translation.source"), .selectedId = "auto",
      .options = {{"auto", i18n::message("readme.translation.auto_detect")},
                  {"ja", "日本語"}, {"en", "English"}, {"ko", "한국어"}}};
  v.sourceSelector->refresh(v.sourceSelection);
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
  v.cancelTranslation();
  v.targetSelection.open = false;
  v.targetSelector->refresh(v.targetSelection);
  v.sourceSelection.open = false;
  v.sourceSelector->refresh(v.sourceSelection);
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
  v.selector->setTriggerWidth(std::max(1, (panelWidth - 32) / 2));
  v.sourceSelector->setTriggerWidth(std::max(1, (panelWidth - 32) / 2));
  v.encodingSelector->setTriggerWidth(std::max(1, (panelWidth - 32) / 2));
  v.targetSelector->setTriggerWidth(std::max(1, (panelWidth - 32) / 2));
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
      v.originalText = v.text;
      v.setStatusVisible(false);
    }
    if (error != chart_readme::Error::None || v.text.empty()) v.setStatusVisible(true);
    v.refreshActions();
    v.root->applyYogaLayout();
    v.reflow(true);
  }
  if (auto result = v.translation.take()) {
    using S = readme_translation::State;
    if (result->state == S::Complete) {
      v.translating = false;
      v.translatedText = std::move(result->text);
      v.showText(true);
    } else if (result->state == S::Cancelled) {
      v.translating = false; v.setStatusVisible(false);
    } else {
      v.translating = result->state != S::Failed;
      v.status->setLocalizedText(i18n::message(result->state == S::Failed ? "readme.translation.failed" :
          result->state == S::Working ? "readme.translation.working" : "readme.translation.preparing"));
      v.setStatusVisible(true);
    }
    v.refreshActions(); v.root->applyYogaLayout(); v.reflow(false);
  }
  if (v.loading || v.translating) ui::requestAnimationFrame();
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
    else if (v.sourceSelection.open) { v.sourceSelection.open = false; v.sourceSelector->refresh(v.sourceSelection); }
    else if (v.targetSelection.open) { v.targetSelection.open = false; v.targetSelector->refresh(v.targetSelection); }
    else hide();
    return false;
  }
  if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
    v.closingGamepad = true; hide(); return false;
  }
  if (event.type == SDL_EVENT_KEY_DOWN && !v.selection.open && !v.encodingSelection.open && !v.sourceSelection.open && !v.targetSelection.open) {
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
