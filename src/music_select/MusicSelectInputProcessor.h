#pragma once

#include <cstdint>
#include <set>
#include <vector>

enum class MusicSelectKeyLayout { Beat7K, Popn9K, Beat14K };

// MusicSelectKeyProperty.MusicSelectKey in declaration order.
enum class MusicSelectAssignedKey : std::uint8_t {
  Play,
  Auto,
  Replay,
  Up,
  Down,
  FolderOpen,
  FolderClose,
  Practice,
  Option1Up,
  Option1Down,
  GaugeUp,
  GaugeDown,
  OptionDpUp,
  OptionDpDown,
  HsFixUp,
  HsFixDown,
  Option2Up,
  Option2Down,
  TargetUp,
  TargetDown,
  JudgeArea,
  NoMine,
  BpmGuide,
  LegacyNote,
  Constant,
  JudgeWindowUp,
  JudgeWindowDown,
  MarkNote,
  BgaUp,
  BgaDown,
  GaugeAutoShiftUp,
  GaugeAutoShiftDown,
  DurationUp,
  DurationDown,
  NotesTimingUp,
  NotesTimingDown,
  NotesTimingAutoAdjust,
  NextReplay,
};

[[nodiscard]] bool musicSelectKeyAssigned(MusicSelectKeyLayout,
                                          std::size_t lane,
                                          MusicSelectAssignedKey) noexcept;

enum class MusicSelectControlKey {
  Num0,
  Num1,
  Num2,
  Num3,
  Num4,
  Num5,
  Num7,
  Num8,
  Num9,
  Up,
  Down,
  Left,
  Right,
  Enter,
  Escape,
};

enum class MusicSelectCommandKey {
  AutoplayFolder,
  OpenIr,
  AddFavoriteSong,
  AddFavoriteChart,
  UpdateFolder,
  OpenExplorer,
  CopyMd5,
  CopySha256,
};

enum class MusicSelectInputBarKind { Other, Selectable, Directory };

struct MusicSelectLogicalInput {
  std::vector<bool> keys;
  std::vector<bool> changed;
  std::vector<bool> analog;
  std::vector<int> analogDelta;
  int wheel = 0;
  bool start = false;
  bool select = false;
  bool eventMode = false;
  int selectedReplay = -1;
  MusicSelectInputBarKind currentBar = MusicSelectInputBarKind::Selectable;
  bool selectedBarChanged = false;
  std::set<MusicSelectControlKey> controlPressed;
  std::set<MusicSelectControlKey> controlHeld;
  std::set<MusicSelectCommandKey> commands;
};

enum class MusicSelectInputActionKind {
  SearchPrompt,
  Event,
  CommandNextReplay,
  CommandSameFolder,
  CopyMd5,
  CopySha256,
  SetPanel,
  ResetBarInput,
  BarInput,
  MoveNext,
  MovePrevious,
  OptionOpen,
  OptionClose,
  OptionChange,
  ScratchSound,
  ToggleCustomJudge,
  ToggleConstant,
  ToggleShowJudgeArea,
  ToggleLegacyNote,
  ToggleMarkProcessedNote,
  ToggleBpmGuide,
  ToggleNoMine,
  Play,
  Practice,
  Autoplay,
  Replay,
  AutoplayFolder,
  OpenFolder,
  CloseFolder,
  SelectedBarMoved,
  SongBarChangeTimer,
  ExitApplication,
};

struct MusicSelectInputAction {
  MusicSelectInputActionKind kind = MusicSelectInputActionKind::Event;
  int value = 0;
  int argument1 = 0;
  int argument2 = 0;
};

struct MusicSelectInputProcessorConfig {
  MusicSelectKeyLayout layout = MusicSelectKeyLayout::Beat7K;
  int scrollDurationLowMillis = 120;
  int scrollDurationHighMillis = 40;
  int analogTicksPerScroll = 1;
};

class MusicSelectInputProcessor final {
public:
  explicit MusicSelectInputProcessor(MusicSelectInputProcessorConfig);
  void setLayout(MusicSelectKeyLayout layout) noexcept {
    config_.layout = layout;
  }
  [[nodiscard]] std::vector<MusicSelectInputAction>
  process(MusicSelectLogicalInput, std::int64_t nowMillis);

private:
  MusicSelectInputProcessorConfig config_;
  std::int64_t duration_ = 0;
  int angle_ = 0;
  int analogScrollBuffer_ = 0;
  bool isOptionKeyPressed_ = false;
  bool isOptionKeyReleased_ = false;
  std::int64_t timeChangeDuration_ = 0;
  int countChangeDuration_ = 0;
  bool normalBarKeyInput_ = false;
};
