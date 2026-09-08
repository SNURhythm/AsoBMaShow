#pragma once

#include "../ReplayRecordFilters.h"
#include "../ResultRecordSummary.h"
#include "../replay/ReplayFileActionSelection.h"
#include "../repositories/ChartRepository.h"

#include <SDL2/SDL.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ReplayVideoExportOptions;

class Button;
class ResultRecordListView;
class TextView;
class View;

namespace replay_records_modal {
struct ClearFilterButton {
  Button *button = nullptr;
  TextView *text = nullptr;
  std::optional<int> rank;
};

struct OptionFilterButton {
  Button *button = nullptr;
  TextView *text = nullptr;
  std::optional<std::string> option;
};

struct ScoreRankFilterButton {
  Button *button = nullptr;
  TextView *text = nullptr;
  std::optional<std::string> rank;
};

struct SortButton {
  Button *button = nullptr;
  TextView *text = nullptr;
  ReplayRecordSortCriterion criterion = ReplayRecordSortCriterion::Newest;
};
} // namespace replay_records_modal

enum class ReplayRecordsModalAction {
  Watch,
  VideoExport,
};

// A retained chart-records overlay shared by Main Menu and Music Select.
// Scene owners supply data loading and the operations that leave the scene,
// while the modal owns the Records UI, row selection, filter/sort state,
// watch and export option sub-pages, export progress, delete confirmation,
// and every action's eligibility.
struct ReplayRecordsModalCallbacks {
  std::function<std::vector<ResultRecordSummary>(const ChartMetaRecord &)>
      loadRecords;
  std::function<void(const ChartMetaRecord &, const ModernChartResultRecord &)>
      watchModernChart;
  std::function<void(const ChartMetaRecord &, const ModernCourseResultRecord &)>
      watchModernCourse;
  std::function<void(const ChartMetaRecord &)> watchAutoPlay;
  std::function<void(const ChartMetaRecord &, const ModernChartResultRecord &)>
      gbattle;
  std::function<void(const ChartMetaRecord &, const ModernChartResultRecord &)>
      recallModernChart;
  std::function<void(const ModernCourseResultRecord &, bool)>
      recallModernCourse;
  std::function<void(const IrRemoteRecordId &, const std::string &)>
      recallRemote;
  std::function<void(const ChartMetaRecord &, const ModernChartResultRecord &,
                     ReplayVideoExportOptions)>
      exportModernChart;
  std::function<void(const ModernCourseResultRecord &, ReplayVideoExportOptions)>
      exportModernCourse;
  std::function<void(const ChartMetaRecord &, ReplayVideoExportOptions)>
      exportAutoPlay;
  std::function<void(const replay::ReplayFileActionRequest &)> share;
  std::function<void(const replay::ReplayFileActionRequest &)> remove;
  std::function<void(const ModernChartResultRecord &)> irUpload;
  std::function<void(ir::IrRecordState)> irStatusFeedback;
};

class ReplayRecordsModal final {
public:
  static std::unique_ptr<ReplayRecordsModal>
  Create(View *parent, ReplayRecordsModalCallbacks callbacks);
  ~ReplayRecordsModal();
  ReplayRecordsModal(const ReplayRecordsModal &) = delete;
  ReplayRecordsModal &operator=(const ReplayRecordsModal &) = delete;

  [[nodiscard]] View *root() const noexcept { return root_; }
  [[nodiscard]] bool isVisible() const noexcept;

  void showChart(const ChartMetaRecord &record);
  void hide();
  void resize(int width, int height);
  bool handleEvents(SDL_Event &event);
  void update();

  void setStatus(std::string text);
  void setTouchVisualizationEnabled(bool enabled);
  void reloadRecords(bool preserveViewState = true);
  void refresh();
  void setExportInProgress(bool inProgress);
  void setResultRecallInProgress(bool inProgress);
  void setIrUploadInProgress(bool inProgress);
  void setLoadInProgress(bool inProgress);
  void setDocumentHandoffActive(bool active);
  void showExportProgress(const std::string &title,
                          const std::string &message);
  void updateExportProgress(double fraction, const std::string &message);
  void returnToList(const std::string &status = {});
  void showIrFeedback(const std::string &message);
  [[nodiscard]] bool renderTouchPoints() const noexcept {
    return selectedReplayRenderTouchPoints_;
  }
  [[nodiscard]] bool renderReplayGhosts() const noexcept {
    return selectedReplayRenderGhosts_;
  }
  [[nodiscard]] const std::vector<ResultRecordSummary> &
  records() const noexcept {
    return records_;
  }

  // Test-facing action boundary. Scenes and tests share the exact
  // capability-to-owner dispatch through `activate`/`dispatchAction`.
  void selectRecord(const ResultRecordSummary &record);
  [[nodiscard]] const std::optional<ResultRecordSummary> &
  selection() const noexcept {
    return selected_;
  }
  bool activate(ReplayRecordsModalAction action);
  static bool dispatchAction(ReplayRecordsModalAction action,
                             const ChartMetaRecord &record,
                             const ResultRecordSummary &summary,
                             const ReplayRecordsModalCallbacks &callbacks);

private:
  ReplayRecordsModal();

  void select(int index);
  void clearSelection();
  void applyFilters(std::optional<std::string> preferredStableKey =
                        std::nullopt);
  void refreshActions();
  void refreshFilterSortButtons();
  void refreshExportOptionButtons();
  void showFilterSortOptions();
  void showExportOptions();
  void showDeleteConfirmation();
  void cancelDeleteConfirmation();
  void dispatchWatch(const ResultRecordSummary &summary);
  void dispatchExport(const ResultRecordSummary &summary);
  void dispatchResult(const ResultRecordSummary &summary);
  void showListPage();
  [[nodiscard]] bool selectedIsAutoPlay() const;
  [[nodiscard]] bool scoreRankFilterAvailable() const;
  [[nodiscard]] bool operationInProgress() const noexcept;
  [[nodiscard]] bool canHide() const noexcept;
  void updateTitleReset();

  View *root_ = nullptr;
  View *contentFrame_ = nullptr;
  View *listContent_ = nullptr;
  View *filterSortContent_ = nullptr;
  View *watchOptionsContent_ = nullptr;
  View *exportOptionsContent_ = nullptr;
  View *exportProgressContent_ = nullptr;
  View *deleteConfirmationContent_ = nullptr;
  View *exportProgressTrack_ = nullptr;
  View *exportProgressFill_ = nullptr;
  TextView *title_ = nullptr;
  TextView *status_ = nullptr;
  TextView *exportProgressMessageText_ = nullptr;
  TextView *exportProgressPercentText_ = nullptr;
  ResultRecordListView *list_ = nullptr;
  Button *watchButton_ = nullptr;
  Button *gbattleButton_ = nullptr;
  Button *resultButton_ = nullptr;
  Button *exportButton_ = nullptr;
  Button *shareButton_ = nullptr;
  Button *deleteButton_ = nullptr;
  Button *deleteCancelButton_ = nullptr;
  Button *deleteConfirmButton_ = nullptr;
  Button *filterButton_ = nullptr;
  Button *closeButton_ = nullptr;
  Button *fps60Button_ = nullptr;
  Button *fps120Button_ = nullptr;
  Button *resolution1080Button_ = nullptr;
  Button *resolutionFullButton_ = nullptr;
  Button *resultIncludeButton_ = nullptr;
  Button *resultSkipButton_ = nullptr;
  Button *touchShowButton_ = nullptr;
  Button *touchHideButton_ = nullptr;
  Button *ghostShowButton_ = nullptr;
  Button *ghostHideButton_ = nullptr;
  Button *exportTouchShowButton_ = nullptr;
  Button *exportTouchHideButton_ = nullptr;
  Button *exportGhostShowButton_ = nullptr;
  Button *exportGhostHideButton_ = nullptr;
  TextView *watchButtonText_ = nullptr;
  TextView *gbattleButtonText_ = nullptr;
  TextView *resultButtonText_ = nullptr;
  TextView *exportButtonText_ = nullptr;
  TextView *shareButtonText_ = nullptr;
  TextView *deleteButtonText_ = nullptr;
  TextView *deleteCancelButtonText_ = nullptr;
  TextView *deleteConfirmButtonText_ = nullptr;
  TextView *filterButtonText_ = nullptr;
  TextView *closeButtonText_ = nullptr;
  TextView *fps60ButtonText_ = nullptr;
  TextView *fps120ButtonText_ = nullptr;
  TextView *resolution1080ButtonText_ = nullptr;
  TextView *resolutionFullButtonText_ = nullptr;
  TextView *resultIncludeButtonText_ = nullptr;
  TextView *resultSkipButtonText_ = nullptr;
  TextView *touchShowButtonText_ = nullptr;
  TextView *touchHideButtonText_ = nullptr;
  TextView *ghostShowButtonText_ = nullptr;
  TextView *ghostHideButtonText_ = nullptr;
  TextView *exportTouchShowButtonText_ = nullptr;
  TextView *exportTouchHideButtonText_ = nullptr;
  TextView *exportGhostShowButtonText_ = nullptr;
  TextView *exportGhostHideButtonText_ = nullptr;

  ReplayRecordsModalCallbacks callbacks_;
  ChartMetaRecord record_;
  std::vector<ResultRecordSummary> records_;
  std::vector<ResultRecordSummary> visibleRecords_;
  ReplayRecordFilters filters_;
  std::vector<replay_records_modal::ClearFilterButton> clearFilterButtons_;
  std::vector<replay_records_modal::OptionFilterButton>
      playOptionFilterButtons_;
  std::vector<replay_records_modal::ScoreRankFilterButton>
      scoreRankFilterButtons_;
  std::vector<replay_records_modal::SortButton> sortButtons_;
  int selectedIndex_ = -1;
  std::optional<ResultRecordSummary> selected_;
  std::optional<std::string> stableKey_;
  replay::ReplayFileDeleteConfirmation deleteConfirmation_;
  std::optional<ResultRecordSummary> exportSelection_;
  int selectedExportFps_ = 120;
  bool selectedExportFullResolution_ = true;
  bool selectedExportIncludeResultScreen_ = true;
  bool selectedReplayRenderTouchPoints_ = false;
  bool selectedReplayRenderGhosts_ = true;
  bool touchVisualizationEnabled_ = false;
  bool exportInProgress_ = false;
  bool resultRecallInProgress_ = false;
  bool irUploadInProgress_ = false;
  bool loadInProgress_ = false;
  bool documentHandoffActive_ = false;
  std::uint64_t titleResetAt_ = 0;
  bool titleResetPending_ = false;
};