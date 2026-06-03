#include "bms_parser.hpp"
/*
 * Copyright (C) 2024 VioletXF, khoeun03
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

namespace bms_parser {
Chart::Chart() = default;

Chart::~Chart() {
  for (const auto &measure : Measures) {
    delete measure;
  }

  Measures.clear();
}
} // namespace bms_parser

/*
 * Copyright (C) 2024 VioletXF, khoeun03
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

namespace bms_parser {
LandmineNote::LandmineNote(float Damage) : Note(0) { this->Damage = Damage; }

LandmineNote::~LandmineNote() = default;
} // namespace bms_parser

/*
 * Copyright (C) 2024 VioletXF, khoeun03
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

namespace bms_parser {
bool LongNote::IsTail() const { return Tail == nullptr; }

LongNote::LongNote(int Wav) : Note(Wav) { Tail = nullptr; }

void LongNote::Press(long long Time) {
  Play(Time);
  IsHolding = true;
  Tail->IsHolding = true;
}

void LongNote::Release(long long Time) {
  Play(Time);
  IsHolding = false;
  Head->IsHolding = false;
  ReleaseTime = Time;
}

void LongNote::MissPress(long long Time) {}

void LongNote::Reset() {
  Note::Reset();
  IsHolding = false;
  ReleaseTime = 0;
}

LongNote::~LongNote() {
  Head = nullptr;
  Tail = nullptr;
}
} // namespace bms_parser

/*
 * Copyright (C) 2024 VioletXF, khoeun03
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


namespace bms_parser {
Measure::~Measure() {
  for (const auto &Timeline : TimeLines) {
    delete Timeline;
  }
  TimeLines.clear();
}
} // namespace bms_parser

/*
 * Copyright (C) 2024 VioletXF, khoeun03
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

namespace bms_parser {
Note::Note(int wav) { Wav = wav; }

void Note::Play(long long Time) {
  IsPlayed = true;
  PlayedTime = Time;
}

void Note::Press(long long Time) { Play(Time); }

void Note::Reset() {
  IsPlayed = false;
  IsDead = false;
  PlayedTime = 0;
}

Note::~Note() { Timeline = nullptr; }
} // namespace bms_parser

/*
 * Copyright (C) 2024 VioletXF, khoeun03
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <cwctype>
#include <iterator>
#include <random>
#include <thread>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

#ifndef BMS_PARSER_VERBOSE
#define BMS_PARSER_VERBOSE 0
#endif

namespace bms_parser {
class threadRAII {
  std::thread th;

public:
  explicit threadRAII(std::thread &&_th) { th = std::move(_th); }

  ~threadRAII() {
    if (th.joinable()) {
      th.join();
    }
  }
};

enum Channel {
  LaneAutoplay = 1,
  SectionRate = 2,
  BpmChange = 3,
  BgaPlay = 4,
  PoorPlay = 6,
  LayerPlay = 7,
  BpmChangeExtend = 8,
  Stop = 9,

  P1KeyBase = 1 * 36 + 1,
  P2KeyBase = 2 * 36 + 1,
  P1InvisibleKeyBase = 3 * 36 + 1,
  P2InvisibleKeyBase = 4 * 36 + 1,
  P1LongKeyBase = 5 * 36 + 1,
  P2LongKeyBase = 6 * 36 + 1,
  P1MineKeyBase = 13 * 36 + 1,
  P2MineKeyBase = 14 * 36 + 1,

  Scroll = 1020
};

namespace KeyAssign {
const int Beat7[] = {0, 1, 2, 3, 4, 7, -1, 5, 6, 8, 9, 10, 11, 12, 15, -1, 13, 14};
const int Beat4[] = {0, 1, -1, 2, 3, -1, -1, -1, -1,
                     -1, -1, -1, -1, -1, -1, -1, -1, -1};
const int Beat6[] = {0, 1, 2, -1, 3, -1, -1, 4, 5,
                     -1, -1, -1, -1, -1, -1, -1, -1, -1};
const int Beat8[] = {1, 2, 3, 4, 5, 0, -1, 6, 7,
                     -1, -1, -1, -1, -1, -1, -1, -1, -1};
int PopN[] = {0, 1, 2, 3, 4, -1, -1, -1, -1, -1, 5, 6, 7, 8, -1, -1, -1, -1};

const int *Scratchless(int keyMode) {
  switch (keyMode) {
  case 4:
    return Beat4;
  case 6:
    return Beat6;
  case 8:
    return Beat8;
  default:
    return Beat7;
  }
}
} // namespace KeyAssign

constexpr int TempKey = 16;

Parser::Parser() : BpmTable{}, StopLengthTable{}, ScrollTable{} {
  std::random_device seeder;
  Seed = seeder();
}

bool Parser::IsSupportedRandomPrng(const std::string &RandomPrng) {
  return RandomPrng == RandomPrngId;
}

bool Parser::SetRandomPrng(const std::string &RandomPrng) {
  if (!IsSupportedRandomPrng(RandomPrng)) {
    return false;
  }
  this->RandomPrng = RandomPrng;
  return true;
}

const std::string &Parser::GetRandomPrng() const { return RandomPrng; }

void Parser::SetRandomSeed(unsigned int RandomSeed) { Seed = RandomSeed; }

unsigned int Parser::GetRandomSeed() const { return Seed; }

int Parser::NoWav = -1;
int Parser::MetronomeWav = -2;

inline bool Parser::MatchHeader(const std::string_view &str,
                                const std::string_view &headerUpper) {
  auto size = headerUpper.length();
  if (str.length() < size) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    if (std::towupper(str[i]) != headerUpper[i]) {
      return false;
    }
  }
  return true;
}

void Parser::Parse(const std::filesystem::path &fpath, Chart **chart,
                   bool addReadyMeasure, bool metaOnly,
                   std::atomic_bool &bCancelled) {
#if BMS_PARSER_VERBOSE == 1
  auto startTime = std::chrono::high_resolution_clock::now();
#endif
  std::vector<unsigned char> bytes;
  std::ifstream file(fpath, std::ios::binary);
  if (!file.is_open()) {
    std::cout << "Failed to open file: " << fpath << std::endl;
    return;
  }
#if BMS_PARSER_VERBOSE == 1
  // measure file read time
  auto midStartTime = std::chrono::high_resolution_clock::now();
#endif
  file.seekg(0, std::ios::end);
  auto size = file.tellg();
  file.seekg(0, std::ios::beg);
  bytes.resize(static_cast<size_t>(size));
  file.read(reinterpret_cast<char *>(bytes.data()), size);
  file.close();
#if BMS_PARSER_VERBOSE == 1
  std::cout << "File read took "
            << std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::high_resolution_clock::now() - midStartTime)
                   .count()
            << "\n";
#endif
  Parse(bytes, chart, addReadyMeasure, metaOnly, bCancelled);
  auto new_chart = *chart;
  if (new_chart != nullptr) {
    new_chart->Meta.BmsPath = fpath;

    new_chart->Meta.Folder = fpath.parent_path();
  }
#if BMS_PARSER_VERBOSE == 1
  auto endTime = std::chrono::high_resolution_clock::now();
  std::cout << "Total parsing+reading took "
            << std::chrono::duration_cast<std::chrono::microseconds>(endTime -
                                                                     startTime)
                   .count()
            << "\n";
#endif
}

void Parser::Parse(const std::vector<unsigned char> &bytes, Chart **chart,
                   bool addReadyMeasure, bool metaOnly,
                   std::atomic_bool &bCancelled) {
#if BMS_PARSER_VERBOSE == 1
  auto startTime = std::chrono::high_resolution_clock::now();
#endif
  auto new_chart = new Chart();
  *chart = new_chart;
  new_chart->Meta.RandomSeed = Seed;
  new_chart->Meta.RandomPrng = RandomPrng;

  static std::regex headerRegex(R"(^#([A-Za-z]+?)(\d\d)? +?(.+)?)");

  if (bCancelled) {
    return;
  }

  auto measures =
      std::unordered_map<int, std::vector<std::pair<int, std::string>>>();

  // compute hash in separate thread
  std::thread md5Thread([&bytes, new_chart] {
#if BMS_PARSER_VERBOSE == 1
    auto startTime = std::chrono::high_resolution_clock::now();
#endif
    MD5 md5;
    md5.update(bytes.data(), bytes.size());
    md5.finalize();
    new_chart->Meta.MD5 = md5.hexdigest();
#if BMS_PARSER_VERBOSE == 1
    std::cout << "Hashing MD5 took "
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::high_resolution_clock::now() - startTime)
                     .count()
              << "\n";
#endif
  });
  threadRAII md5RAII(std::move(md5Thread));
  std::thread sha256Thread([&bytes, new_chart] {
#if BMS_PARSER_VERBOSE == 1
    auto startTime = std::chrono::high_resolution_clock::now();
#endif
    new_chart->Meta.SHA256 = sha256(bytes);
#if BMS_PARSER_VERBOSE == 1
    std::cout << "Hashing SHA256 took "
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::high_resolution_clock::now() - startTime)
                     .count()
              << "\n";
#endif
  });
  threadRAII sha256RAII(std::move(sha256Thread));

  // std::cout<<"file size: "<<size<<std::endl;
  // bytes to std::string
#if BMS_PARSER_VERBOSE == 1
  auto midStartTime = std::chrono::high_resolution_clock::now();
#endif
  std::string content;
  ShiftJISConverter::BytesToUTF8(bytes.data(), bytes.size(), content);
#if BMS_PARSER_VERBOSE == 1
  std::cout << "ShiftJIS-UTF8 conversion took "
            << std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::high_resolution_clock::now() - midStartTime)
                   .count()
            << "\n";
#endif
  // std::wcout<<content<<std::endl;
  std::vector<int> RandomStack;
  std::vector<bool> SkipStack;
  // init prng with seed
  std::mt19937_64 Prng(Seed);

  std::string line;
  std::istringstream stream(content);
#if BMS_PARSER_VERBOSE == 1
  midStartTime = std::chrono::high_resolution_clock::now();
#endif
  auto lastMeasure = -1;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (bCancelled) {
      return;
    }
    // std::cout << line << std::endl;
    if (line.size() <= 1 || line[0] != L'#')
      continue;
    if (bCancelled) {
      return;
    }

    if (MatchHeader(line, "#IF")) // #IF n
    {
      if (RandomStack.empty()) {
        // UE_LOG(LogTemp, Warning, TEXT("RandomStack is empty!"));
        continue;
      }
      const int CurrentRandom = RandomStack.back();
      const int n =
          static_cast<int>(std::strtol(line.substr(4).c_str(), nullptr, 10));
      SkipStack.push_back(CurrentRandom != n);
      continue;
    }
    if (MatchHeader(line, "#ELSE")) {
      if (SkipStack.empty()) {
        // UE_LOG(LogTemp, Warning, TEXT("SkipStack is empty!"));
        continue;
      }
      const bool CurrentSkip = SkipStack.back();
      SkipStack.pop_back();
      SkipStack.push_back(!CurrentSkip);
      continue;
    }
    if (MatchHeader(line, "#ELSEIF")) {
      if (SkipStack.empty()) {
        // UE_LOG(LogTemp, Warning, TEXT("SkipStack is empty!"));
        continue;
      }
      const bool CurrentSkip = SkipStack.back();
      SkipStack.pop_back();
      const int CurrentRandom = RandomStack.back();
      const int n =
          static_cast<int>(std::strtol(line.substr(8).c_str(), nullptr, 10));
      SkipStack.push_back(CurrentSkip && CurrentRandom != n);
      continue;
    }
    if (MatchHeader(line, "#ENDIF") || MatchHeader(line, "#END IF")) {
      if (SkipStack.empty()) {
        // UE_LOG(LogTemp, Warning, TEXT("SkipStack is empty!"));
        continue;
      }
      SkipStack.pop_back();
      continue;
    }
    if (!SkipStack.empty() && SkipStack.back()) {
      continue;
    }
    if (MatchHeader(line, "#RANDOM") ||
        MatchHeader(line, "#RONDAM")) // #RANDOM n
    {
      const int n =
          static_cast<int>(std::strtol(line.substr(7).c_str(), nullptr, 10));
      std::uniform_int_distribution<int> dist(1, n);
      RandomStack.push_back(dist(Prng));
      continue;
    }
    if (MatchHeader(line, "#ENDRANDOM")) {
      if (RandomStack.empty()) {
        // UE_LOG(LogTemp, Warning, TEXT("RandomStack is empty!"));
        continue;
      }
      RandomStack.pop_back();
      continue;
    }
    if (MatchHeader(line, "#4K")) {
      ParseHeader(new_chart, "4K", "", "");
      continue;
    }
    if (MatchHeader(line, "#6K")) {
      ParseHeader(new_chart, "6K", "", "");
      continue;
    }
    if (MatchHeader(line, "#8K")) {
      ParseHeader(new_chart, "8K", "", "");
      continue;
    }

    if (line.length() >= 7 && std::isdigit(static_cast<unsigned char>(line[1])) &&
        std::isdigit(static_cast<unsigned char>(line[2])) &&
        std::isdigit(static_cast<unsigned char>(line[3])) &&
        line[6] == ':') {
      const int measure =
          static_cast<int>(std::strtol(line.substr(1, 3).c_str(), nullptr, 10));
      lastMeasure = std::max(lastMeasure, measure);
      const std::string ch = line.substr(4, 2);
      const int channel = ParseInt(ch);
      const std::string value = line.substr(7);
      if (measures.find(measure) == measures.end()) {
        measures[measure] = std::vector<std::pair<int, std::string>>();
      }
      measures[measure].emplace_back(channel, value);
    } else {
      if (MatchHeader(line, "#WAV")) {
        if (metaOnly) {
          continue;
        }
        if (line.length() < 7) {
          continue;
        }
        const auto xx = line.substr(4, 2);
        const auto value = line.substr(7);
        ParseHeader(new_chart, "WAV", xx, value);
      } else if (MatchHeader(line, "#BMP")) {
        if (metaOnly) {
          continue;
        }
        if (line.length() < 7) {
          continue;
        }
        const auto xx = line.substr(4, 2);
        const auto value = line.substr(7);
        ParseHeader(new_chart, "BMP", xx, value);
      } else if (MatchHeader(line, "#STOP")) {
        if (line.length() < 8) {
          continue;
        }
        const auto xx = line.substr(5, 2);
        const auto value = line.substr(8);
        ParseHeader(new_chart, "STOP", xx, value);
      } else if (MatchHeader(line, "#BPM")) {
        if (line.substr(4).rfind(' ', 0) == 0) {
          const auto value = line.substr(5);
          ParseHeader(new_chart, "BPM", "", value);
        } else {
          if (line.length() < 7) {
            continue;
          }
          const auto xx = line.substr(4, 2);
          const auto value = line.substr(7);
          ParseHeader(new_chart, "BPM", xx, value);
        }
      } else if (MatchHeader(line, "#SCROLL")) {
        if (line.length() < 10) {
          continue;
        }
        const auto xx = line.substr(7, 2);
        const auto value = line.substr(10);
        ParseHeader(new_chart, "SCROLL", xx, value);
      } else {
        std::smatch matcher;

        if (std::regex_search(line, matcher, headerRegex)) {
          std::string xx = matcher[2].str();
          std::string value = matcher[3].str();
          if (value.empty()) {
            value = xx;
            xx = "";
          }
          ParseHeader(new_chart, matcher[1].str(), xx, value);
        }
      }
    }
  }
#if BMS_PARSER_VERBOSE == 1
  std::cout << "Parsing headers took "
            << std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::high_resolution_clock::now() - midStartTime)
                   .count()
            << "\n";
#endif
  if (bCancelled) {
    return;
  }
  if (addReadyMeasure) {
    measures[0] = std::vector<std::pair<int, std::string>>();
    measures[0].emplace_back(LaneAutoplay, "********");
  }

  double timePassed = 0;
  int totalNotes = 0;
  int totalLongNotes = 0;
  int totalScratchNotes = 0;
  int totalBackSpinNotes = 0;
  int totalLandmineNotes = 0;
  auto currentBpm = new_chart->Meta.Bpm;
  auto currentScroll = 1.0;
  auto minBpm = new_chart->Meta.Bpm;
  auto maxBpm = new_chart->Meta.Bpm;
  auto lastNote = std::vector<Note *>();
  lastNote.resize(TempKey, nullptr);
  auto lnStart = std::vector<LongNote *>();
  lnStart.resize(TempKey, nullptr);
#if BMS_PARSER_VERBOSE == 1
  midStartTime = std::chrono::high_resolution_clock::now();
#endif
  double measureBeatPosition = 0;
  for (auto measureIdx = 0; measureIdx <= lastMeasure; ++measureIdx) {
    if (bCancelled) {
      return;
    }
    if (measures.find(measureIdx) == measures.end()) {
      measures[measureIdx] = std::vector<std::pair<int, std::string>>();
    }

    // gcd (int, int)
    auto measure = new Measure();

    // NOTE: this should be an ordered map
    auto timelines = std::map<double, TimeLine *>();

    for (auto &pair : measures[measureIdx]) {
      if (bCancelled) {
        break;
      }
      auto channel = pair.first;
      auto &data = pair.second;
      if (channel == SectionRate) {
        measure->Scale = std::strtod(data.c_str(), nullptr);
        continue;
      }

      const bool scratchlessKeyMode =
          new_chart->Meta.IsScratchlessKeyMode();
      const auto *keyAssign =
          scratchlessKeyMode ? KeyAssign::Scratchless(new_chart->Meta.KeyMode)
                             : KeyAssign::Beat7;
      auto laneNumber = 0; // NOTE: This is intentionally set to 0, not -1!
      if (channel >= P1KeyBase && channel < P1KeyBase + 9) {
        laneNumber = keyAssign[channel - P1KeyBase];
        channel = P1KeyBase;
      } else if (channel >= P2KeyBase && channel < P2KeyBase + 9) {
        laneNumber = keyAssign[channel - P2KeyBase + 9];
        channel = P1KeyBase;
      } else if (channel >= P1InvisibleKeyBase &&
                 channel < P1InvisibleKeyBase + 9) {
        laneNumber = keyAssign[channel - P1InvisibleKeyBase];
        channel = P1InvisibleKeyBase;
      } else if (channel >= P2InvisibleKeyBase &&
                 channel < P2InvisibleKeyBase + 9) {
        laneNumber = keyAssign[channel - P2InvisibleKeyBase + 9];
        channel = P1InvisibleKeyBase;
      } else if (channel >= P1LongKeyBase && channel < P1LongKeyBase + 9) {
        laneNumber = keyAssign[channel - P1LongKeyBase];
        channel = P1LongKeyBase;
      } else if (channel >= P2LongKeyBase && channel < P2LongKeyBase + 9) {
        laneNumber = keyAssign[channel - P2LongKeyBase + 9];
        channel = P1LongKeyBase;
      } else if (channel >= P1MineKeyBase && channel < P1MineKeyBase + 9) {
        laneNumber = keyAssign[channel - P1MineKeyBase];
        channel = P1MineKeyBase;
      } else if (channel >= P2MineKeyBase && channel < P2MineKeyBase + 9) {
        laneNumber = keyAssign[channel - P2MineKeyBase + 9];
        channel = P1MineKeyBase;
      }

      if (laneNumber == -1 ||
          (scratchlessKeyMode && laneNumber >= new_chart->Meta.KeyMode)) {
        continue;
      }
      const bool isScratch =
          !scratchlessKeyMode && (laneNumber == 7 || laneNumber == 15);
      if (!scratchlessKeyMode) {
        if (laneNumber == 5 || laneNumber == 6 || laneNumber == 13 ||
            laneNumber == 14) {
          if (new_chart->Meta.KeyMode == 5) {
            new_chart->Meta.KeyMode = 7;
          } else if (new_chart->Meta.KeyMode == 10) {
            new_chart->Meta.KeyMode = 14;
          }
        }
        if (laneNumber >= 8) {
          if (new_chart->Meta.KeyMode == 7) {
            new_chart->Meta.KeyMode = 14;
          } else if (new_chart->Meta.KeyMode == 5) {
            new_chart->Meta.KeyMode = 10;
          }
          new_chart->Meta.IsDP = true;
        }
      }

      const auto dataCount = data.length() / 2;
      for (size_t j = 0; j < dataCount; ++j) {
        if (bCancelled) {
          break;
        }
        std::string val = data.substr(j * 2, 2);
        if (val == "00") {
          if (timelines.empty() && j == 0) {
            auto timeline = new TimeLine(TempKey, metaOnly);
            timelines[0] = timeline; // add ghost timeline
          }

          continue;
        }

        const auto g = Gcd(j, dataCount);
        // ReSharper disable PossibleLossOfFraction

        const auto position =
            static_cast<double>(j / g) /
            static_cast<double>(dataCount / g); // NOLINT(*-integer-division)

        if (timelines.find(position) == timelines.end()) {
          timelines[position] = new TimeLine(TempKey, metaOnly);
        }

        auto timeline = timelines[position];
        if (channel == LaneAutoplay || channel == P1InvisibleKeyBase) {
          if (metaOnly) {
            break;
          }
        }
        switch (channel) {
        case LaneAutoplay:
          if (val == "**") {
            timeline->AddBackgroundNote(new Note{MetronomeWav});
            break;
          }
          if (ParseInt(val) != 0) {
            auto bgNote = new Note{ToWaveId(new_chart, val, metaOnly)};
            timeline->AddBackgroundNote(bgNote);
          }

          break;
        case BpmChange: {
          int bpm = ParseHex(val);
          timeline->Bpm = static_cast<double>(bpm);
          // std::cout << "BPM_CHANGE: " << timeline->Bpm << ", on measure " <<
          // measureIdx << std::endl; Debug.Log($"BPM_CHANGE: {timeline.Bpm}, on
          // measure {measureIdx}");
          timeline->BpmChange = true;
          break;
        }
        case BgaPlay:
          timeline->BgaBase = ParseInt(val);
          break;
        case PoorPlay:
          timeline->BgaPoor = ParseInt(val);
          break;
        case LayerPlay:
          timeline->BgaLayer = ParseInt(val);
          break;
        case BpmChangeExtend: {
          const auto id = ParseInt(val);
          // std::cout << "BPM_CHANGE_EXTEND: " << id << ", on measure " <<
          // measureIdx << std::endl;
          if (!CheckResourceIdRange(id)) {
            // UE_LOG(LogTemp, Warning, TEXT("Invalid BPM id: %s"), *val);
            break;
          }
          if (BpmTable.find(id) != BpmTable.end()) {
            timeline->Bpm = BpmTable[id];
          } else {
            timeline->Bpm = 0;
            // std::cout<<"Undefined BPM: "<<id<<std::endl;
          }
          // Debug.Log($"BPM_CHANGE_EXTEND: {timeline.Bpm}, on measure
          // {measureIdx}, {val}");
          timeline->BpmChange = true;
          break;
        }
        case Scroll: {
          const auto id = ParseInt(val);
          if (!CheckResourceIdRange(id)) {
            // UE_LOG(LogTemp, Warning, TEXT("Invalid Scroll id: %s"), *val);
            break;
          }
          timeline->ScrollChange = true;
          if (ScrollTable.find(id) != ScrollTable.end()) {
            timeline->Scroll = ScrollTable[id];

          } else {
            timeline->Scroll = 1;
          }
          // Debug.Log($"SCROLL: {timeline.Scroll}, on measure {measureIdx}");
          break;
        }
        case Stop: {
          const auto id = ParseInt(val);
          if (!CheckResourceIdRange(id)) {
            // UE_LOG(LogTemp, Warning, TEXT("Invalid StopLength id: %s"),
            // *val);
            break;
          }
          if (StopLengthTable.find(id) != StopLengthTable.end()) {
            timeline->StopLength = StopLengthTable[id];
          } else {
            timeline->StopLength = 0;
          }
          // Debug.Log($"STOP: {timeline.StopLength}, on measure {measureIdx}");
          break;
        }
        case P1KeyBase: {
          const auto ch = ParseInt(val);
          if (ch == Lnobj && lastNote[laneNumber] != nullptr) {
            if (isScratch) {
              ++totalBackSpinNotes;
            } else {
              ++totalLongNotes;
            }

            auto last = lastNote[laneNumber];
            lastNote[laneNumber] = nullptr;
            if (metaOnly) {
              break;
            }

            auto lastTimeline = last->Timeline;
            auto ln = new LongNote{last->Wav};
            delete last;
            ln->Tail = new LongNote{NoWav};
            ln->Tail->Head = ln;
            lastTimeline->SetNote(laneNumber, ln);
            timeline->SetNote(laneNumber, ln->Tail);
          } else {
            auto note = new Note{ToWaveId(new_chart, val, metaOnly)};
            lastNote[laneNumber] = note;
            ++totalNotes;
            if (isScratch) {
              ++totalScratchNotes;
            }
            if (metaOnly) {
              delete note; // this is intended
              break;
            }
            timeline->SetNote(laneNumber, note);
          }
        } break;
        case P1InvisibleKeyBase: {
          auto invNote = new Note{ToWaveId(new_chart, val, metaOnly)};
          timeline->SetInvisibleNote(laneNumber, invNote);
          break;
        }

        case P1LongKeyBase:
          if (Lntype == 1) {
            if (lnStart[laneNumber] == nullptr) {
              ++totalNotes;
              if (isScratch) {
                ++totalBackSpinNotes;
              } else {
                ++totalLongNotes;
              }

              auto ln = new LongNote{ToWaveId(new_chart, val, metaOnly)};
              lnStart[laneNumber] = ln;

              if (metaOnly) {
                delete ln; // this is intended
                break;
              }

              timeline->SetNote(laneNumber, ln);
            } else {
              if (!metaOnly) {
                auto tail = new LongNote{NoWav};
                tail->Head = lnStart[laneNumber];
                lnStart[laneNumber]->Tail = tail;
                timeline->SetNote(laneNumber, tail);
              }
              lnStart[laneNumber] = nullptr;
            }
          }

          break;
        case P1MineKeyBase: {
          // landmine
          ++totalLandmineNotes;
          if (metaOnly) {
            break;
          }
          const auto damage = static_cast<float>(ParseInt(val, true)) / 2.0f;
          timeline->SetNote(laneNumber, new LandmineNote{damage});
          break;
        }
        default:
          break;
        }
      }
    }

    new_chart->Meta.TotalNotes = totalNotes;
    new_chart->Meta.TotalLongNotes = totalLongNotes;
    new_chart->Meta.TotalScratchNotes = totalScratchNotes;
    new_chart->Meta.TotalBackSpinNotes = totalBackSpinNotes;
    new_chart->Meta.TotalLandmineNotes = totalLandmineNotes;

    auto lastPosition = 0.0;

    measure->Timing = static_cast<long long>(timePassed);

    for (auto &pair : timelines) {
      if (bCancelled) {
        break;
      }
      const auto position = pair.first;
      const auto timeline = pair.second;

      // Debug.Log($"measure: {measureIdx}, position: {position}, lastPosition:
      // {lastPosition} bpm: {bpm} scale: {measure.scale} interval: {240 * 1000
      // * 1000 * (position - lastPosition) * measure.scale / bpm}");
      const auto interval =
          240000000.0 * (position - lastPosition) * measure->Scale / currentBpm;
      timePassed += interval;
      timeline->Timing = static_cast<long long>(timePassed);
      timeline->BeatPosition = measureBeatPosition + position * measure->Scale;
      if (timeline->BpmChange) {
        currentBpm = timeline->Bpm;
        minBpm = std::min(minBpm, timeline->Bpm);
        maxBpm = std::max(maxBpm, timeline->Bpm);
      } else {
        timeline->Bpm = currentBpm;
      }

      if (timeline->ScrollChange) {
        currentScroll = timeline->Scroll;
      } else {
        timeline->Scroll = currentScroll;
      }

      // Debug.Log($"measure: {measureIdx}, position: {position}, lastPosition:
      // {lastPosition}, bpm: {currentBpm} scale: {measure.Scale} interval:
      // {interval} stop: {timeline.GetStopDuration()}");

      timePassed += timeline->GetStopDuration();
      if (!metaOnly) {
        measure->TimeLines.push_back(timeline);
      }

      lastPosition = position;
    }

    if (metaOnly) {
      for (auto &timeline : timelines) {
        delete timeline.second;
      }
      timelines.clear();
    }

    if (!metaOnly && measure->TimeLines.empty()) {
      auto timeline = new TimeLine(TempKey, metaOnly);
      timeline->Timing = static_cast<long long>(timePassed);
      timeline->BeatPosition = measureBeatPosition;
      timeline->Bpm = currentBpm;
      measure->TimeLines.push_back(timeline);
    }
    if (!metaOnly) {
      measure->TimeLines[0]->IsFirstInMeasure = true;
    }
    new_chart->Meta.PlayLength = static_cast<long long>(timePassed);
    timePassed +=
        240000000.0 * (1 - lastPosition) * measure->Scale / currentBpm;
    measureBeatPosition += measure->Scale;
    if (!metaOnly) {
      new_chart->Measures.push_back(measure);
    } else {
      delete measure;
    }
  }
#if BMS_PARSER_VERBOSE == 1
  std::cout << "Reading data field took "
            << std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::high_resolution_clock::now() - midStartTime)
                   .count()
            << "\n";
#endif
  new_chart->Meta.TotalLength = static_cast<long long>(timePassed);
  new_chart->Meta.MinBpm = minBpm;
  new_chart->Meta.MaxBpm = maxBpm;
  if (new_chart->Meta.Difficulty == 0) {
    std::string FullTitle;
    FullTitle.reserve(new_chart->Meta.Title.length() +
                      new_chart->Meta.SubTitle.length());
    std::transform(new_chart->Meta.Title.begin(), new_chart->Meta.Title.end(),
                   std::back_inserter(FullTitle), ::towlower);
    std::transform(new_chart->Meta.SubTitle.begin(),
                   new_chart->Meta.SubTitle.end(),
                   std::back_inserter(FullTitle), ::towlower);
    if (FullTitle.find("easy") != std::string::npos) {
      new_chart->Meta.Difficulty = 1;
    } else if (FullTitle.find("normal") != std::string::npos) {
      new_chart->Meta.Difficulty = 2;
    } else if (FullTitle.find("hyper") != std::string::npos) {
      new_chart->Meta.Difficulty = 3;
    } else if (FullTitle.find("another") != std::string::npos) {
      new_chart->Meta.Difficulty = 4;
    } else if (FullTitle.find("insane") != std::string::npos) {
      new_chart->Meta.Difficulty = 5;
    } else {
      if (totalNotes < 250) {
        new_chart->Meta.Difficulty = 1;
      } else if (totalNotes < 600) {
        new_chart->Meta.Difficulty = 2;
      } else if (totalNotes < 1000) {
        new_chart->Meta.Difficulty = 3;
      } else if (totalNotes < 2000) {
        new_chart->Meta.Difficulty = 4;
      } else {
        new_chart->Meta.Difficulty = 5;
      }
    }
  }

#if BMS_PARSER_VERBOSE == 1
  std::cout << "Total parsing time: "
            << std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::high_resolution_clock::now() - startTime)
                   .count()
            << "\n";
#endif
}

void Parser::ParseHeader(Chart *Chart, std::string_view cmd,
                         std::string_view Xx, const std::string &Value) {
  // Debug.Log($"cmd: {cmd}, xx: {xx} isXXNull: {xx == null}, value: {value}");
  // BASE 62
  if (MatchHeader(cmd, "BASE")) {
    if (Value.empty()) {
      return; // TODO: handle this
    }
    auto base = static_cast<int>(std::strtol(Value.c_str(), nullptr, 10));
    std::wcout << "BASE: " << base << std::endl;
    if (base != 36 && base != 62) {
      return; // TODO: handle this
    }
    this->UseBase62 = base == 62;
  } else if (MatchHeader(cmd, "4K")) {
    Chart->Meta.KeyMode = 4;
    Chart->Meta.IsDP = false;
  } else if (MatchHeader(cmd, "6K")) {
    Chart->Meta.KeyMode = 6;
    Chart->Meta.IsDP = false;
  } else if (MatchHeader(cmd, "8K")) {
    Chart->Meta.KeyMode = 8;
    Chart->Meta.IsDP = false;
  } else if (MatchHeader(cmd, "PLAYER")) {
    Chart->Meta.Player =
        static_cast<int>(std::strtol(Value.c_str(), nullptr, 10));
  } else if (MatchHeader(cmd, "GENRE")) {
    Chart->Meta.Genre = Value;
  } else if (MatchHeader(cmd, "TITLE")) {
    Chart->Meta.Title = Value;
  } else if (MatchHeader(cmd, "SUBTITLE")) {
    Chart->Meta.SubTitle = Value;
  } else if (MatchHeader(cmd, "ARTIST")) {
    Chart->Meta.Artist = Value;
  } else if (MatchHeader(cmd, "SUBARTIST")) {
    Chart->Meta.SubArtist = Value;
  } else if (MatchHeader(cmd, "DIFFICULTY")) {
    Chart->Meta.Difficulty =
        static_cast<int>(std::strtol(Value.c_str(), nullptr, 10));
  } else if (MatchHeader(cmd, "BPM")) {
    if (Value.empty()) {
      return; // TODO: handle this
    }
    if (Xx.empty()) {
      // chart initial bpm
      Chart->Meta.Bpm = std::strtod(Value.c_str(), nullptr);
      // std::cout << "MainBPM: " << Chart->Meta.Bpm << std::endl;
    } else {
      // Debug.Log($"BPM: {DecodeBase36(xx)} = {double.Parse(value)}");
      int id = ParseInt(Xx);
      if (!CheckResourceIdRange(id)) {
        // UE_LOG(LogTemp, Warning, TEXT("Invalid BPM id: %s"), *Xx);
        return;
      }
      BpmTable[id] = std::strtod(Value.c_str(), nullptr);
    }
  } else if (MatchHeader(cmd, "STOP")) {
    if (Value.empty() || Xx.empty()) {
      return; // TODO: handle this
    }
    int id = ParseInt(Xx);
    if (!CheckResourceIdRange(id)) {
      // UE_LOG(LogTemp, Warning, TEXT("Invalid STOP id: %s"), *Xx);
      return;
    }
    StopLengthTable[id] = std::strtod(Value.c_str(), nullptr);
  } else if (MatchHeader(cmd, "MIDIFILE")) {
    // TODO: handle this
  } else if (MatchHeader(cmd, "VIDEOFILE")) {
  } else if (MatchHeader(cmd, "PLAYLEVEL")) {
    Chart->Meta.PlayLevel =
        std::strtod(Value.c_str(), nullptr); // TODO: handle error
  } else if (MatchHeader(cmd, "RANK")) {
    Chart->Meta.Rank =
        static_cast<int>(std::strtol(Value.c_str(), nullptr, 10));
  } else if (MatchHeader(cmd, "TOTAL")) {
    auto total = std::strtod(Value.c_str(), nullptr);
    if (total > 0) {
      Chart->Meta.Total = total;
    }
  } else if (MatchHeader(cmd, "VOLWAV")) {
  } else if (MatchHeader(cmd, "STAGEFILE")) {
    Chart->Meta.StageFile = utf8_to_path_t(Value);
  } else if (MatchHeader(cmd, "BANNER")) {
    Chart->Meta.Banner = utf8_to_path_t(Value);
  } else if (MatchHeader(cmd, "BACKBMP")) {
    Chart->Meta.BackBmp = utf8_to_path_t(Value);
  } else if (MatchHeader(cmd, "PREVIEW")) {
    Chart->Meta.Preview = utf8_to_path_t(Value);
  } else if (MatchHeader(cmd, "WAV")) {
    if (Xx.empty() || Value.empty()) {
      // UE_LOG(LogTemp, Warning, TEXT("WAV command requires two arguments"));
      return;
    }
    int id = ParseInt(Xx);
    if (!CheckResourceIdRange(id)) {
      // UE_LOG(LogTemp, Warning, TEXT("Invalid WAV id: %s"), *Xx);
      return;
    }
    Chart->WavTable[id] = Value;
  } else if (MatchHeader(cmd, "BMP")) {
    if (Xx.empty() || Value.empty()) {
      // UE_LOG(LogTemp, Warning, TEXT("BMP command requires two arguments"));
      return;
    }
    int id = ParseInt(Xx);
    if (!CheckResourceIdRange(id)) {
      // UE_LOG(LogTemp, Warning, TEXT("Invalid BMP id: %s"), *Xx);
      return;
    }
    Chart->BmpTable[id] = Value;
    if (Xx == "00") {
      Chart->Meta.BgaPoorDefault = true;
    }
  } else if (MatchHeader(cmd, "LNOBJ")) {
    Lnobj = ParseInt(Value);
  } else if (MatchHeader(cmd, "LNTYPE")) {
    Lntype = static_cast<int>(std::strtol(Value.c_str(), nullptr, 10));
  } else if (MatchHeader(cmd, "LNMODE")) {
    Chart->Meta.LnMode =
        static_cast<int>(std::strtol(Value.c_str(), nullptr, 10));
  } else if (MatchHeader(cmd, "SCROLL")) {
    auto xx = ParseInt(Xx);
    auto value = std::strtod(Value.c_str(), nullptr);
    ScrollTable[xx] = value;
    // std::wcout << "SCROLL: " << xx << " = " << value << std::endl;
  } else {
#if BMS_PARSER_VERBOSE == 1
    std::cout << "Unknown command: " << cmd << std::endl;
#endif
  }
}

inline unsigned long long Parser::Gcd(unsigned long long A,
                                      unsigned long long B) {
  while (true) {
    if (B == 0) {
      return A;
    }
    auto a1 = A;
    A = B;
    B = a1 % B;
  }
}

inline bool Parser::CheckResourceIdRange(int Id) const {
  return Id >= 0 && Id < (UseBase62 ? 62 * 62 : 36 * 36);
}

inline int Parser::ToWaveId(Chart *Chart, std::string_view Wav, bool metaOnly) {
  if (metaOnly) {
    return NoWav;
  }
  if (Wav.empty()) {
    return NoWav;
  }
  auto decoded = ParseInt(Wav);
  // check range
  if (!CheckResourceIdRange(decoded)) {
    // UE_LOG(LogTemp, Warning, TEXT("Invalid wav id: %s"), *Wav);
    return NoWav;
  }

  return Chart->WavTable.find(decoded) != Chart->WavTable.end() ? decoded
                                                                : NoWav;
}

inline int Parser::ParseHex(std::string_view Str) {
  auto result = 0;
  for (size_t i = 0; i < Str.length(); ++i) {
    auto c = Str[i];
    if (c >= '0' && c <= '9') {
      result = result * 16 + c - '0';
    } else if (c >= 'A' && c <= 'F') {
      result = result * 16 + c - 'A' + 10;
    } else if (c >= 'a' && c <= 'f') {
      result = result * 16 + c - 'a' + 10;
    }
  }
  return result;
}

inline int Parser::ParseInt(std::string_view Str, bool forceBase36) const {
  if (forceBase36 || !UseBase62) {
    auto result = static_cast<int>(std::strtol(Str.data(), nullptr, 36));
    // std::wcout << "ParseInt36: " << Str << " = " << result << std::endl;
    return result;
  }

  auto result = 0;
  for (size_t i = 0; i < Str.length(); ++i) {
    auto c = Str[i];
    if (c >= '0' && c <= '9') {
      result = result * 62 + c - '0';
    } else if (c >= 'A' && c <= 'Z') {
      result = result * 62 + c - 'A' + 10;
    } else if (c >= 'a' && c <= 'z') {
      result = result * 62 + c - 'a' + 36;
    } else
      return -1;
  }
  // std::wcout << "ParseInt62: " << Str << " = " << result << std::endl;
  return result;
}
#ifdef _WIN32
std::wstring Parser::utf8_to_path_t(const std::string &input) {

  // Determine the size of the buffer needed
  int requiredSize =
      MultiByteToWideChar(65001 /* UTF8 */, 0, input.c_str(), -1, NULL, 0);

  if (requiredSize <= 0) {
    // Conversion failed, return an empty string
    return std::wstring();
  }

  // Create a string with the required size
  std::wstring result(requiredSize, '\0');

  // Perform the conversion
  MultiByteToWideChar(65001 /* UTF8 */, 0, input.c_str(), -1, &result[0],
                      requiredSize);

  // Remove the extra null terminator added by MultiByteToWideChar
  result.resize(requiredSize - 1);

  return result;
}
#else
std::string Parser::utf8_to_path_t(const std::string &input) { return input; }
#endif

Parser::~Parser() = default;
} // namespace bms_parser

/*
 * Updated to C++, zedwood.com 2012
 * Based on Olivier Gay's version
 * See Modified BSD License below:
 *
 * FIPS 180-2 SHA-224/256/384/512 implementation
 * Issue date:  04/30/2005
 * http://www.ouah.org/ogay/sha2/
 *
 * Copyright (C) 2005, 2007 Olivier Gay <olivier.gay@a3.epfl.ch>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

// http://www.zedwood.com/article/cpp-sha256-function
#include <cstring>

namespace bms_parser {
const unsigned int SHA256::sha256_k[64] = // UL = uint32
    {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
     0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
     0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
     0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
     0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
     0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
     0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
     0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
     0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
     0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
     0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void SHA256::transform(const unsigned char *message, unsigned int block_nb) {
  uint32 w[64];
  uint32 wv[8];
  uint32 t1, t2;
  const unsigned char *sub_block;
  int i;
  int j;
  for (i = 0; i < static_cast<int>(block_nb); i++) {
    sub_block = message + (i << 6);
    for (j = 0; j < 16; j++) {
      SHA2_PACK32(&sub_block[j << 2], &w[j]);
    }
    for (j = 16; j < 64; j++) {
      w[j] = SHA256_F4(w[j - 2]) + w[j - 7] + SHA256_F3(w[j - 15]) + w[j - 16];
    }
    for (j = 0; j < 8; j++) {
      wv[j] = m_h[j];
    }
    for (j = 0; j < 64; j++) {
      t1 = wv[7] + SHA256_F2(wv[4]) + SHA2_CH(wv[4], wv[5], wv[6]) +
           sha256_k[j] + w[j];
      t2 = SHA256_F1(wv[0]) + SHA2_MAJ(wv[0], wv[1], wv[2]);
      wv[7] = wv[6];
      wv[6] = wv[5];
      wv[5] = wv[4];
      wv[4] = wv[3] + t1;
      wv[3] = wv[2];
      wv[2] = wv[1];
      wv[1] = wv[0];
      wv[0] = t1 + t2;
    }
    for (j = 0; j < 8; j++) {
      m_h[j] += wv[j];
    }
  }
}

void SHA256::init() {
  m_h[0] = 0x6a09e667;
  m_h[1] = 0xbb67ae85;
  m_h[2] = 0x3c6ef372;
  m_h[3] = 0xa54ff53a;
  m_h[4] = 0x510e527f;
  m_h[5] = 0x9b05688c;
  m_h[6] = 0x1f83d9ab;
  m_h[7] = 0x5be0cd19;
  m_len = 0;
  m_tot_len = 0;
}

void SHA256::update(const unsigned char *message, unsigned int len) {
  unsigned int block_nb;
  unsigned int new_len, rem_len, tmp_len;
  const unsigned char *shifted_message;
  tmp_len = SHA224_256_BLOCK_SIZE - m_len;
  rem_len = len < tmp_len ? len : tmp_len;
  memcpy(&m_block[m_len], message, rem_len);
  if (m_len + len < SHA224_256_BLOCK_SIZE) {
    m_len += len;
    return;
  }
  new_len = len - rem_len;
  block_nb = new_len / SHA224_256_BLOCK_SIZE;
  shifted_message = message + rem_len;
  transform(m_block, 1);
  transform(shifted_message, block_nb);
  rem_len = new_len % SHA224_256_BLOCK_SIZE;
  memcpy(m_block, &shifted_message[block_nb << 6], rem_len);
  m_len = rem_len;
  m_tot_len += (block_nb + 1) << 6;
}

void SHA256::final(unsigned char *digest) {
  unsigned int block_nb;
  unsigned int pm_len;
  unsigned int len_b;
  int i;
  block_nb =
      (1 + ((SHA224_256_BLOCK_SIZE - 9) < (m_len % SHA224_256_BLOCK_SIZE)));
  len_b = (m_tot_len + m_len) << 3;
  pm_len = block_nb << 6;
  memset(m_block + m_len, 0, pm_len - m_len);
  m_block[m_len] = 0x80;
  SHA2_UNPACK32(len_b, m_block + pm_len - 4);
  transform(m_block, block_nb);
  for (i = 0; i < 8; i++) {
    SHA2_UNPACK32(m_h[i], &digest[i << 2]);
  }
}

std::string sha256(const std::vector<unsigned char> &bytes) {
  unsigned char digest[SHA256::DIGEST_SIZE];
  memset(digest, 0, SHA256::DIGEST_SIZE);

  SHA256 ctx = SHA256();
  ctx.init();
  ctx.update(bytes.data(), bytes.size());
  ctx.final(digest);

  char buf[2 * SHA256::DIGEST_SIZE + 1];
  buf[2 * SHA256::DIGEST_SIZE] = 0;
  for (unsigned int i = 0; i < SHA256::DIGEST_SIZE; i++) {
    snprintf(buf + i * 2, 3, "%02x", digest[i]);
  }
  return buf;
}
} // namespace bms_parser

// https://stackoverflow.com/questions/33165171/c-shiftjis-to-utf8-conversion

#include <cstdint>
#include <string>

namespace bms_parser {
void ShiftJISConverter::BytesToUTF8(const unsigned char *input, size_t size,
                                    std::string &result) {
  // ShiftJis won't give 4byte UTF8, so max. 3 byte per input char are needed
  result.resize(size * 3, ' ');
  size_t indexInput = 0, indexOutput = 0;

  while (indexInput < size) {
    char arraySection = (input[indexInput]) >> 4;

    size_t arrayOffset;
    if (arraySection == 0x8) {
      arrayOffset = 0x100; // these are two-byte shiftjis
    } else if (arraySection == 0x9) {
      arrayOffset = 0x1100;
    } else if (arraySection == 0xE) {
      arrayOffset = 0x2100;
    } else {
      arrayOffset = 0; // this is one byte shiftjis
    }

    // determining real array offset
    if (arrayOffset) {
      arrayOffset += ((input[indexInput]) & 0xf) << 8;
      indexInput++;
      if (indexInput >= size) {
        break;
      }
    }
    arrayOffset += input[indexInput++];
    arrayOffset <<= 1;

    // unicode number is...
    uint16_t unicodeValue = (shiftJIS_convTable[arrayOffset] << 8) |
                            shiftJIS_convTable[arrayOffset + 1];

    // converting to UTF8
    if (unicodeValue < 0x80) {
      result[indexOutput++] = unicodeValue;
    } else if (unicodeValue < 0x800) {
      result[indexOutput++] = 0xC0 | (unicodeValue >> 6);
      result[indexOutput++] = 0x80 | (unicodeValue & 0x3f);
    } else {
      result[indexOutput++] = 0xE0 | (unicodeValue >> 12);
      result[indexOutput++] = 0x80 | ((unicodeValue & 0xfff) >> 6);
      result[indexOutput++] = 0x80 | (unicodeValue & 0x3f);
    }
  }

  result.resize(indexOutput);
}
} // namespace bms_parser

/*
 * Copyright (C) 2024 VioletXF, khoeun03
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

namespace bms_parser {
TimeLine::TimeLine(int lanes, bool metaOnly) {
  if (metaOnly) {
    return;
  }
  Notes.resize(lanes, nullptr);
  InvisibleNotes.resize(lanes, nullptr);
  LandmineNotes.resize(lanes, nullptr);
}

TimeLine *TimeLine::SetNote(int lane, Note *note) {
  Notes[lane] = note;
  note->Lane = lane;
  note->Timeline = this;
  return this;
}

TimeLine *TimeLine::SetInvisibleNote(int lane, Note *note) {
  InvisibleNotes[lane] = note;
  note->Lane = lane;
  note->Timeline = this;
  return this;
}

TimeLine *TimeLine::SetLandmineNote(int lane, LandmineNote *note) {
  LandmineNotes[lane] = note;
  note->Lane = lane;
  note->Timeline = this;
  return this;
}

TimeLine *TimeLine::AddBackgroundNote(Note *note) {
  BackgroundNotes.push_back(note);
  note->Timeline = this;
  return this;
}

double TimeLine::GetStopDuration() const {
  return 1250000.0 * StopLength / Bpm; // 1250000 = 240 * 1000 * 1000 / 192
}

TimeLine::~TimeLine() {
  for (const auto &note : Notes) {
    delete note;
  }
  Notes.clear();
  for (const auto &note : InvisibleNotes) {
    delete note;
  }
  InvisibleNotes.clear();
  for (const auto &note : LandmineNotes) {
    delete note;
  }
  LandmineNotes.clear();
  for (const auto &note : BackgroundNotes) {
    delete note;
  }
  BackgroundNotes.clear();
}
} // namespace bms_parser

/* MD5
 converted to C++ class by Frank Thilo (thilo@unix-ag.org)
 for bzflag (http://www.bzflag.org)

   based on:

   md5.h and md5.c
   reference implemantion of RFC 1321

   Copyright (C) 1991-2, RSA Data Security, Inc. Created 1991. All
rights reserved.

License to copy and use this software is granted provided that it
is identified as the "RSA Data Security, Inc. MD5 Message-Digest
Algorithm" in all material mentioning or referencing this software
or this function.

License is also granted to make and use derivative works provided
that such works are identified as "derived from the RSA Data
Security, Inc. MD5 Message-Digest Algorithm" in all material
mentioning or referencing the derived work.

RSA Data Security, Inc. makes no representations concerning either
the merchantability of this software or the suitability of this
software for any particular purpose. It is provided "as is"
without express or implied warranty of any kind.

These notices must be retained in any copies of any part of this
documentation and/or software.

*/

/* interface header */

/* system implementation headers */
#include <cstdio>

// Constants for MD5Transform routine.
#define S11 7
#define S12 12
#define S13 17
#define S14 22
#define S21 5
#define S22 9
#define S23 14
#define S24 20
#define S31 4
#define S32 11
#define S33 16
#define S34 23
#define S41 6
#define S42 10
#define S43 15
#define S44 21

///////////////////////////////////////////////
namespace bms_parser {
// F, G, H and I are basic MD5 functions.
inline MD5::uint4 MD5::F(uint4 x, uint4 y, uint4 z) {
  return (x & y) | (~x & z);
}

inline MD5::uint4 MD5::G(uint4 x, uint4 y, uint4 z) {
  return (x & z) | (y & ~z);
}

inline MD5::uint4 MD5::H(uint4 x, uint4 y, uint4 z) { return x ^ y ^ z; }

inline MD5::uint4 MD5::I(uint4 x, uint4 y, uint4 z) { return y ^ (x | ~z); }

// rotate_left rotates x left n bits.
inline MD5::uint4 MD5::rotate_left(uint4 x, int n) {
  return (x << n) | (x >> (32 - n));
}

// FF, GG, HH, and II transformations for rounds 1, 2, 3, and 4.
// Rotation is separate from addition to prevent recomputation.
inline void MD5::FF(uint4 &a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s,
                    uint4 ac) {
  a = rotate_left(a + F(b, c, d) + x + ac, s) + b;
}

inline void MD5::GG(uint4 &a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s,
                    uint4 ac) {
  a = rotate_left(a + G(b, c, d) + x + ac, s) + b;
}

inline void MD5::HH(uint4 &a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s,
                    uint4 ac) {
  a = rotate_left(a + H(b, c, d) + x + ac, s) + b;
}

inline void MD5::II(uint4 &a, uint4 b, uint4 c, uint4 d, uint4 x, uint4 s,
                    uint4 ac) {
  a = rotate_left(a + I(b, c, d) + x + ac, s) + b;
}

//////////////////////////////////////////////

// default ctor, just initailize
MD5::MD5() { init(); }

//////////////////////////////////////////////

// nifty shortcut ctor, compute MD5 for string and finalize it right away
MD5::MD5(const std::string &text) {
  init();
  update(text.c_str(), text.length());
  finalize();
}

//////////////////////////////

void MD5::init() {
  finalized = false;

  count[0] = 0;
  count[1] = 0;

  // load magic initialization constants.
  state[0] = 0x67452301;
  state[1] = 0xefcdab89;
  state[2] = 0x98badcfe;
  state[3] = 0x10325476;
}

//////////////////////////////

// decodes input (unsigned char) into output (uint4). Assumes len is a multiple
// of 4.
void MD5::decode(uint4 output[], const uint1 input[], size_type len) {
  for (unsigned int i = 0, j = 0; j < len; i++, j += 4)
    output[i] = ((uint4)input[j]) | (((uint4)input[j + 1]) << 8) |
                (((uint4)input[j + 2]) << 16) | (((uint4)input[j + 3]) << 24);
}

//////////////////////////////

// encodes input (uint4) into output (unsigned char). Assumes len is
// a multiple of 4.
void MD5::encode(uint1 output[], const uint4 input[], size_type len) {
  for (size_type i = 0, j = 0; j < len; i++, j += 4) {
    output[j] = input[i] & 0xff;
    output[j + 1] = (input[i] >> 8) & 0xff;
    output[j + 2] = (input[i] >> 16) & 0xff;
    output[j + 3] = (input[i] >> 24) & 0xff;
  }
}

//////////////////////////////

// apply MD5 algo on a block
void MD5::transform(const uint1 block[blocksize]) {
  uint4 a = state[0], b = state[1], c = state[2], d = state[3], x[16];
  decode(x, block, blocksize);

  /* Round 1 */
  FF(a, b, c, d, x[0], S11, 0xd76aa478);  /* 1 */
  FF(d, a, b, c, x[1], S12, 0xe8c7b756);  /* 2 */
  FF(c, d, a, b, x[2], S13, 0x242070db);  /* 3 */
  FF(b, c, d, a, x[3], S14, 0xc1bdceee);  /* 4 */
  FF(a, b, c, d, x[4], S11, 0xf57c0faf);  /* 5 */
  FF(d, a, b, c, x[5], S12, 0x4787c62a);  /* 6 */
  FF(c, d, a, b, x[6], S13, 0xa8304613);  /* 7 */
  FF(b, c, d, a, x[7], S14, 0xfd469501);  /* 8 */
  FF(a, b, c, d, x[8], S11, 0x698098d8);  /* 9 */
  FF(d, a, b, c, x[9], S12, 0x8b44f7af);  /* 10 */
  FF(c, d, a, b, x[10], S13, 0xffff5bb1); /* 11 */
  FF(b, c, d, a, x[11], S14, 0x895cd7be); /* 12 */
  FF(a, b, c, d, x[12], S11, 0x6b901122); /* 13 */
  FF(d, a, b, c, x[13], S12, 0xfd987193); /* 14 */
  FF(c, d, a, b, x[14], S13, 0xa679438e); /* 15 */
  FF(b, c, d, a, x[15], S14, 0x49b40821); /* 16 */

  /* Round 2 */
  GG(a, b, c, d, x[1], S21, 0xf61e2562);  /* 17 */
  GG(d, a, b, c, x[6], S22, 0xc040b340);  /* 18 */
  GG(c, d, a, b, x[11], S23, 0x265e5a51); /* 19 */
  GG(b, c, d, a, x[0], S24, 0xe9b6c7aa);  /* 20 */
  GG(a, b, c, d, x[5], S21, 0xd62f105d);  /* 21 */
  GG(d, a, b, c, x[10], S22, 0x2441453);  /* 22 */
  GG(c, d, a, b, x[15], S23, 0xd8a1e681); /* 23 */
  GG(b, c, d, a, x[4], S24, 0xe7d3fbc8);  /* 24 */
  GG(a, b, c, d, x[9], S21, 0x21e1cde6);  /* 25 */
  GG(d, a, b, c, x[14], S22, 0xc33707d6); /* 26 */
  GG(c, d, a, b, x[3], S23, 0xf4d50d87);  /* 27 */
  GG(b, c, d, a, x[8], S24, 0x455a14ed);  /* 28 */
  GG(a, b, c, d, x[13], S21, 0xa9e3e905); /* 29 */
  GG(d, a, b, c, x[2], S22, 0xfcefa3f8);  /* 30 */
  GG(c, d, a, b, x[7], S23, 0x676f02d9);  /* 31 */
  GG(b, c, d, a, x[12], S24, 0x8d2a4c8a); /* 32 */

  /* Round 3 */
  HH(a, b, c, d, x[5], S31, 0xfffa3942);  /* 33 */
  HH(d, a, b, c, x[8], S32, 0x8771f681);  /* 34 */
  HH(c, d, a, b, x[11], S33, 0x6d9d6122); /* 35 */
  HH(b, c, d, a, x[14], S34, 0xfde5380c); /* 36 */
  HH(a, b, c, d, x[1], S31, 0xa4beea44);  /* 37 */
  HH(d, a, b, c, x[4], S32, 0x4bdecfa9);  /* 38 */
  HH(c, d, a, b, x[7], S33, 0xf6bb4b60);  /* 39 */
  HH(b, c, d, a, x[10], S34, 0xbebfbc70); /* 40 */
  HH(a, b, c, d, x[13], S31, 0x289b7ec6); /* 41 */
  HH(d, a, b, c, x[0], S32, 0xeaa127fa);  /* 42 */
  HH(c, d, a, b, x[3], S33, 0xd4ef3085);  /* 43 */
  HH(b, c, d, a, x[6], S34, 0x4881d05);   /* 44 */
  HH(a, b, c, d, x[9], S31, 0xd9d4d039);  /* 45 */
  HH(d, a, b, c, x[12], S32, 0xe6db99e5); /* 46 */
  HH(c, d, a, b, x[15], S33, 0x1fa27cf8); /* 47 */
  HH(b, c, d, a, x[2], S34, 0xc4ac5665);  /* 48 */

  /* Round 4 */
  II(a, b, c, d, x[0], S41, 0xf4292244);  /* 49 */
  II(d, a, b, c, x[7], S42, 0x432aff97);  /* 50 */
  II(c, d, a, b, x[14], S43, 0xab9423a7); /* 51 */
  II(b, c, d, a, x[5], S44, 0xfc93a039);  /* 52 */
  II(a, b, c, d, x[12], S41, 0x655b59c3); /* 53 */
  II(d, a, b, c, x[3], S42, 0x8f0ccc92);  /* 54 */
  II(c, d, a, b, x[10], S43, 0xffeff47d); /* 55 */
  II(b, c, d, a, x[1], S44, 0x85845dd1);  /* 56 */
  II(a, b, c, d, x[8], S41, 0x6fa87e4f);  /* 57 */
  II(d, a, b, c, x[15], S42, 0xfe2ce6e0); /* 58 */
  II(c, d, a, b, x[6], S43, 0xa3014314);  /* 59 */
  II(b, c, d, a, x[13], S44, 0x4e0811a1); /* 60 */
  II(a, b, c, d, x[4], S41, 0xf7537e82);  /* 61 */
  II(d, a, b, c, x[11], S42, 0xbd3af235); /* 62 */
  II(c, d, a, b, x[2], S43, 0x2ad7d2bb);  /* 63 */
  II(b, c, d, a, x[9], S44, 0xeb86d391);  /* 64 */

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;

  // Zeroize sensitive information.
  memset(x, 0, sizeof x);
}

//////////////////////////////

// MD5 block update operation. Continues an MD5 message-digest
// operation, processing another message block
void MD5::update(const unsigned char input[], size_type length) {
  // compute number of bytes mod 64
  size_type index = count[0] / 8 % blocksize;

  // Update number of bits
  if ((count[0] += (length << 3)) < (length << 3))
    count[1]++;
  count[1] += (length >> 29);

  // number of bytes we need to fill in buffer
  size_type firstpart = 64 - index;

  size_type i;

  // transform as many times as possible.
  if (length >= firstpart) {
    // fill buffer first, transform
    memcpy(&buffer[index], input, firstpart);
    transform(buffer);

    // transform chunks of blocksize (64 bytes)
    for (i = firstpart; i + blocksize <= length; i += blocksize)
      transform(&input[i]);

    index = 0;
  } else
    i = 0;

  // buffer remaining input
  memcpy(&buffer[index], &input[i], length - i);
}

//////////////////////////////

// for convenience provide a verson with signed char
void MD5::update(const char input[], size_type length) {
  update((const unsigned char *)input, length);
}

//////////////////////////////

// MD5 finalization. Ends an MD5 message-digest operation, writing the
// the message digest and zeroizing the context.
MD5 &MD5::finalize() {
  static unsigned char padding[64] = {
      0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  if (!finalized) {
    // Save number of bits
    unsigned char bits[8];
    encode(bits, count, 8);

    // pad out to 56 mod 64.
    size_type index = count[0] / 8 % 64;
    size_type padLen = (index < 56) ? (56 - index) : (120 - index);
    update(padding, padLen);

    // Append length (before padding)
    update(bits, 8);

    // Store state in digest
    encode(digest, state, 16);

    // Zeroize sensitive information.
    memset(buffer, 0, sizeof buffer);
    memset(count, 0, sizeof count);

    finalized = true;
  }

  return *this;
}

//////////////////////////////

// return hex representation of digest as string
std::string MD5::hexdigest() const {
  if (!finalized)
    return "";

  char buf[33];
  for (int i = 0; i < 16; i++)
    snprintf(buf + i * 2, 3, "%02x", digest[i]);
  buf[32] = 0;

  return buf;
}

//////////////////////////////

std::ostream &operator<<(std::ostream &out, MD5 md5) {
  return out << md5.hexdigest();
}

//////////////////////////////

std::string md5(const std::string str) {
  MD5 md5 = MD5(str);

  return md5.hexdigest();
}
} // namespace bms_parser
