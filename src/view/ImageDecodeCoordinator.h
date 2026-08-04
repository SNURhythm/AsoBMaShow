#pragma once

#include "DecodedImageCache.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <stop_token>
#include <vector>

namespace image_decode {

enum class ImageDecodeWaitState { Ready, Failed, Cancelled, Stopped };

struct ImageDecodeWaitResult {
  ImageDecodeWaitState state = ImageDecodeWaitState::Failed;
  std::optional<DecodedImageData> image;
};

struct ImageDecodeRequest {
  std::string key;
  std::filesystem::path path;
  int targetWidth = 0;
  int targetHeight = 0;
  bool priority = false;
};

class ImageDecodeCoordinator {
public:
  using Ticket = std::uint64_t;
  using Loader =
      std::function<std::optional<DecodedImageData>(const ImageDecodeRequest &,
                                                    std::stop_token)>;
  using LegacyLoader =
      std::function<std::optional<DecodedImageData>(const ImageDecodeRequest &)>;

  explicit ImageDecodeCoordinator(Loader loader, std::size_t workerCount = 2);
  explicit ImageDecodeCoordinator(LegacyLoader loader,
                                  std::size_t workerCount = 2);
  ~ImageDecodeCoordinator();
  ImageDecodeCoordinator(const ImageDecodeCoordinator &) = delete;
  ImageDecodeCoordinator &operator=(const ImageDecodeCoordinator &) = delete;

  [[nodiscard]] Ticket request(ImageDecodeRequest request);
  void cancel(Ticket ticket);
  [[nodiscard]] std::optional<DecodedImageData> takeReady(Ticket ticket);
  [[nodiscard]] ImageDecodeWaitResult waitTake(Ticket ticket,
                                                std::stop_token stop = {});
  [[nodiscard]] bool hasFailed(Ticket ticket) const;
  [[nodiscard]] bool isTracked(Ticket ticket) const;
  void drop(std::string_view key);
  void dropPrefix(std::string_view prefix);
  void dropAll();
  void shutdown();

  [[nodiscard]] std::size_t pendingCount(std::string_view key) const;
  [[nodiscard]] std::size_t pendingCountPrefix(std::string_view prefix) const;
  [[nodiscard]] std::size_t readyBytes() const;
  [[nodiscard]] std::size_t workerCount() const noexcept;

private:
  enum class WorkState { Queued, InFlight, Ready, Failed };
  struct Work {
    std::uint64_t id = 0;
    ImageDecodeRequest request;
    WorkState state = WorkState::Queued;
    std::set<Ticket> consumers;
    std::optional<DecodedImageData> image;
    std::stop_source stop;
  };

  void run();
  void removeTicketLocked(Ticket ticket);
  void eraseWorkLocked(std::map<std::string, Work, std::less<>>::iterator work);

  Loader loader_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::mutex shutdownMutex_;
  std::deque<std::string> priorityQueue_;
  std::deque<std::string> queue_;
  std::map<std::string, Work, std::less<>> work_;
  std::map<Ticket, std::string> tickets_;
  std::map<Ticket, ImageDecodeWaitState> terminalTickets_;
  std::vector<std::thread> workers_;
  std::uint64_t nextTicket_ = 1;
  std::uint64_t nextWorkId_ = 1;
  std::size_t configuredWorkerCount_ = 0;
  std::size_t readyBytes_ = 0;
  bool stopping_ = false;
};

} // namespace image_decode
