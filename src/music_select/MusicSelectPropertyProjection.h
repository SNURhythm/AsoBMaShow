#pragma once

#include "MusicSelectBarManager.h"
#include "../AppSettings.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

enum class MusicSelectRankingState {
  None,
  Access,
  Finish,
  Fail,
};

struct MusicSelectRankingEntry {
  std::string name;
  int score = 0;
  int rank = 0;
  int playerType = 0;
  int clearType = 0;
};

struct MusicSelectRankingSnapshot {
  MusicSelectRankingState state = MusicSelectRankingState::None;
  int totalPlayers = 0;
  int rank = 0;
  std::array<int, 11> clearCounts{};
  std::vector<MusicSelectRankingEntry> entries;
  int offset = 0;
  std::int64_t pendingDurationMillis = -1;
};

struct MusicSelectClockFields {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
};

struct MusicSelectPropertyRuntimeSnapshot {
  MusicSelectClockFields wallClock;
  std::int64_t applicationUptimeMillis = 0;
  int framesPerSecond = 0;
  int panelState = 0;
  int selectedReplay = -1;
  int sortIndex = 0;
  std::string playerName;
  std::string targetName;
  std::string rivalName;
  std::string searchWord;
  std::string tableName;
  std::string tableLevel;
  std::string tableFullName;
  std::string version;
  std::string irName;
  std::string irUserName;
  bool irOnline = false;
  bool updateScore = true;
  PlayerScoreHistorySnapshot playerHistory;
  MusicSelectRankingSnapshot ranking;
};

[[nodiscard]] skin::MusicSelectPropertyValues
projectMusicSelectProperties(const AppSettings &,
                             const MusicSelectBarManagerSnapshot &,
                             const MusicSelectPropertyRuntimeSnapshot &);
