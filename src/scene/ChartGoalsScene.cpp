#include "ChartGoalsScene.h"
#include "ChartViewerScene.h"
#include "SceneManager.h"
#include "../LongNoteModeUtils.h"
#include "../rendering/common.h"
#include "../rendering/ResponsiveUi.h"
#include "../view/ActionButtonStyle.h"
#include "../view/ActionButtonContent.h"
#include "../view/AppTheme.h"
#include "../view/Button.h"
#include "../view/ClearLampColors.h"
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
  query.rawSongData = true;
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
std::string scoreTarget(const chart_goals::Target &target) {
  if (target.scoreDenominator == 27) return kombo::scoreRankLabel(target.scoreNumerator);
  return rateText(target) + "% EX";
}
std::string scope(const chart_goals::Goal &goal) {
  return goal.longNoteMode == 0 ? i18n::tr("goals.ui.shared_scope") : long_note_mode::idFromValue(goal.longNoteMode);
}
void targets(View *parent, const chart_goals::Target &target) {
  if (target.lamp >= 0)
    lamp(parent, i18n::format("goals.ui.lamp_target", {{"value", kombo::lampLabel(target.lamp)}}), target.lamp);
  if (target.scoreNumerator > 0)
    label(parent, i18n::format("goals.ui.score_target", {{"value", scoreTarget(target)}}));
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
                               std::optional<int> initialMode)
    : Scene(context), returnTarget_(std::move(target)), onSelectChart_(std::move(onSelect)),
      initial_(std::move(initial)), initialMode_(initialMode), profileId_(context.profileManager.activeProfile().id) {}
bool ChartGoalsScene::checkProfile() {
  if (profileId_ == context.profileManager.activeProfile().id) return true;
  profileId_ = context.profileManager.activeProfile().id;
  initial_.reset(); initialMode_.reset(); resolved_.reset(); selected_.reset();
  draft_ = {}; page_ = {}; offset_ = achievedOffset_ = 0; showAchieved_ = false;
  editing_ = customScore_ = confirmRemove_ = loadFailed_ = false;
  message_.clear(); customRate_.clear();
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
  } else reload();
  build();
}
void ChartGoalsScene::reload() {
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
  query.rawSongData = true; query.limit = 1;
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
void ChartGoalsScene::build() {
  for (auto *view : views) delete view;
  views.clear();
  root_ = new View(0, 0, rendering::window_width, rendering::window_height);
  root_->setFlexDirection(FlexDirection::Column)->setGap(12)->setPadding(Edge::All, 18)
      ->setThemedBackgroundColor(app_theme::mainMenuBackdrop);
  addView(root_);
  action(root_, i18n::tr("kombo.ui.back"), [this] { back(); })->setWidth(140);
  scroll_ = new ScrollView(); scroll_->setFlex(1)->setMinHeight(0); root_->addView(scroll_);
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
    auto *identity = new View();
    identity->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(12)->setFlexShrink(0);
    goalArtwork(identity, resolved_ ? &*resolved_ : nullptr);
    label(identity, draft_.chart.Title + (draft_.chart.SubTitle.empty() ? "" : " " + draft_.chart.SubTitle), 28)->setFlex(1)->setMinWidth(0);
    detail->addView(identity);
    label(detail, scope(draft_), 18)->setThemedColor(app_theme::textSecondary);
    if (editing_) {
      std::string error;
      const auto current = context.scoreRepository.PreviewChartGoal(draft_, error);
      if (current) {
        const auto &progress = current->progress;
        lamp(detail, i18n::format("goals.ui.best_lamp", {{"value", kombo::lampLabel(
            progress.bestClear ? progress.bestClear->clearType : -1)}}),
            progress.bestClear ? progress.bestClear->clearType : -1);
        std::string best = i18n::tr("goals.ui.no_record");
        if (progress.bestScore) {
          best = std::to_string(progress.bestScore->score) + " EX";
          if (progress.bestScore->maxScore > 0) best += " / " + std::to_string(progress.bestScore->maxScore);
        }
        label(detail, i18n::format("goals.ui.best_score", {{"value", best}}));
      } else label(detail, i18n::tr("goals.ui.storage_error"));
      label(detail, i18n::tr("goals.ui.target_help"), 18);
      std::vector<DropdownView::Option> lamps{{"-1", i18n::tr("goals.ui.no_target")}};
      for (int rank : {100, 150, 200, 300, 400, 500, 600}) lamps.push_back({.id=std::to_string(rank), .label=kombo::lampLabel(rank), .leadingColor=clearLampColorForRank(rank)});
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
      // Selectable long-note charts can have separate goals for each mode.
      if (scoreLongNoteModeForClearLamp(draft_.chart, 1) == 1 &&
          scoreLongNoteModeForClearLamp(draft_.chart, 2) == 2 &&
          scoreLongNoteModeForClearLamp(draft_.chart, 3) == 3) {
        auto *modes = new View();
        modes->setFlexDirection(FlexDirection::Row)->setGap(8)->setFlexShrink(0);
        for (int mode : long_note_mode::kPlayableValues) {
          auto *button = action(modes, long_note_mode::idFromValue(mode), [this, mode] {
            later([this, mode] { showGoal({.chart=draft_.chart, .longNoteMode=mode}); });
          });
          button->setFlex(1);
          button->setSelected(mode == draft_.longNoteMode);
        }
        detail->addView(modes);
      }
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
      std::string best = i18n::tr("goals.ui.no_record");
      if (progress.bestScore) {
        best = std::to_string(progress.bestScore->score) + " EX";
        if (progress.bestScore->maxScore > 0) best += " / " + std::to_string(progress.bestScore->maxScore);
      }
      label(detail, i18n::format("goals.ui.best_score", {{"value", best}}));
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
    label(body, i18n::tr("goals.ui.intro"), 19);
    if (page_.goals.empty()) label(body, i18n::format("goals.ui.empty", {{"flag", "⚑"}}));
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
        auto *identity = new View();
        identity->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(12)->setFlexShrink(0);
        const auto found = artwork.find(chart_goals::normalizedHash(entry.goal.chart.SHA256));
        goalArtwork(identity, found == artwork.end() ? nullptr : &found->second);
        label(identity, entry.goal.chart.Title + (entry.goal.chart.SubTitle.empty() ? "" : " " + entry.goal.chart.SubTitle), 24)->setFlex(1)->setMinWidth(0);
        row->addView(identity);
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
    renderGroup(false, offset_);
    if (std::ranges::any_of(page_.goals, [](const auto &entry) { return entry.progress.achieved; })) {
      action(body, i18n::tr(showAchieved_ ? "goals.ui.hide_achieved" : "goals.ui.show_achieved"),
          [this] { later([this] { showAchieved_ = !showAchieved_; }); },
          app_theme::ActionCategory::Neutral, showAchieved_ ? ui_icons::kHide : ui_icons::kPlus);
      if (showAchieved_) renderGroup(true, achievedOffset_);
    }
  }
  if (!message_.empty()) label(body, message_, 19)->setThemedColor(app_theme::coral);
  addView(portal_); layout();
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
void ChartGoalsScene::cleanupScene() { root_ = nullptr; scroll_ = nullptr; portal_ = nullptr; }
