#pragma once

#include "MusicSelectTypes.h"

#include <stop_token>
#include <string_view>
#include <utility>

class MusicSelectSongIndex final {
public:
  explicit MusicSelectSongIndex(std::string directoryContext);

  void add(const ChartMetaRecord &, const std::optional<ScoreBestSnapshot> &,
           int clearRank);
  void finish(std::stop_token stop = {});
  std::pair<std::string, std::string>
  configure(std::string_view mode, std::string_view difficulty,
             std::string_view sort, std::stop_token stop = {});
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] const std::filesystem::path &pathAt(std::size_t) const;
  [[nodiscard]] const MusicSelectBarId &idAt(std::size_t) const;
  [[nodiscard]] const std::string &titleAt(std::size_t) const;
  [[nodiscard]] std::optional<std::size_t>
  indexOf(const MusicSelectBarId &) const;

private:
  struct Entry {
    std::filesystem::path path;
    MusicSelectBarId id;
    std::string title;
    std::string titleKey;
    std::string artistKey;
    double maxBpm = 0;
    double level = 0;
    double scoreRate = 0;
    std::int64_t length = 0;
    std::int64_t duration = 0;
    std::int64_t lastPlayed = 0;
    int difficulty = 0;
    int lamp = 0;
    int badPoints = 0;
    std::uint16_t modes = 0;
    std::uint16_t difficulties = 0;
    bool hidden = false;
    bool exists = false;
    bool hasScore = false;
    bool hasScoreRate = false;
    bool hasDuration = false;
  };

  struct Configuration {
    std::string mode;
    std::string difficulty;
    std::string sort;
    std::pair<std::string, std::string> resolved;
  };

  [[nodiscard]] static int compare(const Entry &, const Entry &,
                                    std::string_view sort);

  std::string directoryContext_;
  std::vector<Entry> entries_;
  std::vector<std::string> hashes_;
  std::vector<std::size_t> rows_;
  std::vector<std::size_t> positions_;
  std::vector<std::size_t> identityOrder_;
  std::optional<Configuration> configuration_;
  bool finished_ = false;
};
