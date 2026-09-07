#pragma once

#include "MusicSelectTypes.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

class MusicSelectFolderStatusLoader final {
public:
  using Processor = std::function<skin::MusicSelectBarFrame(const MusicSelectBar &)>;
  struct Result {
    MusicSelectBarId id;
    skin::MusicSelectBarFrame frame;
    std::string error;
  };

  ~MusicSelectFolderStatusLoader();
  bool request(std::vector<MusicSelectBar>, std::string modeFilter,
               int longNoteMode, Processor);
  void cancel();
  [[nodiscard]] std::vector<Result> takeResults();

private:
  struct Request {
    std::vector<MusicSelectBar> bars;
    Processor process;
    std::uint64_t generation = 0;
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
  std::jthread worker_;
};
