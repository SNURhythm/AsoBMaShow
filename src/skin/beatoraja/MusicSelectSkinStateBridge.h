#pragma once

#include "Skin2DRenderer.h"
#include "../../music_select/MusicSelectTypes.h"

#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace skin {

struct MusicSelectPropertyValues {
  std::map<int, bool> booleans;
  std::map<int, std::int64_t> integers;
  std::map<int, std::int64_t> imageIndexes;
  std::map<int, double> rates;
  std::map<int, double> floats;
  std::map<int, std::string> strings;
  std::map<int, std::int64_t> timers;
  std::map<std::string, bool, std::less<>> namedBooleans;
  std::map<std::string, std::int64_t, std::less<>> namedIntegers;
  std::map<std::string, std::int64_t, std::less<>> namedImageIndexes;
  std::map<std::string, double, std::less<>> namedRates;
  std::map<std::string, double, std::less<>> namedFloats;
  std::map<std::string, std::string, std::less<>> namedStrings;
  std::map<std::string, std::int64_t, std::less<>> namedTimers;
};

struct MusicSelectSkinFrame {
  std::uint64_t serial = 0;
  std::int64_t elapsedMillis = 0;
  MusicSelectPropertyValues properties;
  MusicSelectSongListFrame songList;
};

struct MusicSelectSkinActionSink {
  std::function<void(int, std::span<const int>)> event;
  std::function<void(int, double)> floatWriter;
  std::function<void(int, std::string_view)> stringWriter;
};

class MusicSelectSkinStateBridge final : public ISkinFrameState {
public:
  explicit MusicSelectSkinStateBridge(const MusicSelectSkinFrame &);

  std::uint64_t frameSerial() const noexcept override;
  SkinPropertyLookup<bool>
  booleanProperty(const SkinBuiltinPropertySelector &) override;
  SkinPropertyLookup<std::int64_t>
  integerProperty(const SkinBuiltinPropertySelector &,
                  SkinIntegerPropertyDomain) override;
  SkinPropertyLookup<double>
  floatProperty(const SkinBuiltinPropertySelector &,
                SkinFloatPropertyDomain) override;
  SkinPropertyLookup<std::string_view>
  stringProperty(const SkinBuiltinPropertySelector &) override;
  SkinPropertyLookup<SkinRuntimeOffset> offsetProperty(int) override;
  std::int64_t timerProperty(const SkinBuiltinPropertySelector &) override;
  std::span<const SkinProjectedNoteView>
  projectedNotes() const noexcept override;
  std::span<const SkinProjectedLongNoteView>
  projectedLongNotes() const noexcept override;
  std::span<const SkinProjectedLineView>
  projectedLines() const noexcept override;
  SkinGaugeStateView gaugeState() const noexcept override;
  SkinJudgeStateView judgeState(int) const noexcept override;
  SkinNoteExpansionStateView noteExpansionState() const noexcept override;

private:
  const MusicSelectSkinFrame *frame_ = nullptr;
};

} // namespace skin
