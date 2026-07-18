#include "BokutachiCacheStore.h"

#include "../../VersionedJson.h"
#include "../IrProfileSettings.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <utility>

namespace ir::tachi {
namespace {

bool validNormalizedOrigin(std::string_view value) {
  const auto normalized = normalizeServerOrigin(value);
  return normalized && *normalized == value;
}

bool validGame(std::string_view value) {
  return value == "bms-7k" || value == "bms-14k";
}

bool validSha256(std::string_view value) {
  return value.size() == 64 &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

bool validChartId(std::string_view value) {
  return !value.empty() &&
         value.size() <= BokutachiCacheStore::kMaximumChartIdBytes &&
         std::ranges::none_of(value, [](unsigned char character) {
           return character <= 0x20U || character == 0x7fU;
         });
}

std::optional<std::int64_t> positiveInteger(const nlohmann::json &value) {
  if (value.is_number_unsigned()) {
    const auto encoded = value.get<std::uint64_t>();
    if (encoded == 0 ||
        encoded > static_cast<std::uint64_t>(
                      std::numeric_limits<std::int64_t>::max())) {
      return std::nullopt;
    }
    return static_cast<std::int64_t>(encoded);
  }
  if (!value.is_number_integer()) {
    return std::nullopt;
  }
  const auto encoded = value.get<std::int64_t>();
  return encoded > 0 ? std::optional(encoded) : std::nullopt;
}

std::string firstDiagnostic(const versioned_json::LoadResult &loaded,
                            std::string_view fallback) {
  return loaded.diagnostics.empty() ? std::string(fallback)
                                    : loaded.diagnostics.front();
}

} // namespace

bool BokutachiCacheStore::activate(const std::filesystem::path &path,
                                   std::string &diagnostic) noexcept {
  try {
    std::scoped_lock lock(mutex_);
    diagnostic.clear();
    path_ = path;
    origins_.clear();
    activated_ = true;
    writesEnabled_ = true;

    std::error_code error;
    const bool exists = std::filesystem::exists(path_, error);
    if (error) {
      writesEnabled_ = false;
      diagnostic = "Bokutachi cache existence check failed: " + error.message();
      return false;
    }
    if (!exists) {
      return true;
    }
    const auto size = std::filesystem::file_size(path_, error);
    if (error) {
      writesEnabled_ = false;
      diagnostic = "Bokutachi cache size check failed: " + error.message();
      return false;
    }
    if (size > kMaximumFileBytes) {
      diagnostic = "Bokutachi cache exceeds the one MiB size limit";
      return false;
    }

    const std::array<versioned_json::Migration, 1> migrations{
        [](nlohmann::json &, std::string &migrationError) {
          migrationError = "unversioned Bokutachi caches are unsupported";
          return false;
        }};
    const auto loaded = versioned_json::loadAndMigrate(
        path_, kCurrentSchemaVersion, migrations);
    if (loaded.status == versioned_json::LoadStatus::FutureVersion) {
      writesEnabled_ = false;
      diagnostic =
          firstDiagnostic(loaded, "Bokutachi cache is newer than this build");
      return false;
    }
    if (loaded.status != versioned_json::LoadStatus::Loaded) {
      if (loaded.status == versioned_json::LoadStatus::IoError) {
        writesEnabled_ = false;
      }
      diagnostic =
          firstDiagnostic(loaded, "Bokutachi cache could not be loaded");
      return false;
    }

    const auto origins = loaded.document.find("origins");
    if (origins == loaded.document.end() || !origins->is_array() ||
        origins->size() > kMaximumOrigins) {
      diagnostic = "Bokutachi cache origins are invalid";
      return false;
    }

    std::vector<OriginEntry> parsed;
    parsed.reserve(origins->size());
    std::unordered_set<std::string> seenOrigins;
    std::size_t chartCount = 0;
    for (const auto &encodedOrigin : *origins) {
      if (!encodedOrigin.is_object()) {
        diagnostic = "Bokutachi cache origin entry is invalid";
        return false;
      }
      const auto origin = encodedOrigin.find("serverOrigin");
      const auto charts = encodedOrigin.find("charts");
      if (origin == encodedOrigin.end() || !origin->is_string() ||
          !validNormalizedOrigin(origin->get_ref<const std::string &>()) ||
          charts == encodedOrigin.end() || !charts->is_array() ||
          !seenOrigins.insert(origin->get<std::string>()).second) {
        diagnostic = "Bokutachi cache origin fields are invalid";
        return false;
      }
      OriginEntry entry{.serverOrigin = origin->get<std::string>()};
      if (const auto user = encodedOrigin.find("userID");
          user != encodedOrigin.end()) {
        entry.userId = positiveInteger(*user);
        if (!entry.userId) {
          diagnostic = "Bokutachi cache user identity is invalid";
          return false;
        }
      }

      std::unordered_set<std::string> seenCharts;
      entry.charts.reserve(charts->size());
      for (const auto &encodedChart : *charts) {
        if (++chartCount > kMaximumChartMappings || !encodedChart.is_object()) {
          diagnostic = "Bokutachi cache chart collection is invalid";
          return false;
        }
        const auto game = encodedChart.find("game");
        const auto sha = encodedChart.find("sha256");
        const auto chartId = encodedChart.find("chartID");
        if (game == encodedChart.end() || !game->is_string() ||
            !validGame(game->get_ref<const std::string &>()) ||
            sha == encodedChart.end() || !sha->is_string() ||
            !validSha256(sha->get_ref<const std::string &>()) ||
            chartId == encodedChart.end() || !chartId->is_string() ||
            !validChartId(chartId->get_ref<const std::string &>())) {
          diagnostic = "Bokutachi cache chart fields are invalid";
          return false;
        }
        std::string chartKey =
            game->get<std::string>() + "\n" + sha->get<std::string>();
        if (!seenCharts.insert(std::move(chartKey)).second) {
          diagnostic = "Bokutachi cache contains a duplicate chart mapping";
          return false;
        }
        entry.charts.push_back({.game = game->get<std::string>(),
                                .sha256 = sha->get<std::string>(),
                                .chartId = chartId->get<std::string>()});
      }
      parsed.push_back(std::move(entry));
    }
    origins_ = std::move(parsed);
    return true;
  } catch (const std::exception &error) {
    diagnostic =
        std::string("Bokutachi cache activation failed: ") + error.what();
    return false;
  } catch (...) {
    diagnostic = "Bokutachi cache activation failed";
    return false;
  }
}

std::optional<std::int64_t>
BokutachiCacheStore::userId(std::string_view serverOrigin) const noexcept {
  try {
    std::scoped_lock lock(mutex_);
    const auto found =
        std::ranges::find(origins_, serverOrigin, &OriginEntry::serverOrigin);
    return found == origins_.end() ? std::nullopt : found->userId;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string>
BokutachiCacheStore::chartId(std::string_view serverOrigin,
                             std::string_view game,
                             std::string_view chartSha256) const noexcept {
  try {
    std::scoped_lock lock(mutex_);
    const auto origin =
        std::ranges::find(origins_, serverOrigin, &OriginEntry::serverOrigin);
    if (origin == origins_.end()) {
      return std::nullopt;
    }
    const auto chart = std::ranges::find_if(
        origin->charts, [&](const ChartMapping &candidate) {
          return candidate.game == game && candidate.sha256 == chartSha256;
        });
    return chart == origin->charts.end()
               ? std::nullopt
               : std::optional<std::string>(chart->chartId);
  } catch (...) {
    return std::nullopt;
  }
}

bool BokutachiCacheStore::rememberUserId(std::string_view serverOrigin,
                                         std::int64_t userId,
                                         std::string &diagnostic) noexcept {
  try {
    std::scoped_lock lock(mutex_);
    diagnostic.clear();
    if (!activated_ || !writesEnabled_) {
      diagnostic = "Bokutachi cache writes are unavailable";
      return false;
    }
    if (!validNormalizedOrigin(serverOrigin) || userId <= 0) {
      diagnostic = "Bokutachi cache user identity is invalid";
      return false;
    }
    auto found =
        std::ranges::find(origins_, serverOrigin, &OriginEntry::serverOrigin);
    if (found != origins_.end() && found->userId == userId) {
      return true;
    }
    if (found == origins_.end()) {
      if (origins_.size() == kMaximumOrigins) {
        origins_.erase(origins_.begin());
      }
      origins_.push_back({.serverOrigin = std::string(serverOrigin)});
      found = std::prev(origins_.end());
    }
    found->userId = userId;
    return saveLocked(diagnostic);
  } catch (const std::exception &error) {
    diagnostic = std::string("Bokutachi cache update failed: ") + error.what();
    return false;
  } catch (...) {
    diagnostic = "Bokutachi cache update failed";
    return false;
  }
}

bool BokutachiCacheStore::rememberChartId(std::string_view serverOrigin,
                                          std::string_view game,
                                          std::string_view chartSha256,
                                          std::string_view chartId,
                                          std::string &diagnostic) noexcept {
  try {
    std::scoped_lock lock(mutex_);
    diagnostic.clear();
    if (!activated_ || !writesEnabled_) {
      diagnostic = "Bokutachi cache writes are unavailable";
      return false;
    }
    if (!validNormalizedOrigin(serverOrigin) || !validGame(game) ||
        !validSha256(chartSha256) || !validChartId(chartId)) {
      diagnostic = "Bokutachi cache chart mapping is invalid";
      return false;
    }
    auto origin =
        std::ranges::find(origins_, serverOrigin, &OriginEntry::serverOrigin);
    if (origin != origins_.end()) {
      const auto chart = std::ranges::find_if(
          origin->charts, [&](const ChartMapping &candidate) {
            return candidate.game == game && candidate.sha256 == chartSha256;
          });
      if (chart != origin->charts.end()) {
        if (chart->chartId == chartId) {
          return true;
        }
        chart->chartId = chartId;
        return saveLocked(diagnostic);
      }
    }
    if (origin == origins_.end()) {
      if (origins_.size() == kMaximumOrigins) {
        origins_.erase(origins_.begin());
      }
      origins_.push_back({.serverOrigin = std::string(serverOrigin)});
      origin = std::prev(origins_.end());
    }
    while (chartCountLocked() >= kMaximumChartMappings) {
      const auto oldest =
          std::ranges::find_if(origins_, [](const OriginEntry &candidate) {
            return !candidate.charts.empty();
          });
      if (oldest == origins_.end()) {
        break;
      }
      oldest->charts.erase(oldest->charts.begin());
    }
    origin =
        std::ranges::find(origins_, serverOrigin, &OriginEntry::serverOrigin);
    if (origin == origins_.end()) {
      origins_.push_back({.serverOrigin = std::string(serverOrigin)});
      origin = std::prev(origins_.end());
    }
    origin->charts.push_back({.game = std::string(game),
                              .sha256 = std::string(chartSha256),
                              .chartId = std::string(chartId)});
    return saveLocked(diagnostic);
  } catch (const std::exception &error) {
    diagnostic = std::string("Bokutachi cache update failed: ") + error.what();
    return false;
  } catch (...) {
    diagnostic = "Bokutachi cache update failed";
    return false;
  }
}

bool BokutachiCacheStore::eraseChartId(std::string_view serverOrigin,
                                       std::string_view game,
                                       std::string_view chartSha256,
                                       std::string &diagnostic) noexcept {
  try {
    std::scoped_lock lock(mutex_);
    diagnostic.clear();
    if (!activated_ || !writesEnabled_) {
      diagnostic = "Bokutachi cache writes are unavailable";
      return false;
    }
    if (!validNormalizedOrigin(serverOrigin) || !validGame(game) ||
        !validSha256(chartSha256)) {
      diagnostic = "Bokutachi cache chart mapping is invalid";
      return false;
    }
    const auto origin =
        std::ranges::find(origins_, serverOrigin, &OriginEntry::serverOrigin);
    if (origin == origins_.end()) {
      return true;
    }
    const auto oldSize = origin->charts.size();
    std::erase_if(origin->charts, [&](const ChartMapping &candidate) {
      return candidate.game == game && candidate.sha256 == chartSha256;
    });
    if (origin->charts.size() == oldSize) {
      return true;
    }
    removeEmptyOriginsLocked();
    return saveLocked(diagnostic);
  } catch (const std::exception &error) {
    diagnostic = std::string("Bokutachi cache update failed: ") + error.what();
    return false;
  } catch (...) {
    diagnostic = "Bokutachi cache update failed";
    return false;
  }
}

bool BokutachiCacheStore::clearUserIds(std::string &diagnostic) noexcept {
  try {
    std::scoped_lock lock(mutex_);
    diagnostic.clear();
    const bool changed =
        std::ranges::any_of(origins_, [](const OriginEntry &entry) {
          return entry.userId.has_value();
        });
    if (!changed) {
      return true;
    }
    if (!activated_ || !writesEnabled_) {
      diagnostic = "Bokutachi cache writes are unavailable";
      return false;
    }
    for (auto &origin : origins_) {
      origin.userId.reset();
    }
    removeEmptyOriginsLocked();
    if (saveLocked(diagnostic)) {
      return true;
    }
    const std::string writeDiagnostic = diagnostic;
    if (discardFileLocked(diagnostic)) {
      diagnostic.clear();
      return true;
    }
    diagnostic = writeDiagnostic +
                 (diagnostic.empty()
                      ? "; stale Bokutachi cache could not be discarded"
                      : "; stale Bokutachi cache could not be discarded: " +
                            diagnostic);
    return false;
  } catch (const std::exception &error) {
    diagnostic = std::string("Bokutachi cache update failed: ") + error.what();
    return false;
  } catch (...) {
    diagnostic = "Bokutachi cache update failed";
    return false;
  }
}

bool BokutachiCacheStore::saveLocked(std::string &diagnostic) {
  for (;;) {
    nlohmann::json encodedOrigins = nlohmann::json::array();
    for (const auto &origin : origins_) {
      nlohmann::json encodedCharts = nlohmann::json::array();
      for (const auto &chart : origin.charts) {
        encodedCharts.push_back({{"game", chart.game},
                                 {"sha256", chart.sha256},
                                 {"chartID", chart.chartId}});
      }
      nlohmann::json encodedOrigin{{"serverOrigin", origin.serverOrigin},
                                   {"charts", std::move(encodedCharts)}};
      if (origin.userId) {
        encodedOrigin["userID"] = *origin.userId;
      }
      encodedOrigins.push_back(std::move(encodedOrigin));
    }
    nlohmann::json document{{"schemaVersion", kCurrentSchemaVersion},
                            {"origins", std::move(encodedOrigins)}};
    const std::size_t encodedBytes = document.dump(2).size() + 1;
    if (encodedBytes <= kMaximumFileBytes) {
      return versioned_json::saveAtomic(
          path_, document, diagnostic,
          operations_ ? &*operations_ : nullptr);
    }
    const std::size_t chartCount = chartCountLocked();
    if (chartCount == 0) {
      diagnostic = "Bokutachi cache cannot fit within the one MiB size limit";
      return false;
    }
    const std::size_t averageChartBytes =
        std::max<std::size_t>(1, encodedBytes / chartCount);
    const std::size_t excessBytes = encodedBytes - kMaximumFileBytes;
    const std::size_t evictionCount =
        std::max<std::size_t>(
            1, (excessBytes + averageChartBytes - 1) / averageChartBytes);
    for (std::size_t index = 0; index < evictionCount; ++index) {
      if (!evictOldestChartLocked()) {
        break;
      }
    }
    removeEmptyOriginsLocked();
  }
}

bool BokutachiCacheStore::discardFileLocked(
    std::string &diagnostic) noexcept {
  try {
    bool changed = false;
    for (const auto &candidate : {
             path_, std::filesystem::path(path_.string() + ".tmp"),
             std::filesystem::path(path_.string() + ".bak"),
             std::filesystem::path(path_.string() + ".bak.pending"),
             std::filesystem::path(path_.string() + ".bak.previous")}) {
      std::error_code error;
      const bool removed = std::filesystem::remove(candidate, error);
      if (error) {
        diagnostic = "unable to discard stale Bokutachi cache: " +
                     error.message();
        return false;
      }
      changed = changed || removed;
    }
    if (!changed) {
      return true;
    }
    const auto parent = path_.parent_path().empty()
                            ? std::filesystem::path(".")
                            : path_.parent_path();
    return atomic_file::syncDirectory(parent, diagnostic);
  } catch (const std::exception &error) {
    diagnostic = std::string("unable to discard stale Bokutachi cache: ") +
                 error.what();
    return false;
  } catch (...) {
    diagnostic = "unable to discard stale Bokutachi cache";
    return false;
  }
}

bool BokutachiCacheStore::evictOldestChartLocked() noexcept {
  const auto oldest =
      std::ranges::find_if(origins_, [](const OriginEntry &candidate) {
        return !candidate.charts.empty();
      });
  if (oldest == origins_.end()) {
    return false;
  }
  oldest->charts.erase(oldest->charts.begin());
  return true;
}

std::size_t BokutachiCacheStore::chartCountLocked() const noexcept {
  std::size_t count = 0;
  for (const auto &origin : origins_) {
    count += origin.charts.size();
  }
  return count;
}

void BokutachiCacheStore::removeEmptyOriginsLocked() {
  std::erase_if(origins_, [](const OriginEntry &entry) {
    return !entry.userId && entry.charts.empty();
  });
}

} // namespace ir::tachi
