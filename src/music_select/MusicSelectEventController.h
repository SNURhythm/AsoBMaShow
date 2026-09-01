#pragma once

#include "../AppSettings.h"

#include <string>
#include <string_view>
#include <vector>

enum class MusicSelectEventEffectKind {
  RefreshBars,
  OptionChangeSound,
  OpenSettings,
  Play,
  Autoplay,
  Practice,
  Replay,
  OpenDocument,
  OpenIr,
  UpdateFolder,
  OpenExplorer,
  OpenDownloadSite,
  ChangeFavoriteSong,
  ChangeFavoriteChart,
  SetRival,
};

struct MusicSelectEventEffect {
  MusicSelectEventEffectKind kind =
      MusicSelectEventEffectKind::OptionChangeSound;
  int value = 0;
};

struct MusicSelectEventContext {
  AppSettings &settings;
  int sortIndex = 0;
  bool hasSelectedPlayConfig = false;
  bool selectedSongHasPath = false;
  int rivalCount = 0;
  int currentRivalIndex = -1;
};

struct MusicSelectEventOutcome {
  std::vector<MusicSelectEventEffect> effects;
  bool settingsChanged = false;
  std::string failure;
};

class MusicSelectEventController final {
public:
  [[nodiscard]] static MusicSelectEventOutcome
  execute(MusicSelectEventContext &, int eventId, std::string_view eventName,
          int argument1, int argument2);
};
