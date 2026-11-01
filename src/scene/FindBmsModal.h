#pragma once

#include "FindBmsTask.h"
#include "../downloads/DownloadJobs.h"
#include <chrono>
#include "../repositories/ChartRepository.h"

#include <SDL3/SDL.h>
#include <functional>
#include <memory>

class Button;
class TextView;
class ScrollView;
class View;

std::filesystem::path findBmsDownloadRoot(ChartRepository::Session *session);

struct FindBmsModalCallbacks {
  std::function<bool()> admit;
  std::function<std::filesystem::path()> downloadRoot;
  std::function<BmsSearchDownloadOptions()> downloadOptions;
  std::function<void()> downloadStarted;
  std::function<void(const ChartMetaRecord &, const BmsSearchResult &,
                     bool matched)> filesReady;
  std::function<void()> refreshLibrary;
  std::function<void(const std::string &)> openDownloads;
  std::function<std::optional<downloads::Job>(const std::string &)> readDownload;
  std::function<bool(const std::string &)> cancelDownload, retryDownload;
  std::function<bool(const std::string &, BmsSearchPendingArtifactDecision)> decideDownload;
};

// The parent owns the views. Destroy this controller before its parent view.
class FindBmsModal final {
public:
  static std::unique_ptr<FindBmsModal>
  Create(View *parent, FindBmsModalCallbacks callbacks);
  ~FindBmsModal();
  FindBmsModal(const FindBmsModal &) = delete;
  FindBmsModal &operator=(const FindBmsModal &) = delete;

  void show(const ChartMetaRecord &record, bool requireConfirmation = false);
  void update();
  void resize(int width, int height);
  void refresh();
  void cancelAndWait();
  bool inProgress() const;
  bool isVisible() const;
  View *root() const;
  bool handleEvents(SDL_Event &event);
  void hide();

private:
  explicit FindBmsModal(FindBmsModalCallbacks callbacks);
  void build(View *parent);
  bool updateQueuedDownload(bool force = false);
  void startLookup();
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
  ScrollView *findBmsDetailScroll = nullptr;
  TextView *findBmsDetailText = nullptr;
  Button *findBmsCloseButton = nullptr;
  Button *findBmsBackgroundButton = nullptr;
  Button *findBmsConfirmButton = nullptr;
  Button *findBmsRetryButton = nullptr;
  Button *findBmsKeepFilesButton = nullptr;
  Button *findBmsDeleteFilesButton = nullptr;
  Button *findBmsOpenButton = nullptr;
  Button *findBmsGoogleButton = nullptr;
  Button *findBmsRefreshButton = nullptr;
  TextView *findBmsCloseButtonText = nullptr;
  TextView *findBmsBackgroundButtonText = nullptr;
  TextView *findBmsConfirmButtonText = nullptr;
  TextView *findBmsRetryButtonText = nullptr;
  TextView *findBmsKeepFilesButtonText = nullptr;
  TextView *findBmsDeleteFilesButtonText = nullptr;
  TextView *findBmsOpenButtonText = nullptr;
  TextView *findBmsGoogleButtonText = nullptr;
  TextView *findBmsRefreshButtonText = nullptr;
  bool queuedDownloadRunning_ = false, queuedDownloadRetry_ = false;
  std::chrono::steady_clock::time_point queuedDownloadUpdateAt_{};
  std::string queuedDownloadSnapshot_;
  bool findBmsAwaitingConfirmation = false;
  ChartMetaRecord findBmsModalChart;
  BmsSearchResult findBmsResult;
  BmsSearchDownloadOptions findBmsCapturedOptions;
  std::optional<storage_management::StorageProgress> findBmsStorageProgress;
  std::optional<BmsSearchPendingArtifactDecision> findBmsPendingDecision;
  std::string findBmsProgressMessage;
  std::uint64_t findBmsProgressCurrent = 0;
  std::uint64_t findBmsProgressTotal = 0;
  double findBmsProgressFraction = 0.0;
  std::deque<std::string> findBmsProgressLog;
};
