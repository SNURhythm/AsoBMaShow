#include "MusicSelectBarManager.h"

#include <algorithm>
#include <ranges>
#include <utility>

MusicSelectBarManager::MusicSelectBarManager(MusicSelectProjection projection)
    : projection_(std::move(projection)) {
  rebuildRows();
}

const MusicSelectBar *MusicSelectBarManager::selected() const {
  return selectedIndex_ < rows_.size() ? &rows_[selectedIndex_] : nullptr;
}

void MusicSelectBarManager::rebuildRows(
    std::optional<MusicSelectBarId> preferred) {
  rows_.clear();
  const std::vector<MusicSelectBarId> *ids = &projection_.root;
  if (!directory_.empty()) {
    const auto *directory = projection_.find(directory_.back());
    if (directory != nullptr) ids = &directory->children;
  }
  rows_.reserve(ids->size());
  for (const auto &id : *ids) {
    if (const auto *bar = projection_.find(id)) rows_.push_back(*bar);
  }
  selectedIndex_ = 0;
  if (preferred) {
    const auto found = std::ranges::find(rows_, *preferred,
                                         &MusicSelectBar::id);
    if (found != rows_.end()) {
      selectedIndex_ = static_cast<std::size_t>(found - rows_.begin());
    }
  }
}

bool MusicSelectBarManager::openSelected() {
  const auto *bar = selected();
  if (bar == nullptr || bar->children.empty()) return false;
  const MusicSelectBarId id = bar->id;
  directory_.push_back(id);
  rebuildRows();
  return true;
}

bool MusicSelectBarManager::close() {
  if (directory_.empty()) return false;
  const MusicSelectBarId closed = directory_.back();
  directory_.pop_back();
  rebuildRows(closed);
  return true;
}

void MusicSelectBarManager::move(bool increase, std::int64_t nowMillis,
                                 std::int64_t durationMillis) {
  if (rows_.empty()) return;
  if (increase) {
    selectedIndex_ = (selectedIndex_ + 1) % rows_.size();
    movementDirection_ = 1;
  } else {
    selectedIndex_ = (selectedIndex_ + rows_.size() - 1) % rows_.size();
    movementDirection_ = -1;
  }
  movementEndMillis_ = nowMillis + durationMillis;
}

void MusicSelectBarManager::setSelectedPosition(float value) {
  if (rows_.empty()) return;
  if (value >= 0.0F && value < 1.0F) {
    selectedIndex_ = static_cast<std::size_t>(rows_.size() * value);
  }
}

void MusicSelectBarManager::refresh(MusicSelectProjection projection) {
  std::optional<MusicSelectBarId> preferred;
  if (const auto *bar = selected()) preferred = bar->id;
  projection_ = std::move(projection);
  directory_.erase(
      std::remove_if(directory_.begin(), directory_.end(),
                     [this](const auto &id) {
                       const auto *bar = projection_.find(id);
                       return bar == nullptr || bar->children.empty();
                     }),
      directory_.end());
  rebuildRows(preferred);
}

MusicSelectBarManagerSnapshot MusicSelectBarManager::snapshot() const {
  std::string directoryText;
  for (const auto &id : directory_) {
    if (const auto *bar = projection_.find(id)) {
      directoryText += bar->title + " > ";
    }
  }
  return {.rows = rows_,
          .selectedIndex = selectedIndex_,
          .directory = directory_,
          .directoryText = std::move(directoryText),
          .movementDirection = movementDirection_,
          .movementEndMillis = movementEndMillis_};
}
