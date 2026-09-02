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
using MusicSelectArchiveFilePredicate =
    std::function<bool(const std::filesystem::path &)>;

enum class MusicSelectPointerOrigin {
  Mouse,
  Touch,
};

[[nodiscard]] constexpr bool
musicSelectPointerActivatesRow(MusicSelectPointerOrigin origin) {
  return origin == MusicSelectPointerOrigin::Mouse ||
         origin == MusicSelectPointerOrigin::Touch;
}

// BarRenderer hands the clicked Bar to MusicSelector.select. That method
// changes the current directory only for DirectoryBar; every other click
// starts playback from the manager's already centered bar.
[[nodiscard]] constexpr bool
musicSelectPointerKeepsCenteredBar(skin::MusicSelectBarKind kind) {
  return !skin::musicSelectIsDirectoryBarKind(kind);
}

enum class MusicSelectTouchTarget : std::uint8_t {
  None,
  Bar,
  Slider,
  Navigation,
};

struct MusicSelectTouchMotion {
  bool accepted = false;
  bool sliderDrag = false;
  int rowDelta = 0;
};

struct MusicSelectTouchRelease {
  bool accepted = false;
  bool tap = false;
  bool goBack = false;
};

// The skinned selector has discrete Beatoraja bars rather than a native
// RecyclerView. This capture converts a direct vertical swipe into those
// discrete bar movements while retaining slider drags and tap selection. An
// unclaimed right swipe beginning at the left edge navigates back one folder.
class MusicSelectTouchGesture final {
public:
  [[nodiscard]] bool begin(std::int64_t finger, float normalizedX,
                           float normalizedY,
                           MusicSelectTouchTarget target) noexcept {
    if (finger_ || target == MusicSelectTouchTarget::None) return false;
    if (target == MusicSelectTouchTarget::Navigation &&
        normalizedX > kNavigationEdgeWidth) {
      return false;
    }
    finger_ = finger;
    target_ = target;
    navigationAnchorX_ = normalizedX;
    rowAnchorY_ = normalizedY;
    dragged_ = false;
    goBack_ = false;
    return true;
  }

  [[nodiscard]] MusicSelectTouchMotion
  move(std::int64_t finger, float normalizedX, float normalizedY) noexcept {
    if (!finger_ || *finger_ != finger) return {};
    MusicSelectTouchMotion result{.accepted = true};
    if (target_ == MusicSelectTouchTarget::Navigation) {
      const float horizontalDistance = normalizedX - navigationAnchorX_;
      const float verticalDistance = normalizedY - rowAnchorY_;
      goBack_ = horizontalDistance >= kNavigationDistance &&
                horizontalDistance > std::abs(verticalDistance);
      return result;
    }
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
    const bool goBack = target_ == MusicSelectTouchTarget::Navigation && goBack_;
    finger_.reset();
    target_ = MusicSelectTouchTarget::None;
    goBack_ = false;
    return {.accepted = true, .tap = tap, .goBack = goBack};
  }

  void cancel() noexcept {
    finger_.reset();
    target_ = MusicSelectTouchTarget::None;
    goBack_ = false;
  }

private:
  static constexpr float kNormalizedRowStep = 0.04F;
  static constexpr float kNavigationEdgeWidth = 0.10F;
  static constexpr float kNavigationDistance = 0.10F;
  std::optional<std::int64_t> finger_;
  MusicSelectTouchTarget target_ = MusicSelectTouchTarget::None;
  float navigationAnchorX_ = 0.0F;
  float rowAnchorY_ = 0.0F;
  bool dragged_ = false;
  bool goBack_ = false;
};

[[nodiscard]] std::vector<std::filesystem::path>
musicSelectDocumentPaths(const MusicSelectBar &,
                         const MusicSelectArchiveDocumentResolver & = {});

[[nodiscard]] std::string
musicSelectExplorerTitleQuery(std::string_view title);

[[nodiscard]] std::optional<std::filesystem::path>
musicSelectExplorerPath(const MusicSelectBar &,
                        const MusicSelectExplorerLookups &,
                        const MusicSelectArchivePathSplitter & = {},
                        const MusicSelectArchiveFilePredicate & = {});

[[nodiscard]] std::optional<int>
musicSelectDifficultyTableUpdateId(const MusicSelectBar &);

[[nodiscard]] std::optional<std::filesystem::path>
musicSelectRefreshPath(const MusicSelectBar &,
                       const MusicSelectArchivePathSplitter &);

[[nodiscard]] std::vector<std::string>
musicSelectDownloadUrls(const MusicSelectBar &);
