#pragma once

#include "MusicSelectTypes.h"

#include <condition_variable>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

class MusicSelectFolderStatusLoader final {
public:
  using Processor = std::function<skin::MusicSelectBarFrame(
      const MusicSelectBar &, std::stop_token)>;
  using LegacyProcessor =
      std::function<skin::MusicSelectBarFrame(const MusicSelectBar &)>;
  struct Result {
    MusicSelectBarId id;
    skin::MusicSelectBarFrame frame;
    std::string error;
  };

  ~MusicSelectFolderStatusLoader();
  bool request(std::vector<MusicSelectBar>, std::string modeFilter,
               int longNoteMode, Processor);
  template <typename Process>
    requires std::is_invocable_r_v<skin::MusicSelectBarFrame, Process &,
                                    const MusicSelectBar &>
  bool request(std::vector<MusicSelectBar> bars, std::string modeFilter,
               int longNoteMode, Process process) {
    return request(std::move(bars), std::move(modeFilter), longNoteMode,
        [process = std::move(process)](const MusicSelectBar &bar,
                                       std::stop_token) mutable {
          return process(bar);
        });
  }
  void cancel();
  [[nodiscard]] bool retryReady();
  [[nodiscard]] std::vector<Result> takeResults();

private:
  struct Request {
    std::vector<MusicSelectBar> bars;
    Processor process;
    std::uint64_t generation = 0;
    std::stop_token stop;
  };

  void run(std::stop_token);
  std::mutex mutex_;
  std::condition_variable_any condition_;
  std::optional<Request> pending_;
  std::vector<Result> results_;
  std::uint64_t generation_ = 0;
  std::vector<MusicSelectBarId> rows_;
  std::string modeFilter_;
  int longNoteMode_ = -1;
  std::stop_source activeStop_;
  std::vector<MusicSelectBar> failedBars_;
  std::optional<std::chrono::steady_clock::time_point> retryAt_;
  std::jthread worker_;
};
