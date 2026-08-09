#include "SkinConfigurationWriteQueue.h"

#include <type_traits>
#include <utility>

namespace skin {

static_assert(std::is_nothrow_move_constructible_v<SkinConfigurationWriteRequest>);
static_assert(std::is_nothrow_move_assignable_v<SkinConfigurationWriteRequest>);

SkinConfigurationEnqueueResult SkinConfigurationWriteQueue::enqueue(
    const SkinConfigurationWriteRequest &request) {
  auto owned = request;
  return enqueue(std::move(owned));
}

SkinConfigurationEnqueueResult
SkinConfigurationWriteQueue::enqueue(
    SkinConfigurationWriteRequest &&request) noexcept {
  const std::lock_guard lock(mutex_);
  if (closed_) {
    return SkinConfigurationEnqueueResult::Closed;
  }
  if (count_ == maxPending) {
    return SkinConfigurationEnqueueResult::QueueFull;
  }

  const std::size_t tail = (head_ + count_) % maxPending;
  pending_[tail] = std::move(request);
  ++count_;
  return SkinConfigurationEnqueueResult::Enqueued;
}

std::vector<SkinConfigurationWriteRequest> SkinConfigurationWriteQueue::drain() {
  const std::lock_guard lock(mutex_);
  std::vector<SkinConfigurationWriteRequest> drained;
  drained.reserve(count_);
  while (count_ != 0) {
    drained.emplace_back(std::move(pending_[head_]));
    head_ = (head_ + 1) % maxPending;
    --count_;
  }
  return drained;
}

void SkinConfigurationWriteQueue::close() noexcept {
  const std::lock_guard lock(mutex_);
  closed_ = true;
}

} // namespace skin
