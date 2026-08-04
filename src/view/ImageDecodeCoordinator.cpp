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

ImageDecodeCoordinator::ImageDecodeCoordinator(LegacyLoader loader,
                                               std::size_t workerCount)
    : ImageDecodeCoordinator(
          [loader = std::move(loader)](const ImageDecodeRequest &request,
                                       std::stop_token) {
            return loader ? loader(request) : std::nullopt;
          },
          workerCount) {}

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
  {
    std::lock_guard lock(mutex_);
    removeTicketLocked(ticket);
  }
  cv_.notify_all();
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

ImageDecodeWaitResult ImageDecodeCoordinator::waitTake(Ticket ticket,
                                                        std::stop_token stop) {
  if (ticket == 0) {
    return {.state = ImageDecodeWaitState::Stopped};
  }
  {
    std::lock_guard lock(mutex_);
    waitingTickets_.insert(ticket);
  }
  std::stop_callback stopped(stop, [this, ticket] { cancel(ticket); });
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this, ticket] {
    if (terminalTickets_.contains(ticket)) {
      return true;
    }
    const auto ticketEntry = tickets_.find(ticket);
    if (ticketEntry == tickets_.end()) {
      return true;
    }
    const auto work = work_.find(ticketEntry->second);
    return work == work_.end() || work->second.state == WorkState::Ready ||
           work->second.state == WorkState::Failed;
  });
  if (const auto terminal = terminalTickets_.find(ticket);
      terminal != terminalTickets_.end()) {
    const ImageDecodeWaitState state = terminal->second;
    terminalTickets_.erase(terminal);
    waitingTickets_.erase(ticket);
    return {.state = state};
  }
  const auto ticketEntry = tickets_.find(ticket);
  if (ticketEntry == tickets_.end()) {
    waitingTickets_.erase(ticket);
    return {.state = stopping_ ? ImageDecodeWaitState::Stopped
                               : ImageDecodeWaitState::Cancelled};
  }
  const auto work = work_.find(ticketEntry->second);
  if (work == work_.end()) {
    tickets_.erase(ticketEntry);
    waitingTickets_.erase(ticket);
    return {.state = stopping_ ? ImageDecodeWaitState::Stopped
                               : ImageDecodeWaitState::Cancelled};
  }
  if (work->second.state == WorkState::Ready && work->second.image) {
    ImageDecodeWaitResult result{.state = ImageDecodeWaitState::Ready,
                                 .image = *work->second.image};
    work->second.consumers.erase(ticket);
    tickets_.erase(ticketEntry);
    if (work->second.consumers.empty()) {
      eraseWorkLocked(work);
    }
    waitingTickets_.erase(ticket);
    return result;
  }
  work->second.consumers.erase(ticket);
  tickets_.erase(ticketEntry);
  if (work->second.consumers.empty()) {
    eraseWorkLocked(work);
  }
  waitingTickets_.erase(ticket);
  return {.state = ImageDecodeWaitState::Failed};
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

bool ImageDecodeCoordinator::isTracked(Ticket ticket) const {
  std::lock_guard lock(mutex_);
  return tickets_.contains(ticket);
}

void ImageDecodeCoordinator::drop(std::string_view key) {
  {
    std::lock_guard lock(mutex_);
    const auto work = work_.find(key);
    if (work != work_.end()) {
      eraseWorkLocked(work);
    }
  }
  cv_.notify_all();
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
  cv_.notify_all();
}

void ImageDecodeCoordinator::dropAll() {
  std::lock_guard lock(mutex_);
  while (!work_.empty()) {
    eraseWorkLocked(work_.begin());
  }
  priorityQueue_.clear();
  queue_.clear();
  readyBytes_ = 0;
  cv_.notify_all();
}

void ImageDecodeCoordinator::shutdown() {
  std::lock_guard shutdownLock(shutdownMutex_);
  {
    std::lock_guard lock(mutex_);
    if (!stopping_) {
      stopping_ = true;
      priorityQueue_.clear();
      queue_.clear();
      for (auto &[key, work] : work_) {
        (void)key;
        work.stop.request_stop();
        for (const Ticket ticket : work.consumers)
          if (waitingTickets_.contains(ticket))
            terminalTickets_[ticket] = ImageDecodeWaitState::Stopped;
      }
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
        std::stop_token stop;
        {
          std::lock_guard lock(mutex_);
          const auto found = work_.find(request.key);
          if (found != work_.end() && found->second.id == workId) {
            stop = found->second.stop.get_token();
          }
        }
        decoded = loader_(request, stop);
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
      // The result retains RGBA only. Drop source bytes before notifying
      // consumers so a ready work item never keeps both allocations.
      work->second.request.encoded.reset();
      work->second.image = std::move(decoded);
      work->second.state = WorkState::Ready;
      readyBytes_ += work->second.image->byteSize();
    } else {
      work->second.request.encoded.reset();
      work->second.state = WorkState::Failed;
    }
    cv_.notify_all();
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
    if (waitingTickets_.contains(ticket))
      terminalTickets_[ticket] = ImageDecodeWaitState::Cancelled;
    return;
  }
  work->second.consumers.erase(ticket);
  tickets_.erase(ticketEntry);
  if (waitingTickets_.contains(ticket))
    terminalTickets_[ticket] = ImageDecodeWaitState::Cancelled;
  if (work->second.consumers.empty()) {
    eraseWorkLocked(work);
  }
}

void ImageDecodeCoordinator::eraseWorkLocked(
    std::map<std::string, Work, std::less<>>::iterator work) {
  work->second.stop.request_stop();
  for (const Ticket ticket : work->second.consumers) {
    tickets_.erase(ticket);
    if (waitingTickets_.contains(ticket))
      terminalTickets_[ticket] = stopping_ ? ImageDecodeWaitState::Stopped
                                           : ImageDecodeWaitState::Cancelled;
  }
  if (work->second.state == WorkState::Ready && work->second.image) {
    readyBytes_ -= work->second.image->byteSize();
  }
  std::erase(priorityQueue_, work->first);
  std::erase(queue_, work->first);
  work_.erase(work);
}

} // namespace image_decode
