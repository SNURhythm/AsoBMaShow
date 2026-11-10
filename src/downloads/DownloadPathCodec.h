#pragma once

#include "../../yoga/lib/nlohmann/json.hpp"
#include <filesystem>
#include <string>

namespace downloads {

// The current private store is supplied by the platform, never by a manifest.
// iOS moves Documents and Application Support together when its container ID
// changes. Persist only these owned subtrees relative to that container.
class DownloadPathCodec {
public:
  explicit DownloadPathCodec(const std::filesystem::path &storeRoot) {
    const auto root = storeRoot.lexically_normal();
    if (root.is_absolute() && root.filename() == "GingerRushDownloadsV1" &&
        root.parent_path().filename() == "Application Support" &&
        root.parent_path().parent_path().filename() == "Library")
      container_ = root.parent_path().parent_path().parent_path();
  }
  bool portable() const { return !container_.empty(); }

  nlohmann::json encode(const nlohmann::json &value) const {
    const auto path = read(value);
    if (portable() && path.is_absolute()) {
      const auto relative = path.lexically_relative(container_);
      if (owned(relative)) return {{"containerRelative", utf8(relative)}};
    }
    return value;
  }

  nlohmann::json decode(const nlohmann::json &value) const {
    if (value.is_object()) {
      if (!portable() || value.size() != 1 || !value.contains("containerRelative"))
        throw std::runtime_error("Invalid download container identity.");
      const auto relative = read(value.at("containerRelative"));
      if (!owned(relative)) throw std::runtime_error("Unsafe relative download path.");
      return utf8(container_ / relative);
    }
    const auto path = read(value);
    if (!portable() || !path.is_absolute() || path != path.lexically_normal()) return value;
    // Migrate v1 absolute paths without trusting a saved container root. Match
    // the same iOS container layout accepted by the chart library migration.
    std::filesystem::path prefix;
    for (const auto &part : path) {
      prefix /= part;
      if (!uuid(prefix.filename().string()) || prefix.parent_path().filename() != "Application" ||
          prefix.parent_path().parent_path().filename() != "Data" ||
          prefix.parent_path().parent_path().parent_path().filename() != "Containers") continue;
      const auto relative = path.lexically_relative(prefix);
      if (owned(relative)) return utf8(container_ / relative);
      return value;
    }
    return value;
  }

private:
  static bool owned(const std::filesystem::path &relative) {
    if (relative.empty() || relative.is_absolute() || relative != relative.lexically_normal()) return false;
    for (const auto &part : relative) if (part == ".." || part == ".") return false;
    if (*relative.begin() == "Documents") return true;
    const auto privateRelative = relative.lexically_relative("Library/Application Support/GingerRushDownloadsV1");
    return !privateRelative.empty() && *privateRelative.begin() != "..";
  }
  static bool uuid(const std::string &value) {
    if (value.size() != 36) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
      else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f') ||
                 (value[i] >= 'A' && value[i] <= 'F'))) return false;
    }
    return true;
  }
  static std::filesystem::path read(const nlohmann::json &value) {
    const auto text = value.get<std::string>();
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
  }
  static std::string utf8(const std::filesystem::path &path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
  }
  std::filesystem::path container_;
};

} // namespace downloads
