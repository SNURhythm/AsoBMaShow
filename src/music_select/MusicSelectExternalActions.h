#pragma once

#include "MusicSelectTypes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct MusicSelectExplorerLookups {
  std::function<std::vector<std::filesystem::path>(
      std::span<const std::string>)>
      originalMd5Paths;
  std::function<std::vector<std::filesystem::path>(std::string_view)>
      textPaths;
};

using MusicSelectArchiveDocumentResolver = std::function<
    std::optional<std::vector<std::filesystem::path>>(
        const std::filesystem::path &)>;

using MusicSelectArchivePathSplitter = std::function<bool(
    const std::filesystem::path &, std::filesystem::path &,
    std::filesystem::path &)>;

enum class MusicSelectPointerOrigin {
  Mouse,
  Touch,
};

[[nodiscard]] constexpr bool
musicSelectPointerActivatesRow(MusicSelectPointerOrigin origin) {
  return origin == MusicSelectPointerOrigin::Mouse;
}

enum class MusicSelectTouchTarget : std::uint8_t {
  None,
  Bar,
  Slider,
};

struct MusicSelectTouchMotion {
  bool accepted = false;
  bool sliderDrag = false;
  int rowDelta = 0;
};

struct MusicSelectTouchRelease {
  bool accepted = false;
  bool tap = false;
};

// The skinned selector has discrete Beatoraja bars rather than a native
// RecyclerView. This capture converts a direct vertical swipe into those
// discrete bar movements while retaining slider drags and tap selection.
class MusicSelectTouchGesture final {
public:
  [[nodiscard]] bool begin(std::int64_t finger, float normalizedY,
                           MusicSelectTouchTarget target) noexcept {
    if (finger_ || target == MusicSelectTouchTarget::None) return false;
    finger_ = finger;
    target_ = target;
    rowAnchorY_ = normalizedY;
    dragged_ = false;
    return true;
  }

  [[nodiscard]] MusicSelectTouchMotion
  move(std::int64_t finger, float normalizedY) noexcept {
    if (!finger_ || *finger_ != finger) return {};
    MusicSelectTouchMotion result{.accepted = true};
    if (target_ == MusicSelectTouchTarget::Slider) {
      dragged_ = true;
      result.sliderDrag = true;
      return result;
    }
    while (normalizedY <= rowAnchorY_ - kNormalizedRowStep) {
      ++result.rowDelta;
      rowAnchorY_ -= kNormalizedRowStep;
    }
    while (normalizedY >= rowAnchorY_ + kNormalizedRowStep) {
      --result.rowDelta;
      rowAnchorY_ += kNormalizedRowStep;
    }
    dragged_ = dragged_ || result.rowDelta != 0;
    return result;
  }

  [[nodiscard]] MusicSelectTouchRelease end(std::int64_t finger) noexcept {
    if (!finger_ || *finger_ != finger) return {};
    const bool tap = target_ == MusicSelectTouchTarget::Bar && !dragged_;
    finger_.reset();
    target_ = MusicSelectTouchTarget::None;
    return {.accepted = true, .tap = tap};
  }

  void cancel() noexcept {
    finger_.reset();
    target_ = MusicSelectTouchTarget::None;
  }

private:
  static constexpr float kNormalizedRowStep = 0.04F;
  std::optional<std::int64_t> finger_;
  MusicSelectTouchTarget target_ = MusicSelectTouchTarget::None;
  float rowAnchorY_ = 0.0F;
  bool dragged_ = false;
};

[[nodiscard]] std::vector<std::filesystem::path>
musicSelectDocumentPaths(const MusicSelectBar &,
                         const MusicSelectArchiveDocumentResolver & = {});

[[nodiscard]] std::string
musicSelectExplorerTitleQuery(std::string_view title);

[[nodiscard]] std::optional<std::filesystem::path>
musicSelectExplorerPath(const MusicSelectBar &,
                        const MusicSelectExplorerLookups &,
                        const MusicSelectArchivePathSplitter & = {});

[[nodiscard]] std::optional<std::filesystem::path>
musicSelectRefreshPath(const MusicSelectBar &,
                       const MusicSelectArchivePathSplitter &);

[[nodiscard]] std::vector<std::string>
musicSelectDownloadUrls(const MusicSelectBar &);
