#pragma once

#include "MusicSelectTypes.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

class MusicSelectRowProvider;

class MusicSelectDirectoryLoader final {
public:
  struct Content {
    std::vector<MusicSelectBar> children;
    std::shared_ptr<MusicSelectRowProvider> provider;
  };
  struct Result {
    MusicSelectBarId id;
    std::uint64_t generation = 0;
    Content content;
    std::string error;
  };
  using Processor = std::function<Content(std::stop_token)>;

  ~MusicSelectDirectoryLoader();
  std::uint64_t request(MusicSelectBarId, Processor);
  void cancel();
  [[nodiscard]] std::vector<Result> takeResults();

private:
  struct Request {
    MusicSelectBarId id;
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
  std::stop_source activeStop_;
  bool stopping_ = false;
  std::jthread worker_;
};
