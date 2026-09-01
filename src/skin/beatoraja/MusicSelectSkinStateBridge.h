#pragma once

#include "Skin2DRenderer.h"
#include "../../music_select/MusicSelectTypes.h"

#include <functional>
#include <filesystem>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace skin {

struct MusicSelectSkinFrame {
  std::uint64_t serial = 0;
  std::int64_t elapsedMillis = 0;
  MusicSelectPropertyValues properties;
  MusicSelectSongListFrame songList;
  std::filesystem::path stageFile;
  std::filesystem::path backBmp;
  std::filesystem::path banner;
};

struct MusicSelectSkinActionSink {
  std::function<void(int, std::span<const int>)> event;
  std::function<void(int, double)> floatWriter;
  std::function<void(int, std::string_view)> stringWriter;
};

class MusicSelectSkinStateBridge final : public ISkinFrameState {
public:
  explicit MusicSelectSkinStateBridge(const MusicSelectSkinFrame &);
  MusicSelectSkinStateBridge(const MusicSelectSkinFrame &,
                             std::map<int, std::int64_t> &,
                             const std::set<int> &);

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
  bool setTimerProperty(int, std::int64_t) override;
  void setCustomTimer(int, std::int64_t);
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
  std::map<int, std::int64_t> customTimerValues_;
  std::map<int, std::int64_t> *persistentCustomTimerValues_ = nullptr;
  const std::set<int> *activeCustomTimerIds_ = nullptr;
};

} // namespace skin
