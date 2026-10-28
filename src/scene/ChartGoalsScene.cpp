#include "ChartGoalsScene.h"
#include "ChartViewerScene.h"
#include "SceneManager.h"
#include "../LongNoteModeUtils.h"
#include "../ScoreRankUtils.h"
#include "../rendering/common.h"
#include "../rendering/ResponsiveUi.h"
#include "../view/ActionButtonStyle.h"
#include "../view/ActionButtonContent.h"
#include "../view/AppTheme.h"
#include "../view/Button.h"
#include "../view/ClearLampColors.h"
#include "../ClearLampOrder.h"
#include "../view/DropdownView.h"
#include "../view/IconText.h"
#include "../view/ImageView.h"
#include "../view/OverlayPortal.h"
#include "../view/ScrollView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include <iomanip>
#include <sstream>
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

namespace {
TextView *label(View *parent, const std::string &value, int size = 21) {
  auto *text = new TextView(app_theme::font, size);
  text->setText(value); text->setWrap(true);
  text->setMinHeight(size + 12)->setFlexShrink(0);
  text->setThemedColor(app_theme::textPrimary);
  parent->addView(text);
  return text;
}
View *card(View *parent) {
  auto *view = new View();
  view->setFlexDirection(FlexDirection::Column)->setGap(12)->setPadding(Edge::All, 20)
      ->setFlexShrink(0)->setCornerRadius(24)->setThemedBackgroundColor(app_theme::panel);
  parent->addView(view);
  return view;
}
Button *action(View *parent, const std::string &value, std::function<void()> click,
               app_theme::ActionCategory category = app_theme::ActionCategory::Neutral,
               uint32_t glyph = 0) {
  auto *button = new Button();
  button->setHeight(rendering::compactMenuCanvasActive ? 66 : 52)->setMinWidth(0)->setFlexShrink(0);
  auto *text = new TextView(app_theme::font, 21);
  text->setText(value); text->setAlign(TextView::CENTER); text->setVAlign(TextView::MIDDLE);
  if (glyph) {
    auto *content = action_button::makeInlineContent(text, glyph);
    content->setPadding(Edge::Left, 14)->setPadding(Edge::Right, 14);
    button->setContentView(content);
  } else button->setContentView(text);
  button->setCornerRadius(16);
  action_button::applyStyle(button, text, category);
  if (glyph) action_button::setIconColor(button, [category] { return app_theme::actionPalette(category).label; });
  button->setOnClickListener(std::move(click)); parent->addView(button);
  return button;
}
// Draw the same rounded shape as the tab, clipped to a narrow left strip.
class GoalLampTabAccent final : public View {
public:
  explicit GoalLampTabAccent(int rank) {
    rounded_.setBackgroundColor(clearLampColorForRank(rank));
    rounded_.setCornerRadius(16);
    setPositionType(YGPositionTypeAbsolute);
    setPosition(Edge::Left, 0)->setPosition(Edge::Right, 0)
        ->setPosition(Edge::Top, 0)->setPosition(Edge::Bottom, 0);
  }
  void onResize(int width, int height) override {
    rounded_.setSize(width, height);
    rounded_.applyYogaLayout();
    rounded_.setPositionNoLayout(getX(), getY());
  }
  void onMove(int, int) override { rounded_.setPositionNoLayout(getX(), getY()); }
protected:
  void renderImpl(RenderContext &context) override {
    ScissorScope clip(context, getX(), getY(), 7, getHeight());
    rounded_.render(context);
  }
private:
  View rounded_;
};
class GoalArtworkPlaceholder final : public TextView {
public:
  explicit GoalArtworkPlaceholder(ImageView &image)
      : TextView(ui_icons::kFontAwesomeSolidPath, 24), image_(image) {}
protected:
  void renderImpl(RenderContext &context) override {
    if (image_.imageWidth() <= 0 || image_.imageHeight() <= 0)
      TextView::renderImpl(context);
  }
private:
  ImageView &image_;
};
std::optional<std::filesystem::path> goalArtworkPath(const ChartMetaRecord *record) {
  if (!record || record->unavailable || record->solidArchive) return {};
  const auto &meta = record->meta;
  const auto path = !meta.StageFile.empty() ? meta.StageFile : meta.Banner;
  if (path.empty()) return {};
  return meta.Folder / path;
}
void goalArtwork(View *parent, const ChartMetaRecord *record) {
  constexpr int size = 72;
  auto *frame = new View();
  frame->setName("goalArtwork");
  frame->setWidth(size)->setHeight(size)->setFlexShrink(0)
      ->setThemedBackgroundColor(app_theme::insetSurface);
  auto *image = new ImageView(0, 0, size, size);
  image->setName("goalArtworkImage");
  image->setPositionType(YGPositionTypeAbsolute);
  image->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0);
  image->setContentMode(ImageView::ContentMode::Contain);
  frame->addView(image);
  auto *placeholder = new GoalArtworkPlaceholder(*image);
  placeholder->setName("goalArtworkPlaceholder");
  placeholder->setText(ui_icons::textForCodepoint(ui_icons::kMusic));
  placeholder->setWidth(size)->setHeight(size);
  placeholder->setAlign(TextView::CENTER); placeholder->setVAlign(TextView::MIDDLE);
  placeholder->setThemedColor(app_theme::textSecondary);
  frame->addView(placeholder);
  if (const auto path = goalArtworkPath(record)) image->setImageAsync(fspath_to_path_t(*path));
  parent->addView(frame);
}
std::string goalDifficultyText(const bms_parser::ChartMeta &meta, const ChartMetaRecord *record) {
  if (record && !record->difficultyTableLabels.empty()) return record->difficultyTableLabels;
  const auto level = record ? record->meta.PlayLevel : meta.PlayLevel;
  if (level <= 0) return "Lv. —";
  std::ostringstream text;
  text << "Lv. " << level;
  return text.str();
}
void goalIdentity(View *parent, const bms_parser::ChartMeta &meta,
                  const ChartMetaRecord *record, int titleSize) {
  auto *identity = new View();
  identity->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(12)->setFlexShrink(0);
  goalArtwork(identity, record);
  auto *text = new View();
  text->setFlexDirection(FlexDirection::Column)->setGap(6)->setFlex(1)->setMinWidth(0);
  label(text, meta.Title + (meta.SubTitle.empty() ? "" : " " + meta.SubTitle), titleSize);
  auto *difficulty = label(text, goalDifficultyText(meta, record), 18);
  difficulty->setName("goalDifficulty");
  YGNodeStyleSetMaxWidthPercent(difficulty->getNode(), 100);
  difficulty->setAlignSelf(YGAlignFlexStart)
      ->setPadding(Edge::Left, 10)->setPadding(Edge::Right, 10)
      ->setPadding(Edge::Top, 4)->setPadding(Edge::Bottom, 4)->setCornerRadius(8);
  difficulty->setThemedColor(app_theme::cyan);
  difficulty->setThemedBackgroundColor([] { return app_theme::withAlpha(app_theme::cyan(), 24); });
  identity->addView(text);
  parent->addView(identity);
}
std::map<std::string, ChartMetaRecord> loadGoalArtwork(
    ApplicationContext &context, const std::vector<const chart_goals::GoalProgress *> &entries, int offset) {
  std::map<std::string, ChartMetaRecord> result;
  ChartMetaQuery query;
  query.allowedSha256.emplace();
  for (int i = offset; i < std::min(static_cast<int>(entries.size()), offset + chart_goals::pageSize); ++i)
    query.allowedSha256->push_back(chart_goals::normalizedHash(entries[i]->goal.chart.SHA256));
  if (query.allowedSha256->empty()) return result;
  auto session = context.chartRepository.OpenSession();
  if (!session) return result;
  std::vector<ChartMetaRecord> records;
  session->QueryChartMeta(query, records);
  for (auto &record : records) {
    const auto hash = chart_goals::normalizedHash(record.meta.SHA256);
    if (!record.unavailable && !record.solidArchive) result.try_emplace(hash, std::move(record));
  }
  return result;
}
void lamp(View *parent, const std::string &text, int rank) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row)->setGap(10)->setAlignItems(YGAlignCenter)->setFlexShrink(0);
  if (hasClearLampColor(rank)) {
    auto *bar = new View();
    bar->setWidth(5)->setHeight(26)->setCornerRadius(2)->setFlexShrink(0);
    bar->setBackgroundColor(clearLampColorForRank(rank)); row->addView(bar);
  }
  label(row, text)->setFlex(1)->setMinWidth(0);
  parent->addView(row);
}
std::string rateText(const chart_goals::Target &target) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(2) << (100.0 * target.scoreNumerator / target.scoreDenominator);
  return text.str();
}
std::string bestScoreText(const std::optional<ScoreBestSnapshot> &score) {
  if (!score) return i18n::tr("goals.ui.no_record");
  auto result = std::to_string(score->score) + " EX";
  if (score->maxScore > 0) {
    result += " / " + std::to_string(score->maxScore);
    result += " · " + score_rank::displayLabelForScore(score->score, score->maxScore);
  }
  return result;
}
std::string scoreTarget(const chart_goals::Target &target) {
  if (target.scoreDenominator == 27) return kombo::scoreRankLabel(target.scoreNumerator);
  return rateText(target) + "% EX";
}
std::string scope(const chart_goals::Goal &goal) {
  return goal.longNoteMode == 0 ? i18n::tr("goals.ui.shared_scope") : long_note_mode::idFromValue(goal.longNoteMode);
}
void targets(View *parent, const chart_goals::Target &target) {
  if (target.lamp >= 0) {
    const auto value = kombo::lampLabel(target.lamp);
    lamp(parent, target.lamp >= kClearTypeFullComboRank ? value
        : i18n::format("kombo.ui.at_least", {{"value", value}}), target.lamp);
  }
  if (target.scoreNumerator > 0) {
    const auto value = scoreTarget(target);
    label(parent, target.scoreNumerator >= target.scoreDenominator ? value
        : i18n::format("kombo.ui.at_least", {{"value", value}}));
  }
}
void select(View *parent, OverlayPortal *portal, const std::string &caption,
            std::vector<DropdownView::Option> options, std::string selected,
            std::function<void(const std::string &)> changed) {
  label(parent, caption, 18);
  auto state = std::make_shared<DropdownView::State>();
  state->selectedId = std::move(selected); state->options = std::move(options); state->maxVisibleItems = 7;
  auto ptr = std::make_shared<DropdownView *>(nullptr);
  auto *view = new DropdownView({.onOptionSelected = [state, ptr, changed](const std::string &id) {
    state->selectedId = id; (*ptr)->refresh(*state); changed(id);
  }}, portal);
  *ptr = view;
  view->setHeight(rendering::compactMenuCanvasActive ? 66 : 52)->setFlexShrink(0);
  view->setTriggerWidth(300); view->refresh(*state); parent->addView(view);
}
}
ChartGoalsScene::ChartGoalsScene(ApplicationContext &context, SceneReturnTarget target,
                               std::optional<ChartMetaRecord> initial, SelectChart onSelect,
                               std::optional<int> initialMode, bool initialRecommendations)
    : Scene(context), returnTarget_(std::move(target)), onSelectChart_(std::move(onSelect)),
      initial_(std::move(initial)), initialMode_(initialMode), initialRecommendations_(initialRecommendations), profileId_(context.profileManager.activeProfile().id) {}
bool ChartGoalsScene::checkProfile() {
  if (profileId_ == context.profileManager.activeProfile().id) return true;
  profileId_ = context.profileManager.activeProfile().id;
  initial_.reset(); initialMode_.reset(); resolved_.reset(); selected_.reset();
  draft_ = {}; page_ = {}; offset_ = achievedOffset_ = 0;
  editing_ = customScore_ = confirmRemove_ = loadFailed_ = false;
  message_.clear(); customRate_.clear();
  scrollOffsets_.clear(); renderedScrollKey_.clear(); navigationOffset_ = 0; navigationScroll_ = nullptr;
  return false;
}
void ChartGoalsScene::later(std::function<void()> action) {
  defer([this, profile = profileId_, action = std::move(action)] {
    if (profile == context.profileManager.activeProfile().id) action();
    else { checkProfile(); reload(); }
    build(); return true;
  }, 0, true);
}
void ChartGoalsScene::init() {
  checkProfile();
  if (initial_) {
    auto record = std::exchange(initial_, {});
    showGoal({.chart=record->meta, .longNoteMode=initialMode_.value_or(scoreLongNoteModeForClearLamp(
        record->meta, long_note_mode::valueFromId(context.settings.selectedLnMode)))});
    initialMode_.reset();
  } else {
    reload();
    if (initialRecommendations_) {
      overviewTab_ = OverviewTab::Recommendations;
      if (!recommendations_.empty()) {
        recommendationRank_ = recommendations_.begin()->first;
        loadRecommendations(*recommendationRank_, 0);
      }
    }
  }
  initialRecommendations_ = false;
  build();
}
void ChartGoalsScene::reload() {
  std::map<int, int> previousOffsets;
  for (const auto &[rank, group] : recommendations_) previousOffsets[rank] = group.offset;
  recommendations_.clear();
  try {
    auto session = context.chartRepository.OpenSession(&context.scoreRepository);
    if (!session) throw std::runtime_error("Chart storage is unavailable.");
    recommendationTables_ = session->SelectDifficultyTables();
    if (recommendationTable_ && std::ranges::none_of(recommendationTables_, [this](const auto &table) {
          return table.id == recommendationTable_;
        })) recommendationTable_ = 0;
    for (const auto &[rank, count] : session->GoalRecommendationCounts(recommendationTable_)) {
      auto &group = recommendations_[rank];
      group.count = count;
      group.offset = std::clamp(previousOffsets[rank], 0,
          (count - 1) / chart_goals::pageSize * chart_goals::pageSize);
    }
  } catch (const std::exception &failure) {
    SDL_Log("Cannot load goal recommendations: %s", failure.what());
    message_ = i18n::tr("goals.ui.storage_error");
  }
  if (recommendationRank_ && !recommendations_.contains(*recommendationRank_))
    recommendationRank_ = recommendations_.empty() ? std::optional<int>{} : recommendations_.begin()->first;
  if (recommendationRank_) {
    const auto &group = recommendations_.at(*recommendationRank_);
    const int lastPage = (group.count - 1) / chart_goals::pageSize * chart_goals::pageSize;
    loadRecommendations(*recommendationRank_, std::clamp(previousOffsets[*recommendationRank_], 0, lastPage));
  }
  std::string error;
  page_ = {};
  loadFailed_ = false;
  // Goal metadata is small; collect pages before partitioning so completed
  // entries never displace unfinished goals onto later pages.
  for (int offset = 0;; offset += chart_goals::pageSize) {
    auto loaded = context.scoreRepository.LoadChartGoals(offset, error);
    if (!loaded) {
      loadFailed_ = true;
      SDL_Log("Cannot load personal goals: %s", error.c_str());
      message_ = i18n::tr("goals.ui.storage_error"); return;
    }
    for (auto &entry : loaded->goals) page_.goals.push_back(std::move(entry));
    if (!loaded->hasMore) break;
  }
  if (selected_ && !editing_) {
    auto current = context.scoreRepository.LoadChartGoal(draft_.chart, draft_.longNoteMode, error);
    if (!error.empty()) { loadFailed_ = true; message_ = i18n::tr("goals.ui.storage_error"); return; }
    selected_ = std::move(current);
    if (selected_) draft_ = selected_->goal;
  }
  resolveChart();
}
void ChartGoalsScene::loadRecommendations(int rank, int offset) {
  try {
    auto session = context.chartRepository.OpenSession(&context.scoreRepository);
    if (!session) throw std::runtime_error("Chart storage is unavailable.");
    const auto candidates = session->GoalRecommendations(rank, offset, recommendationTable_, recommendationSortTable_);
    ChartMetaQuery query;
    query.allowedSha256 = std::vector<std::string>{};
    for (const auto &entry : candidates) query.allowedSha256->push_back(entry.sha256);
    std::vector<ChartMetaRecord> records;
    if (!candidates.empty()) session->QueryChartMeta(query, records);
    auto &group = recommendations_[rank];
    group.rows.clear(); group.offset = offset; group.expanded = true;
    for (const auto &candidate : candidates) {
      const auto found = std::ranges::find_if(records, [&](const auto &record) {
        return record.meta.SHA256 == candidate.sha256;
      });
      if (found != records.end()) group.rows.emplace_back(candidate, *found);
    }
  } catch (const std::exception &failure) {
    SDL_Log("Cannot load goal recommendation page: %s", failure.what());
    message_ = i18n::tr("goals.ui.storage_error");
  }
}
void ChartGoalsScene::recommendGoal(const ChartMetaRecord &record, int mode, int rank) {
  showGoal({.chart=record.meta, .longNoteMode=mode});
  if (loadFailed_) return;
  editing_ = true;
  if (rank >= 50) draft_.target.lamp = rank;
  else {
    draft_.target.scoreNumerator = rank;
    draft_.target.scoreDenominator = 27;
  }
  customScore_ = draft_.target.scoreNumerator > 0 && draft_.target.scoreDenominator != 27;
  customRate_ = rateText(draft_.target);
}
void ChartGoalsScene::changeDraftMode(int mode) {
  const auto target = draft_.target;
  const auto custom = customScore_;
  const auto rate = customRate_;
  showGoal({.chart=draft_.chart, .longNoteMode=mode});
  if (loadFailed_) return;
  editing_ = true;
  draft_.target = target;
  customScore_ = custom;
  customRate_ = rate;
}
void ChartGoalsScene::showGoal(chart_goals::Goal goal) {
  message_.clear(); confirmRemove_ = false;
  std::string error;
  selected_ = context.scoreRepository.LoadChartGoal(goal.chart, goal.longNoteMode, error);
  loadFailed_ = !error.empty();
  if (loadFailed_) { message_ = i18n::tr("goals.ui.storage_error"); return; }
  draft_ = selected_ ? selected_->goal : std::move(goal);
  editing_ = !selected_;
  customScore_ = draft_.target.scoreNumerator > 0 && draft_.target.scoreDenominator != 27;
  customRate_ = rateText(draft_.target);
  resolveChart();
}
void ChartGoalsScene::resolveChart() {
  resolved_.reset();
  if (!selected_ && !editing_) return;
  auto session = context.chartRepository.OpenSession();
  if (!session) return;
  ChartMetaQuery query;
  query.allowedSha256 = std::vector{chart_goals::normalizedHash(draft_.chart.SHA256)};
  query.limit = 1;
  std::vector<ChartMetaRecord> records;
  session->QueryChartMeta(query, records);
  if (!records.empty() && !records.front().unavailable && !records.front().solidArchive &&
      !records.front().meta.BmsPath.empty() &&
      chart_goals::normalizedHash(records.front().meta.SHA256) == query.allowedSha256->front())
    resolved_ = std::move(records.front());
}
void ChartGoalsScene::saveGoal() {
  if (!checkProfile()) { reload(); return; }
  auto candidate = draft_;
  if (customScore_) {
    const auto rate = chart_goals::parseRate(customRate_);
    if (!rate) { message_ = i18n::tr("kombo.ui.invalid_rate"); return; }
    candidate.target.scoreNumerator = *rate; candidate.target.scoreDenominator = 10000;
  }
  try { chart_goals::validate(candidate.target); }
  catch (const std::exception &) { message_ = i18n::tr("kombo.ui.target_required"); return; }
  std::string error;
  if (!context.scoreRepository.SaveChartGoal(candidate, error)) {
    if (error == "goal_already_achieved") {
      message_ = i18n::tr("goals.ui.already_achieved");
    } else {
      SDL_Log("Cannot save personal goal: %s", error.c_str());
      message_ = i18n::tr("goals.ui.storage_error");
    }
    return;
  }
  draft_ = std::move(candidate);
  selected_ = chart_goals::GoalProgress{draft_, {}};
  editing_ = false; message_.clear(); reload();
}
void ChartGoalsScene::removeGoal() {
  if (!checkProfile()) { reload(); return; }
  if (!confirmRemove_ || !selected_) return;
  std::string error;
  if (!context.scoreRepository.RemoveChartGoal(draft_.chart, draft_.longNoteMode, error)) {
    SDL_Log("Cannot remove personal goal: %s", error.c_str());
    message_ = i18n::tr("goals.ui.storage_error"); return;
  }
  if (page_.goals.size() == 1 && offset_ > 0) offset_ -= chart_goals::pageSize;
  selected_.reset(); resolved_.reset(); editing_ = confirmRemove_ = false;
  message_.clear(); reload();
}
void ChartGoalsScene::navigate(bool practice) {
  defer([this, profile = profileId_, practice] {
    if (profile != context.profileManager.activeProfile().id) { checkProfile(); reload(); build(); return true; }
    if (context.storageMaintenanceActive) return true;
    resolveChart();
    if (!resolved_) { message_ = i18n::tr("goals.ui.missing"); build(); return true; }
    auto record = *resolved_;
    const auto mode = draft_.longNoteMode;
    if (practice) {
      if (mode > 0) {
        record.meta.LnMode = mode;
        context.settings.selectedLnMode = long_note_mode::idFromValue(mode);
        (void)context.saveSettings();
      }
      context.sceneManager->changeScene(std::make_unique<ChartViewerScene>(context,
          std::move(record), std::nullopt, std::nullopt, std::nullopt, std::nullopt,
          SceneReturnTarget::Retained(this)), true);
      return true; // This scene is retained: consume the callback before resume.
    }
    const auto selectChart = onSelectChart_;
    const auto target = returnTarget_;
    auto *manager = context.sceneManager;
    if (selectChart) selectChart(record, mode);
    (void)returnToScene(*manager, target);
    return false;
  }, 0, true);
}
std::string ChartGoalsScene::scrollKey() const {
  if (selected_ || editing_)
    return std::string(editing_ ? "edit:" : "detail:") + draft_.chart.SHA256 + ":" + std::to_string(draft_.longNoteMode);
  if (overviewTab_ == OverviewTab::Recommendations) {
    const int rank = recommendationRank_.value_or(-1);
    const auto found = recommendations_.find(rank);
    const int page = found == recommendations_.end() ? 0 : found->second.offset;
    return "recommend:" + std::to_string(rank) + ":" + std::to_string(page) + ":" +
        std::to_string(recommendationTable_) + ":" + std::to_string(recommendationSortTable_);
  }
  return overviewTab_ == OverviewTab::Ongoing ? "ongoing:" + std::to_string(offset_)
                                               : "achieved:" + std::to_string(achievedOffset_);
}
void ChartGoalsScene::build() {
  if (scroll_ && !renderedScrollKey_.empty()) scrollOffsets_[renderedScrollKey_] = scroll_->getScrollOffset();
  if (navigationScroll_) navigationOffset_ = navigationScroll_->getScrollOffset();
  for (auto *view : views) delete view;
  views.clear();
  root_ = new View(0, 0, rendering::window_width, rendering::window_height);
  root_->setFlexDirection(FlexDirection::Column)->setGap(12)->setPadding(Edge::All, 18)
      ->setThemedBackgroundColor(app_theme::mainMenuBackdrop);
  addView(root_);
  action(root_, i18n::tr("kombo.ui.back"), [this] { back(); })->setWidth(140);
  auto *content = new View();
  content->setFlexDirection(FlexDirection::Row)->setGap(12)->setFlex(1)->setMinHeight(0);
  root_->addView(content);
  navigationScroll_ = nullptr;
  if (!editing_ && !selected_) {
    navigationScroll_ = new ScrollView();
    navigationScroll_->setWidthPercent(30)->setMinHeight(0);
    content->addView(navigationScroll_);
    auto *navigation = new View();
    navigation->setFlexDirection(FlexDirection::Column)->setGap(6);
    const auto tab = [this, navigation](const std::string &title, OverviewTab value) {
      action(navigation, title, [this, value] { later([this, value] {
        overviewTab_ = value;
        if (value == OverviewTab::Recommendations && !recommendationRank_ && !recommendations_.empty()) {
          recommendationRank_ = recommendations_.begin()->first;
          loadRecommendations(*recommendationRank_, 0);
        }
      }); },
          overviewTab_ == value ? app_theme::ActionCategory::Primary : app_theme::ActionCategory::Neutral);
    };
    tab(i18n::tr("goals.ui.in_progress"), OverviewTab::Ongoing);
    tab(i18n::tr("goals.ui.recommendations"), OverviewTab::Recommendations);
    if (overviewTab_ == OverviewTab::Recommendations) {
      for (const auto &[rank, group] : recommendations_) {
        auto *sub = action(navigation, i18n::format("goals.ui.near_rank", {
            {"rank", rank >= 50 ? kombo::lampLabel(rank) : kombo::scoreRankLabel(rank)},
            {"count", std::to_string(group.count)}}), [this, rank] { later([this, rank] {
              recommendationRank_ = rank; loadRecommendations(rank, recommendations_[rank].offset);
            }); }, recommendationRank_ == rank ? app_theme::ActionCategory::Primary : app_theme::ActionCategory::Neutral);
        sub->setMargin(Edge::Left, 16);
        if (rank >= kClearTypeLightAssistedEasyClearRank)
          sub->addView(new GoalLampTabAccent(rank));
      }
    }
    tab(i18n::tr("goals.ui.achieved"), OverviewTab::Achieved);
    navigationScroll_->setContentView(navigation);
  }
  scroll_ = new ScrollView(); scroll_->setFlex(1)->setMinWidth(0)->setMinHeight(0); content->addView(scroll_);
  auto *outer = new View(); outer->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignCenter);
  auto *body = new View();
  body->setWidthPercent(100)->setFlexDirection(FlexDirection::Column)->setGap(16)->setPadding(Edge::Bottom, 24);
  YGNodeStyleSetMaxWidth(body->getNode(), 960); outer->addView(body); scroll_->setContentView(outer);
  portal_ = new OverlayPortal(0, 0, rendering::window_width, rendering::window_height);
  portal_->setPositionType(YGPositionTypeAbsolute)->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0)->setZIndex(900);
  label(body, i18n::tr("goals.ui.title"), 30);
  if (loadFailed_) {
    action(body, i18n::tr("kombo.ui.retry"), [this] { later([this] { message_.clear(); reload(); }); });
  } else if (selected_ || editing_) {
    auto *detail = card(body);
    goalIdentity(detail, draft_.chart, resolved_ ? &*resolved_ : nullptr, 28);
    const bool selectableMode = scoreLongNoteModeForClearLamp(draft_.chart, 1) == 1 &&
        scoreLongNoteModeForClearLamp(draft_.chart, 2) == 2 &&
        scoreLongNoteModeForClearLamp(draft_.chart, 3) == 3;
    if (selectableMode) {
      auto *modes = new View();
      modes->setFlexDirection(FlexDirection::Row)->setGap(8)->setFlexShrink(0);
      for (int mode : long_note_mode::kPlayableValues) {
        auto *button = action(modes, long_note_mode::idFromValue(mode), [this, mode] {
          later([this, mode] {
            if (editing_) changeDraftMode(mode);
            else showGoal({.chart=draft_.chart, .longNoteMode=mode});
          });
        });
        button->setFlex(1);
        button->setSelected(mode == draft_.longNoteMode);
      }
      detail->addView(modes);
    } else label(detail, scope(draft_), 18)->setThemedColor(app_theme::textSecondary);
    if (editing_) {
      std::string error;
      const auto current = context.scoreRepository.PreviewChartGoal(draft_, error);
      if (current) {
        const auto &progress = current->progress;
        lamp(detail, i18n::format("goals.ui.best_lamp", {{"value", kombo::lampLabel(
            progress.bestClear ? progress.bestClear->clearType : -1)}}),
            progress.bestClear ? progress.bestClear->clearType : -1);
        label(detail, i18n::format("goals.ui.best_score", {{"value", bestScoreText(progress.bestScore)}}));
      } else label(detail, i18n::tr("goals.ui.storage_error"));
      label(detail, i18n::tr("goals.ui.target_help"), 18);
      std::vector<DropdownView::Option> lamps{{"-1", i18n::tr("goals.ui.no_target")}};
      for (int rank : {50, 100, 200, 300, 400, 500, 600}) lamps.push_back({.id=std::to_string(rank), .label=kombo::lampLabel(rank), .leadingColor=clearLampColorForRank(rank)});
      select(detail, portal_, i18n::tr("goals.ui.lamp"), std::move(lamps), std::to_string(draft_.target.lamp),
          [this](const std::string &id) { draft_.target.lamp = std::stoi(id); });
      std::vector<DropdownView::Option> scores{{"0", i18n::tr("goals.ui.no_target")}};
      for (int rank : {6, 9, 12, 15, 18, 21, 24, 26, 27}) scores.push_back({std::to_string(rank), kombo::scoreRankLabel(rank)});
      scores.push_back({"custom", i18n::tr("kombo.ui.custom_option")});
      select(detail, portal_, i18n::tr("goals.ui.score"), std::move(scores),
          customScore_ ? "custom" : std::to_string(draft_.target.scoreNumerator), [this](const std::string &id) {
            later([this, id] {
              customScore_ = id == "custom";
              if (!customScore_) { draft_.target.scoreNumerator = std::stoi(id); draft_.target.scoreDenominator = 27; }
            });
          });
      if (customScore_) {
        label(detail, i18n::tr("kombo.ui.custom_rate"), 18);
        auto *input = new TextInputBox(app_theme::font, 22);
        input->setHeight(60)->setPadding(Edge::All, 10)->setCornerRadius(14)->setFlexShrink(0);
        input->setThemedColor(app_theme::textPrimary); input->setThemedBackgroundColor(app_theme::insetSurface);
        input->setEditingText(customRate_); input->onTextChanged([this](const auto &value) { customRate_ = value; });
        detail->addView(input);
      }
      action(detail, i18n::tr("goals.ui.save"), [this] { later([this] { saveGoal(); }); }, app_theme::ActionCategory::Primary);
    } else {
      targets(detail, draft_.target);
      const auto setDate = chart_goals::localDateTime(draft_.setAtUnixMillis);
      label(detail, i18n::format("goals.ui.set_date", {{"date", setDate.empty()
          ? i18n::tr("goals.ui.date_unknown") : setDate}}), 18)->setThemedColor(app_theme::textSecondary);
      if (draft_.achievedAtUnixMillis > 0 || selected_->progress.achieved) {
        const auto achievedDate = chart_goals::localDateTime(draft_.achievedAtUnixMillis);
        label(detail, i18n::format("goals.ui.achieved_date", {{"date", achievedDate.empty()
            ? i18n::tr("goals.ui.date_unknown") : achievedDate}}), 18)->setThemedColor(app_theme::textSecondary);
      }
      const auto &progress = selected_->progress;
      label(detail, i18n::tr(progress.achieved ? "goals.ui.achieved" : "goals.ui.in_progress"), 25)
          ->setThemedColor(progress.achieved ? app_theme::lime : app_theme::textPrimary);
      lamp(detail, i18n::format("goals.ui.best_lamp", {{"value", kombo::lampLabel(
          progress.bestClear ? progress.bestClear->clearType : -1)}}),
          progress.bestClear ? progress.bestClear->clearType : -1);
      label(detail, i18n::format("goals.ui.best_score", {{"value", bestScoreText(progress.bestScore)}}));
      if (draft_.target.scoreNumerator > 0)
        label(detail, progress.remainingEx ? i18n::format("goals.ui.ex_remaining", {{"count", std::to_string(*progress.remainingEx)}})
                                          : i18n::tr("goals.ui.unknown_gap"));
      label(detail, i18n::tr("goals.ui.evidence_help"), 17)->setThemedColor(app_theme::textSecondary);
      if (!resolved_) label(detail, i18n::tr("goals.ui.missing"), 18);
      action(detail, i18n::tr("goals.ui.open_play"), [this] { navigate(false); }, app_theme::ActionCategory::Primary)
          ->setEnabled(resolved_.has_value() && bool(onSelectChart_));
      action(detail, i18n::tr("goals.ui.practice"), [this] { navigate(true); })->setEnabled(resolved_.has_value());
      action(detail, i18n::tr("goals.ui.edit"), [this] { later([this] {
        editing_ = true; message_.clear(); confirmRemove_ = false;
        customScore_ = draft_.target.scoreNumerator > 0 && draft_.target.scoreDenominator != 27;
        customRate_ = rateText(draft_.target);
      }); });
      if (confirmRemove_) {
        label(detail, i18n::tr("goals.ui.remove_confirm"), 18);
        action(detail, i18n::tr("goals.ui.remove"), [this] { later([this] { removeGoal(); }); }, app_theme::ActionCategory::Destructive);
        action(detail, i18n::tr("goals.ui.cancel"), [this] { later([this] { confirmRemove_ = false; }); });
      } else action(detail, i18n::tr("goals.ui.remove"), [this] { later([this] { confirmRemove_ = true; }); });
    }
  } else {
    if (overviewTab_ == OverviewTab::Recommendations) {
      label(body, i18n::tr("goals.ui.recommendations"), 24);
      label(body, i18n::tr("goals.ui.near_help"), 18);
      label(body, i18n::tr("goals.ui.gauge_near_help"), 18);
      std::vector<DropdownView::Option> tables{{"0", i18n::tr("goals.ui.all_tables")}};
      for (const auto &table : recommendationTables_)
        tables.push_back({std::to_string(table.id), table.name});
      select(body, portal_, i18n::tr("goals.ui.table_filter"), std::move(tables),
          std::to_string(recommendationTable_), [this](const std::string &id) { later([this, id] {
            recommendationTable_ = std::stoi(id); reload();
          }); });
      if (recommendationTable_ != 0) {
        auto *sorting = new View();
        sorting->setFlexDirection(FlexDirection::Row)->setGap(8)->setFlexShrink(0);
        body->addView(sorting);
        for (const bool byTable : {false, true}) {
          auto *button = action(sorting, i18n::tr(byTable ? "goals.ui.sort_table" : "goals.ui.sort_nearness"),
              [this, byTable] { later([this, byTable] { recommendationSortTable_ = byTable;
                if (recommendationRank_ && recommendations_.contains(*recommendationRank_)) loadRecommendations(*recommendationRank_, 0);
              }); }, recommendationSortTable_ == byTable ? app_theme::ActionCategory::Primary : app_theme::ActionCategory::Neutral);
          button->setFlex(1);
        }
      }
      if (recommendationRank_ && recommendations_.contains(*recommendationRank_)) {
      const int rank = *recommendationRank_;
      const auto &group = recommendations_.at(rank);
      for (const auto &[candidate, record] : group.rows) {
        auto *row = card(body);
        goalIdentity(row, record.meta, &record, 24);
        if (rank>=50) {
          std::ostringstream value;
          value << std::fixed << std::setprecision(1);
          if (rank==600) {
            label(row, i18n::format("goals.ui.combo_breaks", {{"count",std::to_string(candidate.remaining)}}), 18);
          } else if (rank<=300) {
            value << candidate.gaugeValue;
            std::ostringstream border; border << candidate.gaugeBorder;
            label(row, i18n::format("goals.ui.gauge_value", {{"value",value.str()}, {"border",border.str()}}), 18);
          } else if (candidate.deathNotes<0) label(row, i18n::tr("goals.ui.gauge_survived"), 18);
          else {
            value << 100.0*candidate.deathNotes/candidate.totalNotes;
            label(row, i18n::format("goals.ui.gauge_death", {{"percent",value.str()}}), 18);
          }
        } else label(row, std::to_string(candidate.score) + " / " + std::to_string(candidate.maxScore) + " EX", 18);
        const auto caption = rank>=50 ? i18n::format("goals.ui.recommend_lamp", {{"lamp",kombo::lampLabel(rank)}})
            : i18n::format("goals.ui.recommend_ex", {{"ex",std::to_string(candidate.remaining)}, {"rank",kombo::scoreRankLabel(rank)}});
        action(row, caption, [this, record, mode=candidate.longNoteMode, rank] {
          later([this, record, mode, rank] { recommendGoal(record, mode, rank); });
        });
      }
      if (group.offset > 0) action(body, i18n::tr("goals.ui.previous"), [this, rank, offset=group.offset] {
        later([this, rank, offset] { loadRecommendations(rank, offset-chart_goals::pageSize); });
      });
      if (group.offset + chart_goals::pageSize < group.count)
        action(body, i18n::tr("goals.ui.next"), [this, rank, offset=group.offset] {
          later([this, rank, offset] { loadRecommendations(rank, offset+chart_goals::pageSize); });
        });
    }
    }
    const auto renderGroup = [this, body](bool achieved, int &offset) {
      std::vector<const chart_goals::GoalProgress *> entries;
      for (const auto &entry : page_.goals)
        if (entry.progress.achieved == achieved) entries.push_back(&entry);
      const int count = static_cast<int>(entries.size());
      offset = std::min(offset, count == 0 ? 0 : (count - 1) / chart_goals::pageSize * chart_goals::pageSize);
      const auto artwork = loadGoalArtwork(context, entries, offset);
      for (int i = offset; i < std::min(count, offset + chart_goals::pageSize); ++i) {
        const auto &entry = *entries[i];
        auto *row = card(body);
        const auto found = artwork.find(chart_goals::normalizedHash(entry.goal.chart.SHA256));
        goalIdentity(row, entry.goal.chart, found == artwork.end() ? nullptr : &found->second, 24);
        label(row, scope(entry.goal), 18)->setThemedColor(app_theme::textSecondary);
        targets(row, entry.goal.target);
        action(row, i18n::tr(achieved ? "goals.ui.achieved" : "goals.ui.in_progress"),
            [this, goal = entry.goal] { later([this, goal] { showGoal(goal); }); });
      }
      if (offset > 0) action(body, i18n::tr("goals.ui.previous"), [this, achieved] { later([this, achieved] {
        (achieved ? achievedOffset_ : offset_) -= chart_goals::pageSize;
      }); });
      if (offset + chart_goals::pageSize < count) action(body, i18n::tr("goals.ui.next"), [this, achieved] { later([this, achieved] {
        (achieved ? achievedOffset_ : offset_) += chart_goals::pageSize;
      }); });
    };
    if (overviewTab_ == OverviewTab::Ongoing) {
      label(body, i18n::tr("goals.ui.intro"), 19);
      if (page_.goals.empty()) label(body, i18n::format("goals.ui.empty", {{"flag", "⚑"}}));
      renderGroup(false, offset_);
    } else if (overviewTab_ == OverviewTab::Achieved) renderGroup(true, achievedOffset_);
  }
  if (!message_.empty()) label(body, message_, 19)->setThemedColor(app_theme::coral);
  addView(portal_); layout();
  renderedScrollKey_ = scrollKey();
  if (const auto found = scrollOffsets_.find(renderedScrollKey_); found != scrollOffsets_.end())
    scroll_->setScrollOffset(found->second);
}
void ChartGoalsScene::layout() {
  if (!root_) return;
  root_->setSize(rendering::window_width, rendering::window_height);
  portal_->setSize(rendering::window_width, rendering::window_height);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const auto safe = GetIOSSafeAreaInsetsNormalized();
  root_->setPadding(Edge::Top, 18 + safe.top * rendering::window_height)
      ->setPadding(Edge::Bottom, 18 + safe.bottom * rendering::window_height)
      ->setPadding(Edge::Left, 18 + safe.left * rendering::window_width)
      ->setPadding(Edge::Right, 18 + safe.right * rendering::window_width);
#endif
  root_->applyYogaLayout(); scroll_->refreshContentLayout();
  if (navigationScroll_) {
    navigationScroll_->refreshContentLayout();
    navigationScroll_->setScrollOffset(navigationOffset_);
  }
}
void ChartGoalsScene::back() {
  if (confirmRemove_) { later([this] { confirmRemove_ = false; }); return; }
  if (editing_ && selected_) { later([this] { editing_ = false; message_.clear(); reload(); }); return; }
  if (editing_ || selected_) {
    later([this] { editing_ = false; selected_.reset(); message_.clear(); reload(); }); return;
  }
  defer([this] { (void)returnToScene(*context.sceneManager, returnTarget_); return false; }, 0, true);
}
void ChartGoalsScene::onPause() {
  const auto finish = [](View *view, const auto &self) -> void {
    if (auto *input = dynamic_cast<TextInputBox *>(view)) input->endEditing();
    for (auto *child : view->getChildren()) self(child, self);
  };
  for (auto *view : views) { view->cancelPointerInput(); finish(view, finish); }
}
void ChartGoalsScene::onResume() { checkProfile(); reload(); build(); }
void ChartGoalsScene::update(float) {
  if (!checkProfile()) { reload(); build(); }
  if (root_ && (root_->getWidth() != rendering::window_width ||
                root_->getHeight() != rendering::window_height)) layout();
}
void ChartGoalsScene::onPresentationOrientationChanged() { layout(); }
void ChartGoalsScene::onLanguageChanged() { build(); }
EventHandleResult ChartGoalsScene::handleEvents(SDL_Event &event) {
  for (auto it = views.rbegin(); it != views.rend(); ++it) if (!(*it)->handleEvents(event)) return {};
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) back();
  return {};
}
void ChartGoalsScene::cleanupScene() { root_ = nullptr; scroll_ = nullptr; navigationScroll_ = nullptr; portal_ = nullptr; }
