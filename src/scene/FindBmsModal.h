#pragma once

#include "FindBmsTask.h"
#include "../repositories/ChartRepository.h"

#include <SDL2/SDL.h>
#include <functional>
#include <memory>

class Button;
class TextView;
class View;
template <typename T> class RecyclerView;

std::filesystem::path findBmsDownloadRoot(ChartRepository::Session *session);

struct FindBmsModalCallbacks {
  std::function<std::filesystem::path()> downloadRoot;
  std::function<BmsSearchDownloadOptions()> downloadOptions;
  std::function<void()> downloadStarted;
  std::function<void(const ChartMetaRecord &, const BmsSearchResult &,
                     bool matched)> filesReady;
  std::function<void()> refreshLibrary;
};

// The parent owns the views. Destroy this controller before its parent view.
class FindBmsModal final {
public:
  static std::unique_ptr<FindBmsModal>
  Create(View *parent, FindBmsModalCallbacks callbacks);
  ~FindBmsModal();
  FindBmsModal(const FindBmsModal &) = delete;
  FindBmsModal &operator=(const FindBmsModal &) = delete;

  void show(const ChartMetaRecord &record);
  void update();
  void resize(int width, int height);
  void refresh(bool refreshCandidates = true);
  void cancelAndWait();
  bool inProgress() const;
  bool isVisible() const;
  View *root() const;
  bool handleEvents(SDL_Event &event);
  void hide();

private:
  explicit FindBmsModal(FindBmsModalCallbacks callbacks);
  void build(View *parent);
  void startCandidateDownload(size_t candidateIndex);
  void startPendingArtifactResolution(BmsSearchPendingArtifactDecision decision);
  void cancelOrClose();
  void openResultUrl(const std::string &url);

  FindBmsTask findBmsTask;
  FindBmsModalCallbacks callbacks_;
  View *findBmsModalRoot = nullptr;
  View *findBmsProgressTrack = nullptr;
  View *findBmsProgressFill = nullptr;
  TextView *findBmsModalTitleText = nullptr;
  TextView *findBmsStatusText = nullptr;
  TextView *findBmsDetailText = nullptr;
  Button *findBmsCloseButton = nullptr;
  Button *findBmsRetryButton = nullptr;
  Button *findBmsKeepFilesButton = nullptr;
  Button *findBmsDeleteFilesButton = nullptr;
  Button *findBmsOpenButton = nullptr;
  Button *findBmsGoogleButton = nullptr;
  Button *findBmsRefreshButton = nullptr;
  RecyclerView<BmsSearchCandidate> *findBmsCandidateRecyclerView = nullptr;
  TextView *findBmsCloseButtonText = nullptr;
  TextView *findBmsRetryButtonText = nullptr;
  TextView *findBmsKeepFilesButtonText = nullptr;
  TextView *findBmsDeleteFilesButtonText = nullptr;
  TextView *findBmsOpenButtonText = nullptr;
  TextView *findBmsGoogleButtonText = nullptr;
  TextView *findBmsRefreshButtonText = nullptr;
  ChartMetaRecord findBmsModalChart;
  BmsSearchResult findBmsResult;
  std::optional<BmsSearchPendingArtifactDecision> findBmsPendingDecision;
  std::string findBmsProgressMessage;
  std::uint64_t findBmsProgressCurrent = 0;
  std::uint64_t findBmsProgressTotal = 0;
  double findBmsProgressFraction = 0.0;
  std::deque<std::string> findBmsProgressLog;
};
