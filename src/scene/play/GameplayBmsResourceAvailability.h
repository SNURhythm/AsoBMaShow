#pragma once

#include "../../bms_parser.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <thread>
#include <utility>

namespace gameplay {

class BmsResourceImageAvailabilityProbe {
public:
  using Decode =
      std::function<bool(const std::filesystem::path &resourcePath,
                         std::stop_token stop)>;

  BmsResourceImageAvailabilityProbe() = default;
  BmsResourceImageAvailabilityProbe(
      const BmsResourceImageAvailabilityProbe &) = delete;
  BmsResourceImageAvailabilityProbe &
  operator=(const BmsResourceImageAvailabilityProbe &) = delete;

  void start(const bms_parser::ChartMeta &meta,
             const std::filesystem::path &declaredPath,
             Decode decode) noexcept {
    available_.store(false, std::memory_order_release);
    complete_.store(false, std::memory_order_release);
    if (declaredPath.empty() || meta.BmsPath.empty()) {
      complete_.store(true, std::memory_order_release);
      return;
    }
    const auto resourcePath = meta.BmsPath.parent_path() / declaredPath;
    try {
      worker_ = std::jthread(
          [this, resourcePath, decode = std::move(decode)](
              std::stop_token stop) mutable {
            bool available = false;
            if (!stop.stop_requested()) {
              try {
                available = decode(resourcePath, stop);
              } catch (...) {
                available = false;
              }
            }
            if (stop.stop_requested()) {
              return;
            }
            available_.store(available, std::memory_order_release);
            complete_.store(true, std::memory_order_release);
          });
    } catch (...) {
      complete_.store(true, std::memory_order_release);
    }
  }

  [[nodiscard]] bool available() const noexcept {
    return available_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool complete() const noexcept {
    return complete_.load(std::memory_order_acquire);
  }

private:
  std::atomic_bool available_{false};
  std::atomic_bool complete_{false};
  std::jthread worker_;
};

} // namespace gameplay
