#include "PlayRecapScene.h"
#include "SceneManager.h"
#include "../ResultImageExporter.h"
#include "../Utils.h"
#include "../kombo/KomboModel.h"
#include "../rendering/common.h"
#include "../view/ActionButtonStyle.h"
#include "../view/AppTheme.h"
#include "../view/Button.h"
#include "../view/ScrollView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

namespace {
std::string tr(const char *key) { return i18n::tr(key); }
constexpr std::size_t browsePageSize = 20;
constexpr const char *sectionKeys[] = {"recap.ui.lamps", "recap.ui.scores", "recap.ui.combo"};
constexpr const char *periodKeys[] = {"recap.ui.session", "recap.ui.daily", "recap.ui.weekly", "recap.ui.custom"};
TextView *label(View *parent, const std::string &value, int size = 22) {
  auto *text = new TextView(app_theme::font, size);
  text->setText(value); text->setWrap(true);
  text->setMinHeight(size + 10)->setFlexShrink(0);
  text->setThemedColor(app_theme::textPrimary);
  parent->addView(text);
  return text;
}
View *row(View *parent) {
  auto *view = new View();
  view->setFlexDirection(FlexDirection::Row)->setGap(8)->setFlexShrink(0);
  parent->addView(view);
  return view;
}
Button *button(View *parent, const std::string &value, std::function<void()> click, bool selected = false) {
  auto *view = new Button();
  view->setHeight(54)->setFlex(1)->setMinWidth(0)->setCornerRadius(12);
  auto *text = new TextView(app_theme::font, 20);
  text->setText(value); text->setWrap(true);
  text->setAlign(TextView::CENTER); text->setVAlign(TextView::MIDDLE);
  view->setContentView(text);
  action_button::applyStyle(view, text, selected ? app_theme::ActionCategory::Primary : app_theme::ActionCategory::Neutral);
  view->setOnClickListener(std::move(click)); parent->addView(view);
  return view;
}
std::string localStamp(std::int64_t seconds) {
  auto time = recap::localTime(seconds);
  char buffer[40]{};
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &time);
  return buffer;
}
}

void PlayRecapScene::init() {
  profile_ = context.profileManager.activeProfile().id;
  profileName_ = context.profileManager.activeProfile().displayName;
  from_ = through_ = recap::localDate(std::time(nullptr));
  reload(); build();
}
void PlayRecapScene::later(std::function<void()> action) {
  defer([this, profile = profile_, action = std::move(action)] {
    if (profile == context.profileManager.activeProfile().id) action();
    build(); return true;
  }, 0, true);
}
void PlayRecapScene::reload() {
  if (exporting_) return;
  if (load_.valid()) { reloadPending_ = true; return; }
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  recap::Range range;
  if (period_ == 0) range = {0, sessionBefore_ ? sessionBefore_ : now + 1};
  else if (period_ < 3) range = recap::calendarRange(now, period_ == 2, dayOffset_);
  else {
    const auto start = recap::parseLocalDate(from_), end = recap::parseLocalDate(through_);
    if (!start || !end || *start > *end) {
      snapshot_ = {}; snapshot_.error = tr("recap.ui.invalid_dates"); return;
    }
    range = {*start, recap::shiftDays(*end, 1)};
  }
  snapshot_ = {}; offsets_ = {}; message_.clear(); loading_ = true;
  loadedProfile_ = profile_;
  auto *repository = &context.scoreRepository;
  const auto database = repository->GetResolvedDatabasePath();
  try {
    load_ = std::async(std::launch::async, [repository, database, range, session = period_ == 0] {
      return repository->LoadPlayRecap(range, session, database);
    });
  } catch (const std::exception &error) {
    loading_ = false; snapshot_.error = error.what();
  }
}
std::string PlayRecapScene::periodText() const {
  if (!snapshot_.range.end || !snapshot_.range.start) return tr(periodKeys[period_]);
  return tr(periodKeys[period_]) + " · " + localStamp(snapshot_.range.start) + " — " +
      localStamp(snapshot_.range.end - 1);
}
void PlayRecapScene::movePeriod(int direction) {
  if (period_ == 0) {
    if (direction < 0 && snapshot_.range.start > 0) {
      sessionForward_.push_back(sessionBefore_);
      sessionBefore_ = snapshot_.range.start;
    } else if (direction > 0 && !sessionForward_.empty()) {
      sessionBefore_ = sessionForward_.back(); sessionForward_.pop_back();
    } else return;
  } else dayOffset_ = std::min(0, dayOffset_ + direction);
  reload();
}
void PlayRecapScene::addUpgrade(View *parent, int kind, const recap::Upgrade &upgrade, bool image) {
  auto *card = new View();
  card->setFlexDirection(FlexDirection::Column)->setGap(4)->setPadding(Edge::All, 12)
      ->setCornerRadius(12)->setFlexShrink(0)->setThemedBackgroundColor(app_theme::panel);
  if (image) card->setHeight(120);
  parent->addView(card);
  auto *title = label(card, upgrade.title, 22);
  if (image) { title->setWrap(false); title->setMinimumFontSize(16); title->setOverflow(TextView::TextOverflow::Hidden); title->setHeight(32); }
  const auto value = [kind](int number) { return kind == 0 ? kombo::lampLabel(number) : std::to_string(number); };
  std::string text = upgrade.before ? value(*upgrade.before) : tr("recap.ui.new");
  text += " → " + value(upgrade.after);
  if (kind && upgrade.before) {
    const int delta = upgrade.after - *upgrade.before;
    text += " (" + std::string(delta > 0 ? "+" : "") + std::to_string(delta) + ")";
  }
  const std::string mode = upgrade.lnMode == 1 ? "LN" : upgrade.lnMode == 2 ? "CN" :
      upgrade.lnMode == 3 ? "HCN" : upgrade.lnMode == -1 ? tr("recap.ui.legacy") : tr("recap.ui.normal");
  auto *detail = label(card, text + " · " + mode, 18);
  detail->setThemedColor(app_theme::cyan);
  if (image) { detail->setHeight(56); detail->setOverflow(TextView::TextOverflow::Hidden); }
}
void PlayRecapScene::build() {
  onPause();
  for (auto *view : views) delete view;
  views.clear();
  root_ = new View(0, 0, rendering::window_width, rendering::window_height);
  root_->setFlexDirection(FlexDirection::Column)->setGap(10)->setPadding(Edge::All, 18)
      ->setThemedBackgroundColor(app_theme::mainMenuBackdrop);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const auto safe = GetIOSSafeAreaInsetsNormalized();
  root_->setPadding(Edge::Top, 18 + safe.top * rendering::window_height)
      ->setPadding(Edge::Bottom, 18 + safe.bottom * rendering::window_height)
      ->setPadding(Edge::Left, 18 + safe.left * rendering::window_width)
      ->setPadding(Edge::Right, 18 + safe.right * rendering::window_width);
#endif
  addView(root_);
  auto *header = row(root_);
  button(header, tr("kombo.ui.back"), [this] { back(); });
  button(header, tr(exporting_ ? "recap.ui.cancel" : "recap.ui.export"), [this] { later([this] {
    if (exporting_) { exporting_ = false; message_ = tr("recap.ui.cancelled") + exportSummary(); }
    else beginExport();
  }); });
  auto *scroll = new ScrollView();
  scroll->setFlex(1)->setMinHeight(0);
  root_->addView(scroll);
  auto *content = new View();
  content->setFlexDirection(FlexDirection::Column)->setGap(12)->setPadding(Edge::Bottom, 20);
  scroll->setContentView(content);
  label(content, tr("recap.ui.title") + " · " + profileName_, 30);
  if (!exporting_) {
    auto *periods = row(content);
    for (int i = 0; i < 4; ++i) button(periods, tr(periodKeys[i]), [this, i] { later([this, i] {
      period_ = i; dayOffset_ = 0; sessionBefore_ = 0; sessionForward_.clear(); reload();
    }); }, i == period_);
    if (period_ == 3) {
      label(content, tr("recap.ui.date_hint"), 18);
      auto *inputs = row(content);
      for (int i = 0; i < 2; ++i) {
        auto *input = new TextInputBox(app_theme::font, 22);
        input->setEditingText(i == 0 ? from_ : through_);
        input->setHeight(54)->setFlex(1)->setMinWidth(0)->setPadding(Edge::All, 10)
            ->setThemedBackgroundColor(app_theme::panel);
        input->onTextChanged([this, i](const std::string &text) { (i == 0 ? from_ : through_) = text; });
        inputs->addView(input);
      }
      button(inputs, tr("recap.ui.apply"), [this] { later([this] { reload(); }); });
    } else {
      auto *navigation = row(content);
      button(navigation, tr("recap.ui.previous"), [this] { later([this] { if (!loading_) movePeriod(-1); }); });
      button(navigation, tr("recap.ui.next"), [this] { later([this] { if (!loading_) movePeriod(1); }); });
      button(navigation, tr("recap.ui.latest"), [this] { later([this] {
        dayOffset_ = 0; sessionBefore_ = 0; sessionForward_.clear(); reload();
      }); });
    }
  }
  label(content, periodText(), 20);
  label(content, tr("recap.ui.scope"), 16)->setThemedColor(app_theme::textSecondary);
  if (!message_.empty()) label(content, message_, 18);
  if (loading_) label(content, tr("recap.ui.loading"));
  else if (!snapshot_.error.empty()) label(content, tr("recap.ui.failed") + " " + snapshot_.error);
  else for (int kind = 0; kind < 3; ++kind) {
    const auto &upgrades = snapshot_.sections[kind];
    label(content, tr(sectionKeys[kind]) + " · " + std::to_string(upgrades.size()), 26);
    if (upgrades.empty()) label(content, tr("recap.ui.empty"), 18);
    const auto end = std::min(upgrades.size(), offsets_[kind] + browsePageSize);
    for (auto i = offsets_[kind]; i < end; ++i) addUpgrade(content, kind, upgrades[i]);
    if (upgrades.size() > browsePageSize) {
      auto *navigation = row(content);
      button(navigation, tr("recap.ui.previous"), [this, kind] { later([this, kind] {
        offsets_[kind] = offsets_[kind] >= browsePageSize ? offsets_[kind] - browsePageSize : 0;
      }); });
      label(navigation, std::to_string(offsets_[kind] + 1) + "–" + std::to_string(end), 18);
      button(navigation, tr("recap.ui.next"), [this, kind] { later([this, kind] {
        if (offsets_[kind] + browsePageSize < snapshot_.sections[kind].size()) offsets_[kind] += browsePageSize;
      }); });
    }
  }
  root_->applyYogaLayout();
}
void PlayRecapScene::beginExport() {
  if (loading_ || !snapshot_.error.empty()) return;
  if (rendering::window_height < 382) { message_ = tr("recap.ui.resized"); return; }
  const auto rows = static_cast<std::size_t>((rendering::window_height - 250) / 132);
  exportPages_.clear();
  for (int kind = 0; kind < 3; ++kind) {
    const auto count = snapshot_.sections[kind].size();
    for (std::size_t start = 0; start < count; start += rows)
      exportPages_.push_back({kind, start, std::min(rows, count - start)});
  }
  if (exportPages_.empty()) { message_ = tr("recap.ui.empty"); return; }
  const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  exportDirectory_ = Utils::GetDocumentsPath("recap_exports") / std::to_string(stamp);
  exportWidth_ = rendering::window_width; exportHeight_ = rendering::window_height;
  exportedToPhotos_ = false; exportPage_ = 0; exporting_ = true;
  message_ = tr("recap.ui.exporting");
}
std::string PlayRecapScene::exportSummary() const {
  if (!exportPage_) return {};
  return " · " + tr("recap.ui.saved") + " " + std::to_string(exportPage_) + "/" +
      std::to_string(exportPages_.size()) + " · " +
      (exportedToPhotos_ ? tr("recap.ui.photos") : exportDirectory_.string());
}
void PlayRecapScene::exportNext() {
  if (rendering::window_width != exportWidth_ || rendering::window_height != exportHeight_) {
    exporting_ = false; message_ = tr("recap.ui.resized") + exportSummary(); build(); return;
  }
  const auto page = exportPages_[exportPage_];
  auto result = ResultImageExporter::ExportSkin(context, "recap_" + std::to_string(exportPage_ + 1),
      [this, page](RenderContext &renderContext) {
        auto canvas = std::make_unique<View>(0, 0, rendering::window_width, rendering::window_height);
        canvas->setFlexDirection(FlexDirection::Column)->setGap(12)->setPadding(Edge::All, 24)
            ->setThemedBackgroundColor(app_theme::mainMenuBackdrop);
        label(canvas.get(), "AsoBMaShow · " + tr("recap.ui.title"), 28);
        auto *name = label(canvas.get(), profileName_ + " · " + periodText(), 18);
        name->setWrap(false); name->setMinimumFontSize(14); name->setOverflow(TextView::TextOverflow::Hidden);
        label(canvas.get(), tr(sectionKeys[page.kind]) + " · " +
            std::to_string(exportPage_ + 1) + "/" + std::to_string(exportPages_.size()), 24);
        for (auto i = page.start; i < page.start + page.count; ++i)
          addUpgrade(canvas.get(), page.kind, snapshot_.sections[page.kind][i], true);
        label(canvas.get(), tr("recap.ui.image_footer"), 16);
        canvas->applyYogaLayout(); canvas->render(renderContext); return true;
      }, {.outputDirectory = exportDirectory_});
  if (!result.success) { exporting_ = false; message_ = tr("recap.ui.failed") + " " + result.message + exportSummary(); }
  else {
    exportedToPhotos_ = !result.artifactRetained;
    ++exportPage_;
    exporting_ = exportPage_ < exportPages_.size();
    message_ = tr(exporting_ ? "recap.ui.exporting" : "recap.ui.saved") + " " +
        std::to_string(exportPage_) + "/" + std::to_string(exportPages_.size());
    if (!exporting_) message_ += " · " + (result.artifactRetained ? exportDirectory_.string() : result.message);
  }
  build();
}
void PlayRecapScene::update(float) {
  if (profile_ != context.profileManager.activeProfile().id) {
    exporting_ = false; profile_ = context.profileManager.activeProfile().id;
    profileName_ = context.profileManager.activeProfile().displayName;
    snapshot_ = {}; sessionBefore_ = 0; sessionForward_.clear(); reload(); build();
  }
  if (load_.valid() && load_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    try { auto result = load_.get(); if (loadedProfile_ == profile_) snapshot_ = std::move(result); }
    catch (const std::exception &error) { snapshot_.error = error.what(); }
    loading_ = false;
    if (std::exchange(reloadPending_, false)) reload();
    build();
  }
  if (exporting_ && !context.appInBackground.load()) exportNext();
  if (root_ && (root_->getWidth() != rendering::window_width || root_->getHeight() != rendering::window_height)) build();
}
void PlayRecapScene::onPause() {
  const auto finish = [](View *view, const auto &self) -> void {
    if (auto *input = dynamic_cast<TextInputBox *>(view)) input->endEditing();
    for (auto *child : view->getChildren()) self(child, self);
    if (auto *scroll = dynamic_cast<ScrollView *>(view); scroll && scroll->getContentView()) self(scroll->getContentView(), self);
  };
  for (auto *view : views) { view->cancelPointerInput(); finish(view, finish); }
}
void PlayRecapScene::onResume() { reload(); build(); }
void PlayRecapScene::back() {
  exporting_ = false;
  defer([this] { (void)returnToScene(*context.sceneManager, returnTarget_); return false; }, 0, true);
}
EventHandleResult PlayRecapScene::handleEvents(SDL_Event &event) {
  for (auto it = views.rbegin(); it != views.rend(); ++it) if (!(*it)->handleEvents(event)) return {};
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) back();
  return {};
}
