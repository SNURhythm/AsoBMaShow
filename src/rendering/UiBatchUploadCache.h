#pragma once

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace rendering {

class UiBatchUploadCache {
public:
  static constexpr std::size_t kMaximumRetainedBytes = 4096;

  void invalidate() noexcept {
    valid_ = false;
    bytes_.clear();
  }

  template <typename Upload>
  bool uploadIfChanged(std::span<const std::byte> bytes, Upload &&upload) {
    if (valid_ && bytes.size() == bytes_.size() &&
        (bytes.empty() ||
         std::memcmp(bytes.data(), bytes_.data(), bytes.size()) == 0)) {
      return true;
    }
    if (!upload(bytes)) return false;
    invalidate();
    // Small UI batches benefit from exact comparisons; large geometry must
    // not leave a second large allocation beside its GPU buffer.
    if (bytes.size() <= kMaximumRetainedBytes) {
      try {
        bytes_.assign(bytes.begin(), bytes.end());
        valid_ = true;
      } catch (...) {
        // The upload succeeded. Allocation failure only disables reuse.
      }
    }
    return true;
  }

private:
  std::vector<std::byte> bytes_;
  bool valid_ = false;
};

} // namespace rendering
