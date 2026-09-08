#include "MusicSelectSqlSongs.h"

#include <algorithm>
#include <stdexcept>

namespace {

std::string chartIdentity(const ChartMetaRecord &record) {
  if (!record.meta.SHA256.empty()) return "sha256:" + record.meta.SHA256;
  if (!record.meta.MD5.empty()) return "md5:" + record.meta.MD5;
  return "path:" + fspath_to_utf8(record.meta.BmsPath.lexically_normal());
}

}

MusicSelectSqlSongs::MusicSelectSqlSongs(
    std::string context, QueryResolver resolve, Projector project,
    MusicSelectBarManagerConfig config)
    : context_(std::move(context)), resolve_(std::move(resolve)),
      project_(std::move(project)), config_(std::move(config)) {
  auto resolver = resolve_;
  query_ = resolver(config_);
  resetPages();
}

MusicSelectSqlSongs::MusicSelectSqlSongs(const MusicSelectSqlSongs &source,
                                        PreparedClone)
    : context_(source.context_), resolve_(source.resolve_),
      project_(source.project_), config_(source.config_), query_(source.query_) {
  resetPages();
}

std::size_t MusicSelectSqlSongs::size() const noexcept {
  return query_.count;
}

std::shared_ptr<MusicSelectRowProvider> MusicSelectSqlSongs::clone() const {
  return std::shared_ptr<MusicSelectRowProvider>(
      new MusicSelectSqlSongs(*this, PreparedClone{}));
}

const MusicSelectBar &MusicSelectSqlSongs::at(std::size_t index) const {
  if (index >= size()) throw std::out_of_range("music-select row index");
  return pages_.get(index);
}

std::optional<std::size_t>
MusicSelectSqlSongs::indexOf(const MusicSelectBarId &id) const {
  const auto prefix = context_ + ":";
  if (size() == 0 || !id.value.starts_with(prefix)) return std::nullopt;
  const auto identity = std::string_view(id.value).substr(prefix.size());
  if (!(identity.starts_with("sha256:") && identity.size() > 7) &&
      !(identity.starts_with("md5:") && identity.size() > 4) &&
      !(identity.starts_with("path:") && identity.size() > 5)) {
    return std::nullopt;
  }
  try {
    const auto index = query_.findIndex(identity);
    if (!index || *index < size()) return index;
    diagnostic_ = "The library changed while locating this chart.";
  } catch (const std::exception &error) {
    diagnostic_ = error.what();
    if (diagnostic_.empty()) diagnostic_ = "Unable to locate chart.";
  } catch (...) {
    diagnostic_ = "Unable to locate chart.";
  }
  return std::nullopt;
}

std::pair<std::string, std::string> MusicSelectSqlSongs::configure(
    const std::string &mode, const std::string &difficulty,
    const std::string &sort) {
  if (config_.sortId == sort &&
      ((config_.modeFilter == mode && config_.difficultyFilter == difficulty) ||
       (query_.resolvedFilters.first == mode &&
        query_.resolvedFilters.second == difficulty))) {
    return query_.resolvedFilters;
  }
  MusicSelectBarManagerConfig config{mode, difficulty, sort};
  auto resolver = resolve_;
  auto query = resolver(config);
  config_ = std::move(config);
  query_ = std::move(query);
  resetPages();
  return query_.resolvedFilters;
}

const std::string &MusicSelectSqlSongs::diagnostic() const noexcept {
  return diagnostic_;
}

void MusicSelectSqlSongs::retryFailedPages() {
  if (!diagnostic_.empty()) resetPages();
}

void MusicSelectSqlSongs::resetPages() {
  diagnostic_.clear();
  pages_.reset(size(), [this](std::size_t offset, std::size_t limit) {
    return loadPage(offset, limit);
  });
}

MusicSelectBar MusicSelectSqlSongs::unavailableBar(std::size_t index) const {
  return {.id = {context_ + ":unavailable:" + std::to_string(index)},
          .kind = skin::MusicSelectBarKind::Song,
          .presentation = {.kind = skin::MusicSelectBarKind::Song,
                           .exists = false}};
}

std::vector<MusicSelectBar> MusicSelectSqlSongs::loadPage(
    std::size_t offset, std::size_t limit) const {
  const auto count = std::min(limit, size() - offset);
  std::vector<MusicSelectBar> result;
  result.reserve(count);
  if (!diagnostic_.empty()) {
    for (std::size_t index = 0; index < count; ++index) {
      result.push_back(unavailableBar(offset + index));
    }
    return result;
  }
  try {
    const auto records = query_.loadPage(offset, count);
    if (records.size() != count) {
      throw std::runtime_error("The library changed while loading this folder.");
    }
    for (const auto &record : records) {
      if (record.meta.BmsPath.empty()) {
        throw std::runtime_error("A chart is unavailable in this folder.");
      }
      auto bar = project_(record);
      if (bar.id.value != context_ + ":" + chartIdentity(record) ||
          bar.kind != skin::MusicSelectBarKind::Song || !bar.chart) {
        throw std::runtime_error("The library changed while loading this folder.");
      }
      result.push_back(std::move(bar));
    }
    return result;
  } catch (const std::exception &error) {
    diagnostic_ = error.what();
    if (diagnostic_.empty()) diagnostic_ = "Unable to load chart page.";
  } catch (...) {
    diagnostic_ = "Unable to load chart page.";
  }
  result.clear();
  for (std::size_t index = 0; index < count; ++index) {
    result.push_back(unavailableBar(offset + index));
  }
  return result;
}
