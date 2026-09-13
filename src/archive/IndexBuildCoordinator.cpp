#include "IndexBuildCoordinator.h"

#include <cassert>
#include <chrono>
#include <utility>

namespace archive_file {

struct IndexBuildCoordinator::BuildState {
  enum class Status { Building, Succeeded, Failed };
  explicit BuildState(std::string archiveKey) : key(std::move(archiveKey)) {}
  const std::string key;
  Status status = Status::Building;
};

IndexBuildCoordinator::Lease::Lease(IndexBuildCoordinator &owner,
                                    std::shared_ptr<BuildState> state,
                                    bool builder)
    : owner_(&owner), state_(std::move(state)), builder_(builder) {}

IndexBuildCoordinator::Lease::~Lease() { complete(false); }

IndexBuildCoordinator::Lease::Lease(Lease &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      state_(std::move(other.state_)), builder_(std::exchange(other.builder_, false)) {}

IndexBuildCoordinator::Lease &
IndexBuildCoordinator::Lease::operator=(Lease &&other) noexcept {
  if (this != &other) {
    complete(false);
    owner_ = std::exchange(other.owner_, nullptr);
    state_ = std::move(other.state_);
    builder_ = std::exchange(other.builder_, false);
  }
  return *this;
}

void IndexBuildCoordinator::Lease::complete(bool success) {
  if (builder_) {
    owner_->complete(state_, success);
    builder_ = false;
  }
}

IndexBuildCoordinator::WaitOutcome
IndexBuildCoordinator::Lease::wait(const Checkpoint &checkpoint) {
  assert(owner_ != nullptr && state_ != nullptr && !builder_);
  std::unique_lock lock(owner_->mutex_);
  do {
    owner_->changed_.wait_for(lock, std::chrono::milliseconds(20), [&] {
      return state_->status != BuildState::Status::Building;
    });
    lock.unlock();
    const bool keepGoing = !checkpoint || checkpoint();
    lock.lock();
    if (!keepGoing) {
      return WaitOutcome::Cancelled;
    }
  } while (state_->status == BuildState::Status::Building);
  return state_->status == BuildState::Status::Succeeded
             ? WaitOutcome::Succeeded : WaitOutcome::Failed;
}

IndexBuildCoordinator::Lease
IndexBuildCoordinator::acquire(const std::string &key) {
  std::lock_guard lock(mutex_);
  const auto found = active_.find(key);
  if (found != active_.end()) {
    return Lease(*this, found->second, false);
  }
  auto state = std::make_shared<BuildState>(key);
  active_.emplace(key, state);
  return Lease(*this, std::move(state), true);
}

void IndexBuildCoordinator::complete(const std::shared_ptr<BuildState> &state,
                                      bool success) {
  {
    std::lock_guard lock(mutex_);
    state->status = success ? BuildState::Status::Succeeded
                            : BuildState::Status::Failed;
    const auto found = active_.find(state->key);
    if (found != active_.end() && found->second == state) {
      active_.erase(found);
    }
  }
  changed_.notify_all();
}

} // namespace archive_file
