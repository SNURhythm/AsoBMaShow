#pragma once

#include "../../AtomicFile.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ir::tachi {

class BokutachiCacheStore final {
public:
  static constexpr int kCurrentSchemaVersion = 1;
  static constexpr std::size_t kMaximumFileBytes = 1024 * 1024;
  static constexpr std::size_t kMaximumOrigins = 16;
  static constexpr std::size_t kMaximumChartMappings = 2048;
  static constexpr std::size_t kMaximumChartIdBytes = 256;

  BokutachiCacheStore() = default;
  explicit BokutachiCacheStore(atomic_file::Operations operations) noexcept
      : operations_(std::move(operations)) {}

  [[nodiscard]] bool activate(const std::filesystem::path &path,
                              std::string &diagnostic) noexcept;

  [[nodiscard]] std::optional<std::int64_t>
  userId(std::string_view serverOrigin) const noexcept;
  [[nodiscard]] std::optional<std::string>
  chartId(std::string_view serverOrigin, std::string_view game,
          std::string_view chartSha256) const noexcept;

  [[nodiscard]] bool rememberUserId(std::string_view serverOrigin,
                                    std::int64_t userId,
                                    std::string &diagnostic) noexcept;
  [[nodiscard]] bool rememberChartId(std::string_view serverOrigin,
                                     std::string_view game,
                                     std::string_view chartSha256,
                                     std::string_view chartId,
                                     std::string &diagnostic) noexcept;
  [[nodiscard]] bool eraseChartId(std::string_view serverOrigin,
                                  std::string_view game,
                                  std::string_view chartSha256,
                                  std::string &diagnostic) noexcept;
  [[nodiscard]] bool clearUserIds(std::string &diagnostic) noexcept;

private:
  struct ChartMapping {
    std::string game;
    std::string sha256;
    std::string chartId;
  };

  struct OriginEntry {
    std::string serverOrigin;
    std::optional<std::int64_t> userId;
    std::vector<ChartMapping> charts;
  };

  [[nodiscard]] bool saveLocked(std::string &diagnostic);
  [[nodiscard]] bool discardFileLocked(std::string &diagnostic) noexcept;
  [[nodiscard]] bool evictOldestChartLocked() noexcept;
  [[nodiscard]] std::size_t chartCountLocked() const noexcept;
  void removeEmptyOriginsLocked();

  mutable std::mutex mutex_;
  std::filesystem::path path_;
  std::vector<OriginEntry> origins_;
  bool activated_ = false;
  bool writesEnabled_ = true;
  std::optional<atomic_file::Operations> operations_;
};

} // namespace ir::tachi
