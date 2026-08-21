#include "SkinMovieCatalog.h"

#include <limits>

namespace skin {

const PreparedSkinMovie *
SkinMovieCatalog::findMovie(SkinResourceId id) const noexcept {
  const auto found = movies_.find(id);
  return found == movies_.end() ? nullptr : &found->second;
}

SkinMovieCatalogFrameResult SkinMovieCatalog::prepareFrame(
    std::span<const SkinMovieCommand *const> commands,
    const PlaySkinViewport &viewport) {
  SkinMovieCatalogFrameResult result;
  if (commands.empty()) {
    result.ready = true;
    return result;
  }
  if (!device_ || !device_->ownsCurrentThread()) {
    return result;
  }
  device_->beginFrame();
  preparedCount_ = 0;
  const auto add = [](std::uint64_t &target, std::uint64_t amount) {
    if (amount > std::numeric_limits<std::uint64_t>::max() - target) {
      return false;
    }
    target += amount;
    return true;
  };
  for (const auto *command : commands) {
    const auto *movie = command != nullptr ? findMovie(command->resource)
                                            : nullptr;
    if (movie == nullptr) {
      discardFrame();
      return result;
    }
    const auto prepared =
        device_->prepareFrame(movie->handle, *command, viewport);
    if (!prepared.ready ||
        !add(result.requirements.vertexBytes,
             prepared.requirements.vertexBytes) ||
        !add(result.requirements.vertexAlignmentPadding,
             prepared.requirements.vertexAlignmentPadding) ||
        !add(result.requirements.indexCount,
             prepared.requirements.indexCount)) {
      discardFrame();
      return result;
    }
    ++preparedCount_;
  }
  result.ready = true;
  return result;
}

void SkinMovieCatalog::discardFrame() noexcept {
  preparedCount_ = 0;
  if (device_) {
    device_->discardFrame();
  }
}

void SkinMovieCatalog::commitFrame() noexcept {
  if (device_ && preparedCount_ != 0) {
    device_->commitFrame();
  }
}

void SkinMovieCatalog::submitPrepared(std::size_t index) noexcept {
  if (device_ && index < preparedCount_) {
    device_->submitPrepared(index);
  }
}

} // namespace skin
