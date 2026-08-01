#include "ImageDecodeCoordinator.h"

#include <algorithm>
#include <utility>

namespace image_decode {

ImageDecodeCoordinator::ImageDecodeCoordinator(Loader loader,
                                               std::size_t workerCount)
    : loader_(std::move(loader)) {
  workerCount = std::max<std::size_t>(1, workerCount);
  configuredWorkerCount_ = workerCount;
  workers_.reserve(workerCount);
  for (std::size_t index = 0; index < workerCount; ++index) {
    workers_.emplace_back([this] { run(); });
  }
}

ImageDecodeCoordinator::~ImageDecodeCoordinator() { shutdown(); }

ImageDecodeCoordinator::Ticket
ImageDecodeCoordinator::request(ImageDecodeRequest request) {
  if (request.key.empty()) {
    return 0;
  }
  std::lock_guard lock(mutex_);
  if (stopping_) {
    return 0;
  }

  const Ticket ticket = nextTicket_++;
  auto existing = work_.find(request.key);
  if (existing != work_.end()) {
    if (request.priority && !existing->second.request.priority &&
        existing->second.state == WorkState::Queued) {
      existing->second.request.priority = true;
      std::erase(queue_, existing->first);
      priorityQueue_.push_back(existing->first);
      cv_.notify_one();
    }
    existing->second.consumers.insert(ticket);
    tickets_.emplace(ticket, existing->first);
    return ticket;
  }

  const std::string key = request.key;
  Work item{.id = nextWorkId_++, .request = std::move(request)};
  item.consumers.insert(ticket);
  const bool priority = item.request.priority;
  work_.emplace(key, std::move(item));
  tickets_.emplace(ticket, key);
  (priority ? priorityQueue_ : queue_).push_back(key);
  cv_.notify_one();
  return ticket;
}

void ImageDecodeCoordinator::cancel(Ticket ticket) {
  std::lock_guard lock(mutex_);
  removeTicketLocked(ticket);
}

std::optional<DecodedImageData>
ImageDecodeCoordinator::takeReady(Ticket ticket) {
  std::lock_guard lock(mutex_);
  const auto ticketEntry = tickets_.find(ticket);
  if (ticketEntry == tickets_.end()) {
    return std::nullopt;
  }
  const auto work = work_.find(ticketEntry->second);
  if (work == work_.end() || work->second.state != WorkState::Ready ||
      !work->second.image) {
    return std::nullopt;
  }
  DecodedImageData result = *work->second.image;
  work->second.consumers.erase(ticket);
  tickets_.erase(ticketEntry);
  if (work->second.consumers.empty()) {
    eraseWorkLocked(work);
  }
  return result;
}

bool ImageDecodeCoordinator::hasFailed(Ticket ticket) const {
  std::lock_guard lock(mutex_);
  const auto ticketEntry = tickets_.find(ticket);
  if (ticketEntry == tickets_.end()) {
    return false;
  }
  const auto work = work_.find(ticketEntry->second);
  return work != work_.end() && work->second.state == WorkState::Failed;
}

void ImageDecodeCoordinator::drop(std::string_view key) {
  std::lock_guard lock(mutex_);
  const auto work = work_.find(key);
  if (work != work_.end()) {
    eraseWorkLocked(work);
  }
}

void ImageDecodeCoordinator::dropPrefix(std::string_view prefix) {
  std::lock_guard lock(mutex_);
  std::vector<std::string> keys;
  for (const auto &[key, item] : work_) {
    (void)item;
    if (key.starts_with(prefix)) {
      keys.push_back(key);
    }
  }
  for (const auto &key : keys) {
    const auto work = work_.find(key);
    if (work != work_.end()) {
      eraseWorkLocked(work);
    }
  }
}

void ImageDecodeCoordinator::dropAll() {
  std::lock_guard lock(mutex_);
  priorityQueue_.clear();
  queue_.clear();
  work_.clear();
  tickets_.clear();
  readyBytes_ = 0;
}

void ImageDecodeCoordinator::shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (!stopping_) {
      stopping_ = true;
      priorityQueue_.clear();
      queue_.clear();
      work_.clear();
      tickets_.clear();
      readyBytes_ = 0;
    }
  }
  cv_.notify_all();
  for (auto &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
}

std::size_t
ImageDecodeCoordinator::pendingCount(std::string_view key) const {
  std::lock_guard lock(mutex_);
  const auto work = work_.find(key);
  return work != work_.end() &&
                 (work->second.state == WorkState::Queued ||
                  work->second.state == WorkState::InFlight)
             ? 1
             : 0;
}

std::size_t
ImageDecodeCoordinator::pendingCountPrefix(std::string_view prefix) const {
  std::lock_guard lock(mutex_);
  std::size_t count = 0;
  for (const auto &[key, item] : work_) {
    if (key.starts_with(prefix) &&
        (item.state == WorkState::Queued ||
         item.state == WorkState::InFlight)) {
      ++count;
    }
  }
  return count;
}

std::size_t ImageDecodeCoordinator::readyBytes() const {
  std::lock_guard lock(mutex_);
  return readyBytes_;
}

std::size_t ImageDecodeCoordinator::workerCount() const noexcept {
  return configuredWorkerCount_;
}

void ImageDecodeCoordinator::run() {
  for (;;) {
    ImageDecodeRequest request;
    std::uint64_t workId = 0;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] {
        return stopping_ || !priorityQueue_.empty() || !queue_.empty();
      });
      if (stopping_) {
        return;
      }

      bool found = false;
      while (!found && (!priorityQueue_.empty() || !queue_.empty())) {
        auto &active = !priorityQueue_.empty() ? priorityQueue_ : queue_;
        std::string key = std::move(active.front());
        active.pop_front();
        const auto work = work_.find(key);
        if (work == work_.end() || work->second.state != WorkState::Queued) {
          continue;
        }
        work->second.state = WorkState::InFlight;
        workId = work->second.id;
        request = work->second.request;
        found = true;
      }
      if (!found) {
        continue;
      }
    }

    std::optional<DecodedImageData> decoded;
    try {
      if (loader_) {
        decoded = loader_(request);
      }
    } catch (...) {
      decoded.reset();
    }

    std::lock_guard lock(mutex_);
    const auto work = work_.find(request.key);
    if (work == work_.end() || work->second.id != workId ||
        work->second.state != WorkState::InFlight) {
      continue;
    }
    if (work->second.consumers.empty()) {
      eraseWorkLocked(work);
      continue;
    }
    if (decoded && decoded->valid()) {
      work->second.image = std::move(decoded);
      work->second.state = WorkState::Ready;
      readyBytes_ += work->second.image->byteSize();
    } else {
      work->second.state = WorkState::Failed;
    }
  }
}

void ImageDecodeCoordinator::removeTicketLocked(Ticket ticket) {
  const auto ticketEntry = tickets_.find(ticket);
  if (ticketEntry == tickets_.end()) {
    return;
  }
  const auto work = work_.find(ticketEntry->second);
  if (work == work_.end()) {
    tickets_.erase(ticketEntry);
    return;
  }
  work->second.consumers.erase(ticket);
  tickets_.erase(ticketEntry);
  if (work->second.consumers.empty()) {
    eraseWorkLocked(work);
  }
}

void ImageDecodeCoordinator::eraseWorkLocked(
    std::map<std::string, Work, std::less<>>::iterator work) {
  for (const Ticket ticket : work->second.consumers) {
    tickets_.erase(ticket);
  }
  if (work->second.state == WorkState::Ready && work->second.image) {
    readyBytes_ -= work->second.image->byteSize();
  }
  std::erase(priorityQueue_, work->first);
  std::erase(queue_, work->first);
  work_.erase(work);
}

} // namespace image_decode
