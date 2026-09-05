#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace image_decode {

struct DecodedImageData {
  int width = 0;
  int height = 0;
  std::shared_ptr<std::vector<unsigned char>> rgba;

  [[nodiscard]] std::size_t byteSize() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
};

class DecodedImageCache {
public:
  explicit DecodedImageCache(std::size_t byteBudget);

  [[nodiscard]] std::optional<DecodedImageData> get(std::string_view key);
  bool put(std::string key, DecodedImageData image);
  bool pin(std::string_view key);
  bool unpin(std::string_view key);
  bool erase(std::string_view key);
  std::size_t erasePrefix(std::string_view prefix);
  void clearEvictable();
  void clear();

  [[nodiscard]] bool contains(std::string_view key) const;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t bytes() const noexcept;
  [[nodiscard]] std::size_t budget() const noexcept;

private:
  struct Entry {
    DecodedImageData image;
    std::size_t pinCount = 0;
    std::list<std::string>::iterator recency;
  };

  void touch(std::map<std::string, Entry, std::less<>>::iterator entry);
  void evictToBudget(std::string_view protectedKey = {});
  bool eraseLocked(std::string_view key);

  mutable std::mutex mutex_;
  std::size_t byteBudget_ = 0;
  std::size_t bytes_ = 0;
  std::list<std::string> recency_;
  std::map<std::string, Entry, std::less<>> entries_;
};

} // namespace image_decode
