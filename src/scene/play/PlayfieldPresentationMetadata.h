#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct PlayfieldPlayOptionSelection {
  std::optional<std::string> option;
  std::optional<long long> seed;
  std::optional<std::string> option2;
  std::optional<long long> seed2;

  bool operator==(const PlayfieldPlayOptionSelection &) const = default;
};

// Immutable after publication: retained frames can outlive a practice-option
// change, course transition or the scene that originally captured them.
struct PlayfieldPresentationMetadata {
  std::string playOptionLabel;
  std::vector<std::string> courseStageTitles;
};

class PlayfieldPresentationMetadataCache {
public:
  // title views are borrowed only for this call. Formatting and owning copies
  // happen only when effective option inputs or title content change.
  template <typename FormatLabel>
  [[nodiscard]] std::shared_ptr<const PlayfieldPresentationMetadata> get(
      const PlayfieldPlayOptionSelection &options,
      std::span<const std::string_view> stageTitles, FormatLabel &&formatLabel) {
    if (metadata_ && options_ == options &&
        std::ranges::equal(metadata_->courseStageTitles, stageTitles,
                           [](const std::string &stored, std::string_view title) {
                             return stored == title;
                           })) {
      return metadata_;
    }
    auto next = std::make_shared<PlayfieldPresentationMetadata>();
    next->playOptionLabel = formatLabel(options);
    next->courseStageTitles.reserve(stageTitles.size());
    for (const auto title : stageTitles) {
      next->courseStageTitles.emplace_back(title);
    }
    // Prepare the owning key before publishing, so allocation failure leaves
    // the previous cache entry and all captured snapshots intact.
    auto nextOptions = options;
    options_ = std::move(nextOptions);
    metadata_ = std::move(next);
    return metadata_;
  }

private:
  PlayfieldPlayOptionSelection options_;
  std::shared_ptr<const PlayfieldPresentationMetadata> metadata_;
};
