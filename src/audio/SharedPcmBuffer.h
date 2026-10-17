#pragma once

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace audio {

// Samples become immutable on publication. Copies share storage; replacing a
// buffer leaves every other owner intact. Callback reads do not copy ownership.
class SharedPcmBuffer {
public:
  SharedPcmBuffer() = default;
  SharedPcmBuffer(std::vector<short> samples) {
    if (!samples.empty())
      samples_ = std::make_shared<const std::vector<short>>(std::move(samples));
  }
  SharedPcmBuffer(std::initializer_list<short> samples)
      : SharedPcmBuffer(std::vector<short>(samples)) {}
  explicit SharedPcmBuffer(std::shared_ptr<const std::vector<short>> samples)
      : samples_(std::move(samples)) {}

  [[nodiscard]] const short *data() const noexcept { return samples_ ? samples_->data() : nullptr; }
  [[nodiscard]] std::size_t size() const noexcept { return samples_ ? samples_->size() : 0; }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] const short *begin() const noexcept { return data(); }
  [[nodiscard]] const short *end() const noexcept { return empty() ? data() : data() + size(); }
  [[nodiscard]] const short &operator[](std::size_t index) const noexcept { return (*samples_)[index]; }
  [[nodiscard]] const std::shared_ptr<const std::vector<short>> &storage() const noexcept {
    return samples_;
  }

  friend bool operator==(const SharedPcmBuffer &left, const SharedPcmBuffer &right) {
    return left.samples_ == right.samples_ || std::ranges::equal(left, right);
  }
  friend bool operator==(const SharedPcmBuffer &left, const std::vector<short> &right) {
    return std::ranges::equal(left, right);
  }

private:
  std::shared_ptr<const std::vector<short>> samples_;
};

} // namespace audio
