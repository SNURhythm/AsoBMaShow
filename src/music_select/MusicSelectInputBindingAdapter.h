#pragma once

#include "MusicSelectInputProcessor.h"
#include "../input/InputBindingResolver.h"

#include <map>
#include <optional>
#include <set>
#include <span>
#include <vector>

[[nodiscard]] MusicSelectKeyLayout
musicSelectKeyLayoutForConfig(int musicSelectInput) noexcept;

[[nodiscard]] std::vector<input::InputScope>
musicSelectInputScopes(MusicSelectKeyLayout);

[[nodiscard]] std::optional<std::size_t>
musicSelectKeyIndex(MusicSelectKeyLayout, input::InputScope,
                    input::LogicalAction) noexcept;

class MusicSelectInputBindingAdapter final {
public:
  MusicSelectInputBindingAdapter(const InputProfile &, MusicSelectKeyLayout);

  void consume(const input::PhysicalInputEvent &);
  void disconnectDevice(std::string_view stableId);
  void reset();

  [[nodiscard]] MusicSelectLogicalInput &state() noexcept { return state_; }
  [[nodiscard]] const MusicSelectLogicalInput &state() const noexcept {
    return state_;
  }
  void clearFrameEdges();

private:
  struct LogicalKey {
    input::InputScope scope;
    input::LogicalAction action;
    auto operator<=>(const LogicalKey &) const = default;
  };

  void apply(std::span<const input::LogicalInputTransition>);
  void consumeAnalog(const input::PhysicalInputEvent &);

  const InputProfile profile_;
  const MusicSelectKeyLayout layout_;
  MusicSelectLogicalInput state_;
  std::map<std::size_t, std::set<LogicalKey>> heldKeys_;
  std::set<LogicalKey> heldStart_;
  std::set<LogicalKey> heldSelect_;
  std::map<std::size_t, float> analogValues_;
  InputBindingResolver resolver_;
};
