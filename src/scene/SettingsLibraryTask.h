#pragma once

#include "../ThreadCompat.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>

// Owns the exclusive Settings table/folder worker and its data-only updates.
// Admission, consumption, shutdown, and destruction belong to the application
// thread. Work dependencies must outlive this owner; work handles its failures.
class SettingsLibraryTask final {
public:
  struct Status {
    std::string text;
    bool succeeded = false;
  };
  struct ImportProgress {
    int current = 0;
    int total = 0;
    std::string tableName;
    std::string statusText;
    bool finished = false;
    bool succeeded = false;
    std::string submittedUrl;
  };
  struct Updates {
    std::optional<Status> tableStatus;
    std::optional<Status> folderStatus;
    std::optional<ImportProgress> importProgress;
    bool reload = false;
  };

  // Valid only during Work. Synchronous importer progress callbacks may borrow
  // it, but must not retain it or publish from another asynchronous operation.
  class Publisher final {
  public:
    Publisher(const Publisher &) = delete;
    Publisher &operator=(const Publisher &) = delete;
    void tableStatus(std::string text, bool succeeded, bool reload = false) const;
    void folderStatus(std::string text, bool succeeded, bool reload = false) const;
    void importProgress(ImportProgress progress) const;

  private:
    friend class SettingsLibraryTask;
    Publisher(SettingsLibraryTask &owner, std::stop_token token)
        : owner_(owner), token_(std::move(token)) {}
    SettingsLibraryTask &owner_;
    std::stop_token token_;
  };
  using Work = std::function<void(const std::stop_token &, const Publisher &)>;

  SettingsLibraryTask() = default;
  ~SettingsLibraryTask();
  SettingsLibraryTask(const SettingsLibraryTask &) = delete;
  SettingsLibraryTask &operator=(const SettingsLibraryTask &) = delete;

  bool start(Work work);
  bool running() const;
  // Latest update per channel, with reload accumulated until consumption.
  // Admission retains completed updates, matching the Settings UI handoff.
  Updates takeUpdates();
  // Suppresses late updates, joins even uncancellable operations, and discards
  // queued updates. Repeated shutdown and subsequent scene reuse are supported.
  void stopAndWait();

private:
  mutable std::mutex mutex_;
  bool running_ = false;
  Updates pending_;
  std::jthread worker_;
};
