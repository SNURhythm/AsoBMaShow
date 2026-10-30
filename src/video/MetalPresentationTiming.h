#pragma once

#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace video {
// Diagnostic samples never retain a drawable, texture, command buffer or layer.
struct MetalPresentationSample {
  double callback = 0, deadline = 0, target = 0;
  bool accepted = false;
  std::atomic<double> request{0}, scheduledObserved{0}, presented{0};
  std::atomic<bool> presentationObserved{false};
};

class MetalPresentationTiming {
public:
  std::uint64_t serial = 0, generation = 0;
  double attachedAt = 0, detachedAt = 0;
  const char *detachReason = "none";
  float rate = 0;
  unsigned width = 0, height = 0;

  // Owner-thread only. After the first 120 callbacks, sample at most once a
  // second into a 16-entry ring. Async Metal handlers hold only weak samples.
  std::shared_ptr<MetalPresentationSample> record(double now, double deadline,
                                                double target, bool accepted) {
    ++callbacks_;
    if (!accepted) ++rejected_;
    if (!std::isfinite(now) || now <= 0) return {};
    if (initialCount_ == initial_.size() && now - lastSampleAt_ < 1.0) return {};
    auto sample = std::make_shared<MetalPresentationSample>();
    sample->callback = now;
    sample->deadline = deadline;
    sample->target = target;
    sample->accepted = accepted;
    if (initialCount_ < initial_.size()) initial_[initialCount_++] = sample;
    else {
      recent_[nextRecent_] = sample;
      nextRecent_ = (nextRecent_ + 1) % recent_.size();
      recentCount_ = std::min(recentCount_ + 1, recent_.size());
    }
    lastSampleAt_ = now;
    return sample;
  }
  std::vector<std::shared_ptr<MetalPresentationSample>> samples() const {
    std::vector<std::shared_ptr<MetalPresentationSample>> result;
    result.reserve(initialCount_ + recentCount_);
    result.insert(result.end(), initial_.begin(), initial_.begin() + initialCount_);
    const auto first = recentCount_ == recent_.size() ? nextRecent_ : 0;
    for (std::size_t i = 0; i < recentCount_; ++i)
      result.push_back(recent_[(first + i) % recent_.size()]);
    return result;
  }
  std::string report() const {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "METAL_TIMING serial=" << serial << " generation=" << generation
        << " attachedAt=" << attachedAt << " detachedAt=" << detachedAt
        << " detachReason=" << detachReason << " rateAtAttach=" << rate
        << " drawableAtAttach=" << width << 'x' << height
        << " callbacks=" << callbacks_ << " rejected=" << rejected_ << '\n';
    const auto retained = samples();
    const auto append = [&](const char *name, std::size_t first, std::size_t end) {
      unsigned requested = 0, requestLate = 0, displayed = 0, displayLate = 0;
      unsigned unavailable = 0, pending = 0, accepted = 0;
      std::array<std::vector<double>, 8> values;
      const auto difference = [&](unsigned metric, double later, double earlier) {
        if (std::isfinite(later) && std::isfinite(earlier) && later > 0 && earlier > 0)
          values[metric].push_back((later - earlier) * 1000);
      };
      for (std::size_t i = first; i < end; ++i) {
        const auto &sample = *retained[i];
        accepted += sample.accepted;
        difference(0, sample.target, sample.callback);
        difference(1, sample.deadline, sample.callback);
        difference(2, sample.target, sample.deadline);
        const double request = sample.request.load(std::memory_order_relaxed);
        if (!(request > 0)) continue; // Idle/rejected callbacks were never submitted.
        ++requested;
        requestLate += std::isfinite(sample.deadline) && request > sample.deadline;
        difference(3, request, sample.callback);
        difference(4, sample.deadline, request);
        difference(5, sample.deadline, sample.scheduledObserved.load(std::memory_order_relaxed));
        const bool observed = sample.presentationObserved.load(std::memory_order_acquire);
        const double presented = sample.presented.load(std::memory_order_relaxed);
        if (!observed) ++pending;
        else if (!std::isfinite(presented) || presented <= 0) ++unavailable;
        else {
          ++displayed;
          displayLate += std::isfinite(sample.target) && presented > sample.target + .001;
          difference(6, presented, sample.target);
          difference(7, presented, request);
        }
      }
      out << " " << name << " samples=" << end - first << " accepted=" << accepted
          << " requested=" << requested << " requestLate=" << requestLate
          << " displayed=" << displayed << " displayLateGt1ms=" << displayLate
          << " unavailable=" << unavailable << " pending=" << pending << '\n';
      const char *names[] = {"targetLeadMs", "deadlineLeadMs", "targetAfterDeadlineMs",
          "callbackToRequestMs", "requestMarginMs", "scheduledObservedMarginMs",
          "displayDeltaMs", "requestToDisplayMs"};
      for (unsigned i = 0; i < values.size(); ++i) {
        auto &v = values[i];
        if (v.empty()) continue;
        std::sort(v.begin(), v.end());
        const auto percentile = [&](double fraction) {
          return v[static_cast<std::size_t>(std::ceil(fraction * v.size())) - 1];
        };
        out << "  " << names[i] << "[n=" << v.size() << " min=" << v.front()
            << " p50=" << percentile(.5) << " p95=" << percentile(.95)
            << " max=" << v.back() << "]\n";
      }
    };
    append("startup", 0, initialCount_);
    if (recentCount_) append("recent", initialCount_, retained.size());
    // Preserve the first callbacks and latest samples without dumping a log.
    for (std::size_t i = 0; i < retained.size(); ++i) {
      if (i >= 3 && i + 3 < retained.size()) continue;
      const auto &sample = *retained[i];
      out << "  sample at=" << sample.callback << " accepted=" << sample.accepted
          << " deadline=" << sample.deadline << " target=" << sample.target
          << " request=" << sample.request.load(std::memory_order_relaxed)
          << " scheduledObserved=" << sample.scheduledObserved.load(std::memory_order_relaxed)
          << " presented=" << sample.presented.load(std::memory_order_relaxed) << '\n';
    }
    return out.str();
  }

private:
  std::array<std::shared_ptr<MetalPresentationSample>, 120> initial_;
  std::array<std::shared_ptr<MetalPresentationSample>, 16> recent_;
  std::size_t initialCount_ = 0, recentCount_ = 0, nextRecent_ = 0;
  std::uint64_t callbacks_ = 0, rejected_ = 0;
  double lastSampleAt_ = 0;
};
} // namespace video
