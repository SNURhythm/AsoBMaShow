#include "FramePacer.h"

#include <algorithm>
#include <limits>

namespace {
using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;
using TimePoint = Clock::time_point;
using Rep = Duration::rep;

static_assert(std::numeric_limits<Rep>::is_integer &&
              std::numeric_limits<Rep>::is_signed);

TimePoint saturatingAdd(TimePoint value, Duration amount) {
  const Rep valueCount = value.time_since_epoch().count();
  const Rep amountCount = amount.count();
  const Rep maximum = TimePoint::max().time_since_epoch().count();
  if (amountCount > 0 && valueCount > maximum - amountCount) {
    return TimePoint::max();
  }
  return TimePoint{Duration{valueCount + amountCount}};
}

Duration positiveDifference(TimePoint later, TimePoint earlier) {
  if (later <= earlier) {
    return Duration::zero();
  }
  const Rep laterCount = later.time_since_epoch().count();
  const Rep earlierCount = earlier.time_since_epoch().count();
  const Rep maximum = Duration::max().count();
  if (earlierCount < 0 && laterCount > maximum + earlierCount) {
    return Duration::max();
  }
  return Duration{laterCount - earlierCount};
}
} // namespace

void FramePacer::setCap(std::uint32_t fps) {
  if (cap == fps) {
    return;
  }
  cap = fps;
  deadlineInitialized = false;
  if (cap == 0) {
    framePeriod = std::chrono::steady_clock::duration::zero();
    return;
  }
  framePeriod = std::max(
      std::chrono::steady_clock::duration{1},
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(1.0 / static_cast<double>(cap))));
}

std::uint32_t FramePacer::currentFrameCap() const { return cap; }

bool FramePacer::applyFrameCap(std::uint32_t fps,
                               std::string & /*errorMessage*/) {
  setCap(fps);
  return true;
}

void FramePacer::reset(std::chrono::steady_clock::time_point now) {
  nextDeadline = now;
  deadlineInitialized = false;
}

std::chrono::steady_clock::duration
FramePacer::remaining(std::chrono::steady_clock::time_point now) const {
  if (cap == 0 || !deadlineInitialized || now >= nextDeadline) {
    return std::chrono::steady_clock::duration::zero();
  }
  return positiveDifference(nextDeadline, now);
}

void FramePacer::framePresented(std::chrono::steady_clock::time_point now) {
  if (cap == 0) {
    deadlineInitialized = false;
    return;
  }
  if (!deadlineInitialized) {
    nextDeadline = saturatingAdd(now, framePeriod);
    deadlineInitialized = true;
    return;
  }

  if (nextDeadline > now) {
    nextDeadline = saturatingAdd(nextDeadline, framePeriod);
    return;
  }

  const auto lateness = positiveDifference(now, nextDeadline);
  const Rep periodCount = framePeriod.count();
  const Rep quotient = lateness.count() / periodCount;
  if (quotient == std::numeric_limits<Rep>::max() ||
      quotient + 1 > std::numeric_limits<Rep>::max() / periodCount) {
    nextDeadline = TimePoint::max();
    return;
  }
  nextDeadline =
      saturatingAdd(nextDeadline, Duration{(quotient + 1) * periodCount});
}
