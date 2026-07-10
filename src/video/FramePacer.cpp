#include "FramePacer.h"

#include <algorithm>

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

void FramePacer::reset(std::chrono::steady_clock::time_point now) {
  nextDeadline = now;
  deadlineInitialized = false;
}

std::chrono::steady_clock::duration
FramePacer::remaining(std::chrono::steady_clock::time_point now) const {
  if (cap == 0 || !deadlineInitialized || now >= nextDeadline) {
    return std::chrono::steady_clock::duration::zero();
  }
  return nextDeadline - now;
}

void FramePacer::framePresented(std::chrono::steady_clock::time_point now) {
  if (cap == 0) {
    deadlineInitialized = false;
    return;
  }
  if (!deadlineInitialized) {
    nextDeadline = now + framePeriod;
    deadlineInitialized = true;
    return;
  }
  do {
    nextDeadline += framePeriod;
  } while (nextDeadline <= now);
}
