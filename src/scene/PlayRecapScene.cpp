#include "PlayRecapScene.h"
#include "../recap/ReceiptView.h"
#include "../recap/ChartMetadata.h"
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
#include <map>
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

namespace {
std::string tr(const char *key) { return i18n::tr(key); }
constexpr std::size_t browsePageSize = 20;
constexpr const char *sectionKeys[] = {"recap.ui.lamps", "recap.ui.scores", "recap.ui.bp"};
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
void loadChartMetadata(recap::Snapshot &snapshot, ChartRepository &charts) {
  std::map<std::string,std::optional<recap::ChartMetadata>> metadata;
  for (const auto &section : snapshot.sections)
    for (const auto &upgrade : section) metadata.try_emplace(upgrade.hash);
  if (metadata.empty()) return;
  auto session = charts.OpenSession();
  if (!session) return;
  ChartMetaQuery query;
  query.rawSongData = true;
  query.allowedSha256.emplace();
  for (const auto &[hash,details] : metadata) query.allowedSha256->push_back(hash);
  std::vector<ChartMetaRecord> records;
  session->QueryChartMeta(query,records);
  for (const auto &record : records) {
    auto found = metadata.find(record.meta.SHA256);
    if (found == metadata.end() || found->second) continue;
    found->second = recap::chartMetadata(record.meta,
        session->DifficultyTableLabelsForChart(record.meta),
        !record.unavailable && !record.solidArchive);
  }
  // Deleted charts may still have a SHA-256 difficulty-table entry.
  for (auto &[hash,details] : metadata) if (!details) {
    bms_parser::ChartMeta meta;
    meta.SHA256 = hash; meta.PlayLevel = 0;
    details = recap::chartMetadata(meta,session->DifficultyTableLabelsForChart(meta),false);
  }
  for (auto &section : snapshot.sections)
    for (auto &upgrade : section) recap::applyChartMetadata(upgrade,*metadata.at(upgrade.hash));
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
  auto *charts = &context.chartRepository;
  const auto database = repository->GetResolvedDatabasePath();
  try {
    load_ = std::async(std::launch::async, [repository, charts, database, range, session = period_ == 0] {
      auto snapshot = repository->LoadPlayRecap(range, session, database);
      if (snapshot.error.empty()) loadChartMetadata(snapshot,*charts);
      return snapshot;
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
void PlayRecapScene::addUpgrade(View *parent, int kind, const recap::Upgrade &upgrade) {
  auto *card = new View();
  card->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(10)->setPadding(Edge::All, 6)
      ->setCornerRadius(4)->setFlexShrink(0)->setThemedBackgroundColor(app_theme::panel);
  parent->addView(card);
  const auto value = [kind](int number) { return kind == 0 ? kombo::lampLabel(number) : std::to_string(number); };
  std::string text = upgrade.before ? value(*upgrade.before) : tr("recap.ui.new");
  text += " → " + value(upgrade.after);
  if (kind && upgrade.before) {
    const int delta = upgrade.after - *upgrade.before;
    text += " (" + std::string(delta > 0 ? "+" : "") + std::to_string(delta) + ")";
  }
  const std::string mode = upgrade.lnMode == 1 ? "LN" : upgrade.lnMode == 2 ? "CN" :
      upgrade.lnMode == 3 ? "HCN" : upgrade.lnMode == -1 ? tr("recap.ui.legacy") : tr("recap.ui.normal");
  auto *difficulty = new View();
  difficulty->setFlexDirection(FlexDirection::Column)->setWidth(108)->setFlexShrink(0);
  card->addView(difficulty);
  label(difficulty,upgrade.difficulty,18);
  label(difficulty,mode,14)->setThemedColor(app_theme::textSecondary);
  const bool narrow = rendering::window_width < 720;
  auto *details = new View();
  details->setFlexDirection(narrow ? FlexDirection::Column : FlexDirection::Row)
      ->setAlignItems(narrow ? YGAlignStretch : YGAlignCenter)->setGap(6)->setFlex(1)->setMinWidth(0);
  card->addView(new recap::JacketView(upgrade.jacket));
  card->addView(details);
  auto *title = label(details,upgrade.title,20);
  if (!narrow) title->setFlex(1)->setMinWidth(0);
  if (kind == 0) {
    auto *lamps = row(details);
    lamps->setFlexWrap(YGWrapWrap)->setAlignItems(YGAlignCenter);
    if (!narrow) lamps->setWidth(280);
    auto *before = label(lamps,upgrade.before ? value(*upgrade.before) : tr("recap.ui.new"),16);
    if (upgrade.before) recap::styleLampBadge(before,*upgrade.before,true);
    label(lamps,"→",16);
    recap::styleLampBadge(label(lamps,value(upgrade.after),16),upgrade.after,true);
  } else {
    auto *change = label(details,text,18);
    change->setThemedColor(app_theme::cyan);
    if (!narrow) change->setWidth(280);
  }
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
  button(header, tr(exporting_ ? "recap.ui.exporting" : "recap.ui.export"), [this] {
    later([this] { if (!exporting_) beginExport(); });
  });
  auto *scroll = new ScrollView();
  scroll->setFlex(1)->setMinHeight(0);
  root_->addView(scroll);
  auto *content = new View();
  content->setFlexDirection(FlexDirection::Column)->setGap(6)->setPadding(Edge::Bottom, 16);
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
    label(content, tr(sectionKeys[kind]) + " · " + std::to_string(upgrades.size()), 22)->setMargin(Edge::Top,8);
    if (upgrades.empty()) label(content, tr("recap.ui.empty"), 18);
    const auto end = std::min(upgrades.size(), offsets_[kind] + browsePageSize);
    for (auto i = offsets_[kind]; i < end; ++i) {
      if (kind == 0 && (i == offsets_[kind] || upgrades[i].after != upgrades[i-1].after)) {
        auto *heading = label(content,kombo::lampLabel(upgrades[i].after),18);
        recap::styleLampBadge(heading,upgrades[i].after,true);
        heading->setAlignSelf(YGAlignStretch);
      }
      addUpgrade(content, kind, upgrades[i]);
    }
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
  exporting_ = true;
  message_ = tr("recap.ui.exporting");
}
void PlayRecapScene::exportReceipt() {
  exporting_ = false;
  recap::ReceiptContent receipt{.title = tr("recap.ui.title"), .profile = profileName_,
      .period = periodText(), .footer = tr("recap.ui.image_footer")};
  for (int kind = 0; kind < 3; ++kind) {
    auto &section = receipt.sections[kind];
    section.title = tr(sectionKeys[kind]); section.empty = tr("recap.ui.empty");
    for (const auto &upgrade : snapshot_.sections[kind]) {
      const auto value = [kind](int number) { return kind == 0 ? kombo::lampLabel(number) : std::to_string(number); };
      const auto mode = upgrade.lnMode == 1 ? "LN" : upgrade.lnMode == 2 ? "CN" :
          upgrade.lnMode == 3 ? "HCN" : upgrade.lnMode == -1 ? tr("recap.ui.legacy") : tr("recap.ui.normal");
      std::string delta;
      if (kind && upgrade.before) {
        const int amount = upgrade.after - *upgrade.before;
        delta = "(" + std::string(amount > 0 ? "+" : "") + std::to_string(amount) + ")";
      }
      section.entries.push_back({upgrade.title, mode,
          upgrade.before ? value(*upgrade.before) : tr("recap.ui.new"), value(upgrade.after), delta,
          kind == 0 ? value(upgrade.after) : "", upgrade.difficulty,
          kind == 0 ? upgrade.before : std::nullopt, kind == 0 ? std::optional{upgrade.after} : std::nullopt, upgrade.jacket});
    }
  }
  const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  exportDirectory_ = Utils::GetDocumentsPath("recap_exports") / std::to_string(stamp);
  try {
    auto canvas = recap::makeReceipt(receipt);
    canvas->applyYogaLayout();
    const auto result = ResultImageExporter::ExportView(context, "recap", *canvas,
        {.outputDirectory = exportDirectory_});
    message_ = result.success ? tr("recap.ui.saved") : tr("recap.ui.failed") + " " + result.message;
    if (result.success) message_ += " · " + (result.artifactRetained ? result.outputPath.string() : tr("recap.ui.photos"));
    else if (result.artifactRetained && std::filesystem::exists(result.outputPath))
      message_ += " · " + result.outputPath.string();
  } catch (const std::exception &error) { message_ = tr("recap.ui.failed") + " " + error.what(); }
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
  if (exporting_ && !context.appInBackground.load()) exportReceipt();
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
