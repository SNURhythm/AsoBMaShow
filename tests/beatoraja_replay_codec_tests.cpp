#include "replay/Base64Url.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/GzipCodec.h"
#include "replay/ReplaySetupAdapter.h"

#include "PlayOptionUtils.h"
#include "bms_parser.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<std::byte>;
using Json = nlohmann::ordered_json;
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

Bytes bytes(std::string_view value) {
  Bytes output(value.size());
  std::transform(value.begin(), value.end(), output.begin(),
                 [](char ch) { return static_cast<std::byte>(ch); });
  return output;
}

std::string string(std::span<const std::byte> value) {
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

Bytes readFixture(std::string_view name) {
  const auto path = std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "tests" /
                    "fixtures" / "replay" / name;
  std::ifstream stream(path, std::ios::binary);
  expect(stream.good(), "golden replay fixture opens");
  const std::vector<char> raw(std::istreambuf_iterator<char>(stream), {});
  Bytes output(raw.size());
  std::transform(raw.begin(), raw.end(), output.begin(),
                 [](char value) { return static_cast<std::byte>(value); });
  return output;
}

Json outerJson(const Bytes &encoded) {
  std::string diagnostic;
  const auto decoded = replay::gzipDecompressBounded(
      encoded, replay::kReplayLimits.maxJsonBytes, diagnostic);
  expect(decoded.has_value(), "test BRD outer gzip decompresses");
  return decoded ? Json::parse(string(*decoded)) : Json{};
}

Bytes encodeJson(const Json &document) {
  const std::string source = document.dump();
  std::string diagnostic;
  const auto encoded = replay::gzipCompress(bytes(source), diagnostic);
  expect(encoded.has_value(), "test JSON gzip compression succeeds");
  return encoded.value_or(Bytes{});
}

Bytes stockKeyRecord(int signedKeyCode, std::int64_t songTimeMicros) {
  Bytes output{static_cast<std::byte>(static_cast<std::uint8_t>(
      static_cast<std::int8_t>(signedKeyCode)))};
  const auto rawTime = static_cast<std::uint64_t>(songTimeMicros);
  for (int shift = 0; shift < 64; shift += 8) {
    output.push_back(static_cast<std::byte>((rawTime >> shift) & 0xffU));
  }
  return output;
}

replay::ReplaySetup setup(std::string sha = std::string(64, 'a'),
                          int keyMode = 7) {
  replay::ReplaySetup value;
  value.chart = {.md5 = std::string(32, 'b'),
                 .sha256 = std::move(sha),
                 .keyMode = keyMode};
  value.longNoteMode = 2;
  value.hasUndefinedLongNotes = true;
  value.chartRandomSeed = 17;
  value.chartRandomPrng = bms_parser::Parser::RandomPrngId;
  value.chartRandomValues = {3, 1, 4};
  value.player1 = {.option = "R-RANDOM",
                   .seed = 0x123456,
                   .laneShufflePattern =
                       std::vector<int>{2, 0, 4, 5, 6, 3, 1, 7}};
  value.player2 = {.option = "NORMAL", .seed = 0x234567};
  value.initialGaugeType = GaugeType::ExHard;
  value.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  value.gaugeAutoShiftLowerBound = GaugeType::Easy;
  value.ruleset = RulesetDescriptor::Current();
  value.playback = {.percent = 125, .mode = audio::PlaybackMode::TimeStretch};
  value.candidateSelection = gameplay::CandidateSelectionMode::Score;
  value.judgeWindowScalePercent = 90;
  value.startingGaugePercent = 42.5F;
  value.initialLaneCoverPercent = 37;
  value.laneCoverEnabled = true;
  value.clubMode = true;
  return value;
}

replay::ReplayChartDocument chartDocument() {
  replay::ReplayChartDocument document;
  document.timeBounds = {.completionSongTimeMicros = 8'000'000};
  document.playback.setup = setup();
  document.playback.input = {
      {.songTimeMicros = -2'000'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 1'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
      {.songTimeMicros = 2'000,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
      {.songTimeMicros = 2'500,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false},
  };
  document.playback.touchSamples = {
      {.action = replay::ReplayTouchAction::Down,
       .fingerId = 7,
       .songTimeMicros = -1'000'000,
       .x = 0.25F,
       .y = 0.75F},
      {.action = replay::ReplayTouchAction::Up,
       .fingerId = 7,
       .songTimeMicros = 3'000,
       .x = 0.5F,
       .y = 0.25F},
  };
  document.playback.laneCoverEvents = {
      {.songTimeMicros = -1'500'000, .noteStartPositionPercent = 41},
      {.songTimeMicros = 4'000,
       .noteStartPositionPercent = 53,
       .resetVisibleTimeReference = true},
  };
  return document;
}

replay::ReplayDecodeContext context(const replay::ReplayChartDocument &chart) {
  return {.stageKeyModes = {chart.playback.setup.chart.keyMode},
          .stageTimeBounds = {chart.timeBounds}};
}

void testPopnFiveKeyStockShuffleSurvivesChartApplication() {
  const std::string bmson = R"({"version":"1.0.0","info":{"mode_hint":"popn-5k","init_bpm":120},"sound_channels":[
    {"name":"1.wav","notes":[{"x":1,"y":240}]},
    {"name":"2.wav","notes":[{"x":2,"y":240}]},
    {"name":"3.wav","notes":[{"x":3,"y":240}]},
    {"name":"4.wav","notes":[{"x":4,"y":240}]},
    {"name":"5.wav","notes":[{"x":5,"y":240}]}]})";
  bms_parser::Parser parser;
  bms_parser::Chart *raw = nullptr;
  std::atomic_bool cancelled{false};
  parser.Parse(std::vector<unsigned char>(bmson.begin(), bmson.end()), &raw,
               false, false, cancelled, "popn-five.bmson");
  std::unique_ptr<bms_parser::Chart> chart(raw);
  expect(chart && chart->Meta.IsPopnFiveKey(), "pop'n replay fixture parses with five active lanes");
  if (!chart) return;

  auto source = chartDocument();
  source.playback.setup.chart = {.md5 = chart->Meta.MD5,
                                  .sha256 = chart->Meta.SHA256, .keyMode = 9};
  source.playback.setup.player1 = {.option = "RANDOM", .seed = 0};
  source.playback.setup.player2 = {};
  source.playback.input.resize(2);
  replay::BeatorajaReplayCodec codec;
  std::string diagnostic;
  const auto encoded = codec.encodeChart(source, 0, diagnostic);
  expect(encoded.has_value(), "pop'n seed-only source encodes");
  if (!encoded) return;
  auto stock = outerJson(*encoded);
  stock.erase("asobmashow");
  // LaneRandomShuffleModifier, seed 0, at Beatoraja 83574048ba48.
  const std::vector<int> pattern{0, 4, 2, 3, 1};
  stock["laneShufflePattern"] = Json::array({pattern});
  auto decoded = codec.decode(encodeJson(stock), context(source));
  expect(decoded.chart && decoded.chart->playback.setup.player1.laneShufflePattern == pattern,
         "stock pop'n 5K imports its five-entry shuffle under mode identity 9");
  if (!decoded.chart) return;
  const auto translated = replay::makeReplayDataFromSetup(
      decoded.chart->playback.setup, ScoreProvenance::Legacy(), chart->Meta, diagnostic);
  expect(translated && translated->chartMeta.IsPopnFiveKey() &&
             translated->laneShufflePattern1P == pattern,
         "setup adapter retains the parsed five-lane layout and imported pattern");
  if (!translated) return;
  bms_parser::TimeLine *notes = nullptr;
  for (auto *measure : chart->Measures)
    for (auto *timeline : measure->TimeLines)
      if (timeline->Notes[0]) notes = timeline;
  expect(notes != nullptr, "pop'n fixture has its playable chord");
  if (!notes) return;
  std::vector<int> originalSounds;
  for (int lane = 0; lane < 5; ++lane) originalSounds.push_back(notes->Notes[lane]->Wav);
  expect(play_options::applyReplayPlayOptions(*chart, *translated),
         "imported pop'n setup applies through the production replay modifier path");
  expect(chart->Meta.GetTotalLaneIndices() == std::vector<int>({0, 1, 2, 3, 4}) &&
             chart->Meta.TotalNotes == 5 &&
             std::all_of(notes->Notes.begin() + 5, notes->Notes.end(),
                         [](const auto *note) { return note == nullptr; }),
         "replay randomization does not expand pop'n 5K into nine lanes");
  for (int lane = 0; lane < 5; ++lane)
    expect(notes->Notes[lane] && notes->Notes[lane]->Lane == lane &&
               notes->Notes[lane]->Wav == originalSounds[pattern[lane]],
           "pop'n RANDOM seed reproduces the Java five-lane source assignment");

  decoded.chart->playback.setup.chart.md5 = chart->Meta.MD5;
  const auto retained = codec.encodeChart(*decoded.chart, 0, diagnostic);
  expect(retained.has_value(), "imported pop'n pattern can be retained in canonical BRD");
  if (retained) {
    expect(codec.decode(*retained, context(source)).chart == decoded.chart,
           "canonical extension round-trip preserves five-entry metadata");
    auto fallback = outerJson(*retained);
    expect(fallback["laneShufflePattern"] == Json::array({pattern}),
           "stock export keeps the five-lane pop'n namespace");
    fallback.erase("asobmashow");
    expect(codec.decode(encodeJson(fallback), context(source)).chart.has_value(),
           "exported pop'n metadata also survives stock-only fallback");
  }
}

void testStockDoublePlayPatternsUseGlobalRowsAndCanonicalLocalRows() {
  // Independent Java LaneRandomShuffleModifier(player=1, scratch=true), seed 0,
  // from SNURhythm/beatoraja 83574048ba48. Beat DP strides include scratch.
  struct Case { int mode; std::vector<int> global; std::vector<int> local; };
  const Case cases[]{
      {10, {6, 10, 7, 11, 9, 8}, {0, 4, 1, 5, 3, 2}},
      {14, {13, 10, 9, 12, 14, 15, 11, 8}, {5, 2, 1, 4, 6, 7, 3, 0}},
      {48, {44, 50, 39, 40, 46, 42, 37, 31, 48, 26, 33, 29, 38, 30, 51, 28,
            36, 45, 49, 27, 34, 43, 32, 47, 35, 41},
           {18, 24, 13, 14, 20, 16, 11, 5, 22, 0, 7, 3, 12, 4, 25, 2,
            10, 19, 23, 1, 8, 17, 6, 21, 9, 15}}};
  replay::BeatorajaReplayCodec codec;
  for (const auto &test : cases) {
    auto source = chartDocument();
    source.playback.setup.chart.keyMode = test.mode;
    source.playback.setup.player1 = {};
    source.playback.setup.player2 = {.option = "RANDOM-EX", .seed = 0,
                                    .laneShufflePattern = test.local};
    source.playback.input.resize(2);
    std::string diagnostic;
    const auto encoded = codec.encodeChart(source, 0, diagnostic);
    expect(encoded.has_value(), "canonical DP pattern encodes");
    if (!encoded) continue;
    auto stock = outerJson(*encoded);
    expect(stock["laneShufflePattern"] == Json::array({nullptr, test.global}),
           "stock export offsets player two by the complete reference lane width");
    expect(codec.decode(*encoded, context(source)).chart == source,
           "global stock metadata leaves canonical local extension metadata unchanged");
    auto historical = stock;
    historical["laneShufflePattern"] = Json::array({nullptr, test.local});
    expect(codec.decode(encodeJson(historical), context(source)).chart == source,
           "existing Aso documents retain local extension rows when their stock projection is also local");
    stock.erase("asobmashow");
    for (const auto &row : {test.global, test.local}) {
      stock["laneShufflePattern"] = Json::array({nullptr, row});
      auto decoded = codec.decode(encodeJson(stock), context(source));
      expect(decoded.chart && decoded.chart->playback.setup.player2.laneShufflePattern == test.local,
             "stock global and historical Aso local DP patterns import to the same canonical row");
      if (!decoded.chart) continue;
      auto meta = bms_parser::ChartMeta{};
      meta.KeyMode = test.mode;
      meta.IsDP = true;
      auto translated = replay::makeReplayDataFromSetup(
          decoded.chart->playback.setup, ScoreProvenance::Legacy(), meta, diagnostic);
      const int resultOffset = test.mode == 10 ? 5 : test.mode == 14 ? 7 : 26;
      expect(translated && translated->laneShufflePattern2P &&
                 translated->laneShufflePattern2P->front() == test.local.front() + resultOffset,
             "consumer adapter adds its existing result namespace offset exactly once");
      decoded.chart->playback.setup.chart.md5 = source.playback.setup.chart.md5;
      const auto reencoded = codec.encodeChart(*decoded.chart, 0, diagnostic);
      expect(reencoded && outerJson(*reencoded)["laneShufflePattern"] ==
                              Json::array({nullptr, test.global}),
             "re-export of either imported namespace restores the stock global row once");
    }
    for (int mutation = 0; mutation < 6; ++mutation) {
      Json row = test.global;
      if (mutation == 0) row[0] = row[1];
      if (mutation == 1) row[0] = test.local[0];
      if (mutation == 2) row[0] = -1;
      if (mutation == 3) row[0] = test.global[0] + 0.5;
      if (mutation == 4) row[0] = std::uint64_t{1} << 32 | test.global[0];
      if (mutation == 5) row.erase(row.begin());
      stock["laneShufflePattern"] = Json::array({nullptr, row});
      expect(!codec.decode(encodeJson(stock), context(source)).chart,
             "stock DP import rejects duplicate, mixed, negative, fractional, overflow and short rows");
    }
    for (const Json lane : {Json(test.local[0] + 0.5),
                            Json((std::uint64_t{1} << 32) | test.local[0])}) {
      auto malformed = outerJson(*encoded);
      malformed["asobmashow"]["setup"]["player2"]["laneShufflePattern"][0] = lane;
      expect(!codec.decode(encodeJson(malformed), context(source)).chart,
             "canonical pattern entries cannot truncate fractional or overflowing JSON numbers");
    }
  }
}

void testStockKeyboardProducerWheelDisplayRows() {
  // Unchanged PatternModifier.create/getRandomPattern at Beatoraja 83574048,
  // seed 0. Non-EX keyboard display rows overwrite the final wheel with
  // scratchKey[player]: 24 for 1P, but 25 (not 51) for global 2P.
  const std::vector<int> randomKeys{
      0, 10, 21, 3, 18, 17, 23, 5, 13, 22, 2, 16,
      9, 11, 12, 14, 20, 1, 19, 4, 7, 6, 8, 15};
  const std::vector<int> rotateKeys{
      10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
      22, 23, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  replay::BeatorajaReplayCodec codec;
  for (const int mode : {24, 48}) {
    for (int player = 0; player < (mode == 48 ? 2 : 1); ++player) {
      for (const int option : {2, 3}) {
        auto source = chartDocument();
        source.playback.setup.chart.keyMode = mode;
        source.playback.setup.player1 = {};
        source.playback.setup.player2 = {};
        auto &selected = player == 0 ? source.playback.setup.player1
                                     : source.playback.setup.player2;
        selected = {.option = option == 2 ? "RANDOM" : "R-RANDOM", .seed = 0};
        auto local = option == 2 ? randomKeys : rotateKeys;
        local.insert(local.end(), {24, 25});
        selected.laneShufflePattern = local;
        source.playback.input.resize(2);
        std::string diagnostic;
        const auto encoded = codec.encodeChart(source, 0, diagnostic);
        expect(encoded.has_value(), "canonical keyboard source encodes");
        if (!encoded) continue;
        auto stock = outerJson(*encoded);
        stock.erase("asobmashow");
        const auto global = stock["laneShufflePattern"][player];
        auto producer = global;
        producer[25] = player == 0 ? 24 : 25;
        stock["laneShufflePattern"][player] = producer;
        const auto decoded = codec.decode(encodeJson(stock), context(source));
        expect(decoded.chart.has_value(), "stock keyboard producer display row imports");
        if (decoded.chart) {
          const auto &actual = player == 0 ? decoded.chart->playback.setup.player1
                                           : decoded.chart->playback.setup.player2;
          expect(actual == selected,
                 "producer repair restores canonical wheels and preserves option/seed");
          auto retained = *decoded.chart;
          retained.playback.setup.chart.md5 = source.playback.setup.chart.md5;
          const auto reencoded = codec.encodeChart(retained, 0, diagnostic);
          expect(reencoded && outerJson(*reencoded)["laneShufflePattern"][player] == global,
                 "repaired display metadata re-exports with correct global wheels");
        }
        for (const Json row : {global, Json(local)}) {
          auto valid = stock;
          valid["laneShufflePattern"][player] = row;
          const auto imported = codec.decode(encodeJson(valid), context(source));
          expect(imported.chart &&
                     (player == 0 ? imported.chart->playback.setup.player1
                                  : imported.chart->playback.setup.player2) == selected,
                 "correct global and historical local keyboard rows stay unchanged");
        }
        for (int mutation = 0; mutation < 10; ++mutation) {
          auto malformed = stock;
          auto &row = malformed["laneShufflePattern"][player];
          if (mutation == 0) row[0] = row[1];
          if (mutation == 1) row[0] = player == 0 ? 26 : 0;
          if (mutation == 2) row[24] = 23 + player * 26;
          if (mutation == 3) row[25] = player == 0 ? 23 : 24;
          if (mutation == 4) row.erase(row.begin());
          if (mutation == 5) row.push_back(25 + player * 26);
          if (mutation == 6) row[0] = -1;
          if (mutation == 7) row[0] = 0.5;
          if (mutation == 8) row[0] = (std::uint64_t{1} << 32);
          if (mutation == 9) {
            row = local;
            row[25] = 24;
            if (player == 0) row[24] = row[25] = 25;
          }
          expect(!codec.decode(encodeJson(malformed), context(source)).chart,
                 "adjacent malformed keyboard rows are not producer repairs");
        }
        for (const int otherOption : {0, 1, 8, 9}) {
          auto wrongOption = stock;
          wrongOption[player == 0 ? "randomoption" : "randomoption2"] = otherOption;
          expect(!codec.decode(encodeJson(wrongOption), context(source)).chart,
                 "producer exception excludes NORMAL, MIRROR and EX options");
        }
        auto canonical = outerJson(*encoded);
        auto invalidLocal = local;
        invalidLocal[25] = 24;
        canonical["asobmashow"]["setup"][player == 0 ? "player1" : "player2"]
                 ["laneShufflePattern"] = invalidLocal;
        expect(!codec.decode(encodeJson(canonical), context(source)).chart,
               "canonical extension still rejects duplicate keyboard wheels");
      }
    }
  }
}

void testIndependentStockFixtures() {
  replay::BeatorajaReplayCodec codec;
  replay::ReplayDecodeContext chartContext{
      .stageKeyModes = {7},
      .stageTimeBounds = {{.completionSongTimeMicros = 10'000'000}},
  };
  const auto chart =
      codec.decode(readFixture("beatoraja-chart.brd"), chartContext);
  expect(chart.chart.has_value() && !chart.course.has_value() &&
             chart.stockOnly && chart.diagnostic.empty(),
         "independent stock chart fixture decodes as stock-only");
  if (chart.chart) {
    const auto &value = chart.chart->playback;
    expect(value.setup.chart.sha256 == std::string(64, 'a') &&
               value.setup.chart.md5.empty() && value.setup.chart.keyMode == 7,
           "stock chart identity uses selected parsed key mode");
    expect(value.setup.longNoteMode == 2 &&
               value.setup.player1.option == "RANDOM" &&
               value.setup.player1.seed == 0x123456,
           "stock LN and random setup map exactly");
    expect(value.setup.player1.laneShufflePattern ==
               std::optional<std::vector<int>>(
                   std::vector<int>{2, 0, 4, 5, 6, 3, 1, 7}),
           "stock lane-shuffle pattern is retained exactly");
    expect(value.setup.initialGaugeType == GaugeType::Hard &&
               value.setup.startingGaugePercent == 100.0F,
           "stock survival gauge starts from the shared gauge authority");
    expect(value.input.size() == 6 &&
               value.input[2].control.kind ==
                   replay::LogicalControlKind::ScratchClockwise &&
               value.input[4].control.kind ==
                   replay::LogicalControlKind::ScratchCounterClockwise,
           "stock keyinput preserves both scratch directions");
  }

  replay::ReplayDecodeContext courseContext{
      .stageKeyModes = {7, 7},
      .stageTimeBounds =
          {
              {.completionSongTimeMicros = 10'000'000},
              {.completionSongTimeMicros = 10'000'000},
          },
  };
  const auto course =
      codec.decode(readFixture("beatoraja-course.brd"), courseContext);
  expect(course.course && course.course->playback.stages.size() == 2 &&
             course.stockOnly,
         "independent stock course fixture preserves both stages");
  if (course.course) {
    expect(course.course->playback.stages[1].setup.chart.sha256 ==
                   std::string(64, 'b') &&
               course.course->playback.stages[1].setup.player1.option ==
                   "R-RANDOM",
           "second stock course stage maps independently");
  }
}

void testLocalChartRoundTripAndStockProjection() {
  replay::BeatorajaReplayCodec codec;
  const auto source = chartDocument();
  std::string diagnostic;
  const auto encoded =
      codec.encodeChart(source, 1'725'000'000'123LL, diagnostic);
  expect(encoded.has_value(), "local chart document encodes");
  if (!encoded) {
    return;
  }
  expect(encoded->size() <= replay::kReplayLimits.maxCompressedBytes,
         "encoded BRD obeys shared compressed size limit");
  const auto stock = outerJson(*encoded);
  expect(stock.at("sha256") == source.playback.setup.chart.sha256 &&
             stock.at("doubleoption") == 0 &&
             stock.at("config").at("lanecover") == 0.37F,
         "Aso BRD exposes stock identity, DP option, and lane cover");

  const auto decoded = codec.decode(*encoded, context(source));
  expect(decoded.chart == std::optional(source) && !decoded.stockOnly &&
             decoded.stageSources ==
                 std::vector{replay::ReplayStageDecodeSource::AsoExtension} &&
             decoded.diagnostic.empty(),
         "local setup and signed playback data round-trip exactly");

  const auto encodedAgain =
      codec.encodeChart(source, 1'725'000'000'123LL, diagnostic);
  expect(encodedAgain == encoded,
         "deterministic encoding supports exact-attempt retry");

  auto assisted = source;
  assisted.playback.setup.assistOption = "ASSISTED";
  const auto assistedBytes = codec.encodeChart(assisted, 1'725'000'000'123LL,
                                              diagnostic);
  expect(assistedBytes.has_value(), "generic assist uses the existing replay setup");
  if (assistedBytes) {
    const auto payload = outerJson(*assistedBytes);
    expect(payload["asobmashow"]["schemaVersion"] == 3 &&
               !payload["asobmashow"]["setup"].contains("pauseUsed"),
           "assisted replay does not require a new schema or pause field");
    expect(codec.decode(*assistedBytes, context(assisted)).chart ==
               std::optional(assisted),
           "generic assisted replay round-trips through the existing codec");
  }
}

void testStockKeyEncodingMatchesIndependentJavaBytes() {
  auto source = chartDocument();
  source.playback.setup.player1.option = "RANDOM";
  source.playback.setup.player1.seed = 0x123456;
  source.playback.setup.player1.laneShufflePattern =
      std::vector<int>{2, 0, 4, 5, 6, 3, 1, 7};
  source.playback.setup.chartRandomValues = {4, 2, 7};
  source.playback.setup.initialGaugeType = GaugeType::Hard;
  source.playback.setup.initialLaneCoverPercent = 37;
  source.playback.input = {
      {.songTimeMicros = 1'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 1'500,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
      {.songTimeMicros = 2'000,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
      {.songTimeMicros = 2'500,
       .control = {.kind = replay::LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false},
      {.songTimeMicros = 3'000,
       .control = {.kind = replay::LogicalControlKind::ScratchCounterClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
      {.songTimeMicros = 3'500,
       .control = {.kind = replay::LogicalControlKind::ScratchCounterClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false},
  };
  source.playback.touchSamples.clear();
  source.playback.laneCoverEvents.clear();

  replay::BeatorajaReplayCodec codec;
  std::string diagnostic;
  const auto encoded =
      codec.encodeChart(source, 1'725'000'000'000LL, diagnostic);
  expect(encoded.has_value(), "independent key-byte source encodes");
  if (!encoded) {
    return;
  }
  const auto document = outerJson(*encoded);
  const auto compressed = replay::base64UrlDecodeBounded(
      document.at("keyinput").get<std::string>(), 1024, diagnostic);
  const auto records =
      compressed ? replay::gzipDecompressBounded(*compressed, 1024, diagnostic)
                 : std::nullopt;
  expect(records == std::optional<Bytes>(readFixture("beatoraja-keyinput.bin")),
         "stock compact key records match Java shrink output byte-for-byte");
}

void testManualAssignmentUsesStockNormalAndExactExtension() {
  replay::BeatorajaReplayCodec codec;
  auto source = chartDocument();
  source.playback.setup.player1.option = "ASSIGN:S1234567";
  source.playback.setup.player1.seed.reset();
  source.playback.setup.player1.laneShufflePattern.reset();
  std::string diagnostic;
  const auto encoded = codec.encodeChart(source, 1, diagnostic);
  expect(encoded && outerJson(*encoded).at("randomoption") == 0,
         "manual assignment projects to stock NORMAL");
  if (encoded) {
    const auto decoded = codec.decode(*encoded, context(source));
    expect(decoded.chart == std::optional(source),
           "manual assignment remains exact in the Aso extension");
  }
}

void testEmptyCompletedReplayAndInclusivePreRoll() {
  replay::BeatorajaReplayCodec codec;
  auto source = chartDocument();
  source.playback.input.clear();
  source.playback.touchSamples.clear();
  source.playback.laneCoverEvents.clear();
  std::string diagnostic;
  const auto encoded = codec.encodeChart(source, 1, diagnostic);
  expect(encoded.has_value(), "completed replay with empty input encodes");
  if (encoded) {
    expect(codec.decode(*encoded, context(source)).chart ==
               std::optional(source),
           "synthetic stock compatibility record collapses to empty input");
  }

  source = chartDocument();
  source.playback.input.front().songTimeMicros =
      replay::kReplayLimits.minimumSongTimeMicros;
  expect(codec.encodeChart(source, 1, diagnostic).has_value(),
         "inclusive shared pre-roll boundary encodes");
  --source.playback.input.front().songTimeMicros;
  expect(!codec.encodeChart(source, 1, diagnostic),
         "timestamp before shared pre-roll boundary is rejected");
}

void testCourseRoundTripAndAggregateLimits() {
  replay::BeatorajaReplayCodec codec;
  const auto first = chartDocument();
  auto second = chartDocument();
  second.playback.setup = setup(std::string(64, 'c'));
  replay::ReplayCourseDocument source{
      .playback = {.stages = {first.playback, second.playback},
                   .restMicrosAfterStage =
                       {replay::kReplayLimits.maxCourseRestMicros, 0}},
      .timeBounds = {first.timeBounds, second.timeBounds},
  };
  replay::ReplayDecodeContext decodeContext{
      .stageKeyModes = {7, 7},
      .stageTimeBounds = source.timeBounds,
  };
  std::string diagnostic;
  const auto encoded = codec.encodeCourse(source, 1, diagnostic);
  expect(encoded.has_value(), "mixed-stage course and maximum rest encode");
  if (encoded) {
    const auto decoded = codec.decode(*encoded, decodeContext);
    expect(decoded.course == std::optional(source),
           "course stages, timing bounds, and rest round-trip exactly");
  }

  ++source.playback.restMicrosAfterStage.front();
  expect(!codec.encodeCourse(source, 1, diagnostic),
         "course rest above shared limit is rejected");

  source.playback.restMicrosAfterStage.front() = 0;
  replay::ReplayLimits strict = replay::kReplayLimits;
  strict.maxInputTransitions = first.playback.input.size() * 2 - 1;
  replay::BeatorajaReplayCodec strictCodec(strict);
  expect(!strictCodec.encodeCourse(source, 1, diagnostic),
         "course input limit is aggregate across stages");
}

void testDoublePlayAndKeyMapping() {
  replay::BeatorajaReplayCodec codec;
  Json stock = outerJson(readFixture("beatoraja-chart.brd"));
  stock["doubleoption"] = 1;
  stock["laneShufflePattern"] = Json::array({nullptr, nullptr});
  replay::ReplayDecodeContext context14{
      .stageKeyModes = {14},
      .stageTimeBounds = {{.completionSongTimeMicros = 10'000'000}},
  };
  const auto flip = codec.decode(encodeJson(stock), context14);
  expect(flip.chart && flip.chart->playback.setup.doublePlayOption ==
                           replay::DoublePlayOption::Flip,
         "stock DP doubleoption FLIP is honored");
  stock["doubleoption"] = 2;
  expect(!codec.decode(encodeJson(stock), context14).chart,
         "unsupported stock DP option fails closed");

  struct Mapping {
    int mode;
    replay::LogicalControl control;
    int stock;
  };
  const std::array mappings{
      Mapping{
          5,
          {.kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 4},
          4},
      Mapping{
          4,
          {.kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 4},
          4},
      Mapping{
          6,
          {.kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 6},
          6},
      Mapping{
          8,
          {.kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 7},
          7},
      Mapping{7,
              {.kind = replay::LogicalControlKind::ScratchClockwise,
               .player = 1,
               .lane = -1},
              7},
      Mapping{
          9,
          {.kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 8},
          8},
      Mapping{10,
              {.kind = replay::LogicalControlKind::ScratchClockwise,
               .player = 2,
               .lane = -1},
              12},
      Mapping{14,
              {.kind = replay::LogicalControlKind::ScratchCounterClockwise,
               .player = 2,
               .lane = -1},
              17},
      Mapping{
          24,
          {.kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 25},
          25},
      Mapping{
          48,
          {.kind = replay::LogicalControlKind::Lane, .player = 2, .lane = 25},
          51},
  };
  for (const auto &mapping : mappings) {
    expect(replay::BeatorajaReplayCodec::beatorajaKeyCode(
               mapping.control, mapping.mode) == mapping.stock &&
               replay::BeatorajaReplayCodec::logicalControl(
                   mapping.stock, mapping.mode) == mapping.control,
           "stock key map is reversible for every supported mode");
  }
}

void testNonStockChartProjectsBeatorajaKeyInput() {
  replay::BeatorajaReplayCodec codec;
  for (const auto [keyMode, lane] :
       {std::pair{4, 4}, std::pair{6, 6}, std::pair{8, 7}}) {
    auto source = chartDocument();
    source.playback.setup = setup(std::string(64, 'a'), keyMode);
    source.playback.setup.player1.laneShufflePattern.reset();
    source.playback.input = {
        {.songTimeMicros = 0,
         .control = {.kind = replay::LogicalControlKind::Lane,
                     .player = 1,
                     .lane = lane},
         .pressed = true},
        {.songTimeMicros = 1,
         .control = {.kind = replay::LogicalControlKind::Lane,
                     .player = 1,
                     .lane = lane},
         .pressed = false},
    };
    std::string diagnostic;
    const auto encoded = codec.encodeChart(source, 1, diagnostic);
    expect(encoded.has_value(),
           "non-stock accepted input produces a BRD document");
    if (!encoded) {
      continue;
    }
    Json stock = outerJson(*encoded);
    const auto compressed = replay::base64UrlDecodeBounded(
        stock.at("keyinput").get<std::string>(), 1024, diagnostic);
    const auto records =
        compressed
            ? replay::gzipDecompressBounded(*compressed, 1024, diagnostic)
            : std::nullopt;
    auto expectedRecords = stockKeyRecord(lane + 1, 0);
    auto releaseRecord = stockKeyRecord(-(lane + 1), 1);
    expectedRecords.insert(expectedRecords.end(), releaseRecord.begin(),
                           releaseRecord.end());
    expect(records == std::optional(expectedRecords),
           "non-stock BRD keyinput uses exact Beatoraja signed records");

    stock.erase("asobmashow");
    const auto stockDecoded = codec.decode(encodeJson(stock), context(source));
    expect(stockDecoded.chart && stockDecoded.stockOnly &&
               stockDecoded.stageSources ==
                   std::vector{replay::ReplayStageDecodeSource::Stock} &&
               stockDecoded.chart->playback.input == source.playback.input,
           "stock-only Beatoraja fallback preserves non-stock BMS channel "
           "lane controls");
  }
}

void testOldRulesetIsExplicitlyObsolete() {
  replay::BeatorajaReplayCodec codec;
  const auto source = chartDocument();
  std::string diagnostic;
  const auto encoded = codec.encodeChart(source, 1, diagnostic);
  expect(encoded.has_value(), "obsolete fixture encodes with current rules");
  if (!encoded) return;
  auto previous = outerJson(*encoded);
  previous["asobmashow"]["setup"]["ruleset"]["version"] = 3;
  const auto decoded = codec.decode(encodeJson(previous), context(source));
  expect(decoded.obsoleteRuleset && decoded.chart && !decoded.course &&
             decoded.chart->playback.input == source.playback.input &&
             decoded.chart->playback.setup.ruleset.version == 3 &&
             !decoded.stockOnly,
         "old Aso input retains its recorded identity for current-rules playback");
}

void testSupportedAsoExtensionIsAuthoritative() {
  replay::BeatorajaReplayCodec codec;
  const auto source = chartDocument();
  std::string diagnostic;
  const auto encoded = codec.encodeChart(source, 1, diagnostic);
  if (!encoded) {
    expect(false, "agreement mutation fixture encodes");
    return;
  }

  Json mismatch = outerJson(*encoded);
  mismatch["sha256"] = std::string(64, 'd');
  auto decoded = codec.decode(encodeJson(mismatch), context(source));
  expect(decoded.chart == std::optional(source) && !decoded.stockOnly,
         "supported extension owns chart identity when stock differs");

  mismatch = outerJson(*encoded);
  mismatch["randomoption"] = 1;
  decoded = codec.decode(encodeJson(mismatch), context(source));
  expect(decoded.chart == std::optional(source) && !decoded.stockOnly,
         "supported extension owns replay options when stock differs");

  mismatch = outerJson(*encoded);
  mismatch["laneShufflePattern"][0] =
      std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7};
  decoded = codec.decode(encodeJson(mismatch), context(source));
  expect(decoded.chart == std::optional(source) && !decoded.stockOnly,
         "supported extension owns lane patterns when stock differs");

  Json future = outerJson(*encoded);
  future["asobmashow"]["schemaVersion"] = 999;
  const auto stockFallback = codec.decode(encodeJson(future), context(source));
  expect(stockFallback.chart && stockFallback.unsupportedAsoExtension &&
             stockFallback.stockOnly,
         "future extension remains an explicit stock-only replay surface");
}

void testLaneCoverStateRoundTripsAndLegacyEventsUseSetupState() {
  replay::BeatorajaReplayCodec codec;
  auto source = chartDocument();
  source.playback.setup.laneCoverEnabled = true;
  source.playback.laneCoverEvents = {
      {.songTimeMicros = -1'000,
       .noteStartPositionPercent = 37,
       .laneCoverEnabled = false,
       .changeKind = ReplayLaneCoverChangeKind::Value},
      {.songTimeMicros = 2'000,
       .noteStartPositionPercent = 41,
       .laneCoverEnabled = true,
       .changeKind = ReplayLaneCoverChangeKind::Enabled,
       .resetVisibleTimeReference = true},
  };

  std::string diagnostic;
  const auto encoded = codec.encodeChart(source, 1, diagnostic);
  expect(encoded.has_value(), "lane-cover state fixture encodes");
  if (!encoded) {
    return;
  }

  const auto decoded = codec.decode(*encoded, context(source));
  expect(decoded.chart == std::optional(source),
         "lane-cover events preserve enabled-state transitions");

  Json legacy = outerJson(*encoded);
  for (auto &event : legacy["asobmashow"]["laneCoverEvents"]) {
    event.erase("laneCoverEnabled");
    event.erase("changeKind");
  }
  auto expectedLegacy = source;
  for (auto &event : expectedLegacy.playback.laneCoverEvents) {
    event.laneCoverEnabled = expectedLegacy.playback.setup.laneCoverEnabled;
    event.changeKind = ReplayLaneCoverChangeKind::Value;
  }
  const auto decodedLegacy = codec.decode(encodeJson(legacy), context(source));
  expect(decodedLegacy.chart == std::optional(expectedLegacy),
         "legacy lane-cover events inherit their recorded setup state");
}

void testFineAndLowerCoverStateRoundTrip() {
  replay::BeatorajaReplayCodec codec;
  auto source = chartDocument();
  lane_cover::State covers{.laneCoverPercent = 37.4F, .laneCoverEnabled = true,
                           .liftEnabled = true, .liftRatio = 0.25F,
                           .hiddenEnabled = true, .hiddenRatio = 0.15F};
  source.playback.setup.coverState = covers;
  covers.liftRatio = 0.251F;
  source.playback.laneCoverEvents = {{
      .songTimeMicros = -1'000, .noteStartPositionPercent = 37,
      .laneCoverEnabled = true, .changeKind = ReplayLaneCoverChangeKind::Lift,
      .coverState = covers}};
  covers.hiddenRatio = 0.151F;
  source.playback.laneCoverEvents.push_back({
      .songTimeMicros = 2'000, .noteStartPositionPercent = 37,
      .laneCoverEnabled = true, .changeKind = ReplayLaneCoverChangeKind::Hidden,
      .resetVisibleTimeReference = true, .coverState = covers});
  std::string diagnostic;
  const auto encoded = codec.encodeChart(source, 1, diagnostic);
  expect(encoded.has_value(), "full fine-grained cover state encodes");
  if (!encoded) return;
  expect(codec.decode(*encoded, context(source)).chart == std::optional(source),
         "initial covers and lift/hidden adjustment events round-trip exactly");
  auto malformed = outerJson(*encoded);
  malformed["asobmashow"]["laneCoverEvents"][0].erase("coverState");
  expect(!codec.decode(encodeJson(malformed), context(source)).chart,
         "lower cover transitions require their recorded state");
  malformed = outerJson(*encoded);
  malformed["asobmashow"]["setup"]["coverState"]["liftRatio"] = 1.1;
  expect(!codec.decode(encodeJson(malformed), context(source)).chart,
         "out-of-range lift state is rejected");
  malformed = outerJson(*encoded);
  malformed["asobmashow"]["laneCoverEvents"][0]["coverState"]["laneCoverPercent"] = 38.0;
  expect(!codec.decode(encodeJson(malformed), context(source)).chart,
         "contradictory fine and legacy cover values are rejected");
}

void testContextAndUntrustedStructureFailClosed() {
  replay::BeatorajaReplayCodec codec;
  const auto source = chartDocument();
  std::string diagnostic;
  const auto encoded = codec.encodeChart(source, 1, diagnostic);
  if (!encoded) {
    return;
  }
  auto wrongMode = context(source);
  wrongMode.stageKeyModes[0] = 14;
  expect(!codec.decode(*encoded, wrongMode).chart,
         "selected parsed key mode cannot be overwritten by replay metadata");
  auto wrongBounds = context(source);
  --wrongBounds.stageTimeBounds[0].completionSongTimeMicros;
  expect(!codec.decode(*encoded, wrongBounds).chart,
         "extension completion context must agree with selected chart");

  replay::ReplayDecodeContext embeddedBoundsContext{
      .stageKeyModes = {source.playback.setup.chart.keyMode},
  };
  const auto embeddedBounds = codec.decode(*encoded, embeddedBoundsContext);
  expect(embeddedBounds.chart &&
             embeddedBounds.chart->timeBounds == source.timeBounds,
         "local Aso extension supplies its validated completion bound");

  const auto stockFixture = readFixture("beatoraja-chart.brd");
  expect(!codec.decode(stockFixture, embeddedBoundsContext).chart,
         "stock-only BRD still requires an authoritative chart time bound");

  Json tooDeep = outerJson(*encoded);
  Json *cursor = &tooDeep["nested"];
  for (std::size_t depth = 0; depth <= replay::kReplayLimits.maxJsonDepth;
       ++depth) {
    (*cursor)["nested"] = Json::object();
    cursor = &(*cursor)["nested"];
  }
  expect(!codec.decode(encodeJson(tooDeep), context(source)).chart,
         "JSON nesting beyond shared limit is rejected");
}

void testStockLiftAndHiddenSurviveImportAndExport() {
  replay::BeatorajaReplayCodec codec;
  const auto source = chartDocument();
  std::string diagnostic;
  const auto encoded = codec.encodeChart(source, 1, diagnostic);
  expect(encoded.has_value(), "stock cover fixture encodes");
  if (!encoded) return;
  auto stock = outerJson(*encoded);
  stock.erase("asobmashow");
  stock["config"] = {{"lanecover", 0.371F}, {"enablelanecover", true},
                     {"lift", 0.245F}, {"enablelift", true},
                     {"hidden", 0.153F}, {"enablehidden", true}};
  const auto imported = codec.decode(encodeJson(stock), context(source));
  expect(imported.chart.has_value(), "stock lift/hidden fixture imports");
  if (!imported.chart) return;
  auto local = source;
  local.playback.setup.coverState = imported.chart->playback.setup.coverState;
  const auto reencoded = codec.encodeChart(local, 1, diagnostic);
  expect(reencoded.has_value(), "imported stock covers can be retained in a local capture");
  if (!reencoded) return;
  const auto config = outerJson(*reencoded)["config"];
  expect(config.value("enablelift", false) && config.value("enablehidden", false) &&
             std::abs(config.value("lift", 0.0F) - 0.245F) < 0.00001F &&
             std::abs(config.value("hidden", 0.0F) - 0.153F) < 0.00001F &&
             std::abs(config.value("lanecover", 0.0F) - 0.371F) < 0.00001F,
         "stock import/export preserves lift, hidden, and fine sudden values");
}

} // namespace

void testAbortedCompletionSurvivesCodecRoundTrip() {
  replay::BeatorajaReplayCodec codec;
  auto document = chartDocument();
  document.timeBounds.aborted = true;
  std::string diagnostic;
  const auto encoded = codec.encodeChart(document, 0, diagnostic);
  expect(encoded.has_value(), "abort fixture encodes");
  if (!encoded) return;
  auto json = outerJson(*encoded);
  expect(json["asobmashow"]["schemaVersion"] == 4,
         "T2-R1: essential abort uses an extension rejected by the v3 reader");
  const auto decoded = codec.decode(encodeJson(json), {.stageKeyModes = {7}});
  expect(decoded.chart.has_value(), "abort extension decodes");
  if (!decoded.chart) return;
  const auto reencoded = codec.encodeChart(*decoded.chart, 0, diagnostic);
  expect(reencoded && outerJson(*reencoded)["asobmashow"].value("aborted", false),
         "COR02: terminal abort fact must survive decode and encode independently of result summary");
  auto trusted = context(document);
  trusted.stageTimeBounds.front().aborted = false;
  expect(!codec.decode(encodeJson(json), trusted).chart,
         "COR02: explicit trusted non-abort must reject conflicting abort evidence");
  trusted.stageTimeBounds.front().aborted = true;
  json["asobmashow"]["aborted"] = false;
  expect(!codec.decode(encodeJson(json), trusted).chart,
         "COR02: explicit trusted abort must reject conflicting non-abort evidence");
  json["asobmashow"]["aborted"] = "true";
  expect(!codec.decode(encodeJson(json), {.stageKeyModes = {7}}).chart,
         "COR02: malformed abort evidence fails closed");
  json["asobmashow"].erase("aborted");
  expect(!codec.decode(encodeJson(json), {.stageKeyModes = {7}}).chart,
         "T2-R1: terminal schema requires its essential abort evidence");
  json["asobmashow"]["schemaVersion"] = 3;
  document.timeBounds.aborted.reset();
  expect(codec.decode(encodeJson(json), context(document)).chart.has_value(),
         "COR02: absent-field v3 replay retains legacy compatibility");
  json["asobmashow"]["aborted"] = true;
  expect(!codec.decode(encodeJson(json), {.stageKeyModes = {7}}).chart,
         "T2-R1: abort evidence cannot be downgraded into the ordinary schema");
  const auto ordinary = codec.encodeChart(document, 0, diagnostic);
  expect(ordinary && outerJson(*ordinary)["asobmashow"]["schemaVersion"] == 3 &&
             codec.decode(*ordinary, context(document)).chart.has_value(),
         "T2-R1: ordinary documents continue to write and read the existing v3 schema");
  replay::ReplayCourseDocument course{
      .playback = {.stages = {document.playback, document.playback},
                   .restMicrosAfterStage = {0, 0}},
      .timeBounds = {document.timeBounds, document.timeBounds}};
  course.timeBounds.back().aborted = true;
  const auto courseBytes = codec.encodeCourse(course, 0, diagnostic);
  expect(courseBytes && outerJson(*courseBytes)[0]["asobmashow"]["schemaVersion"] == 3 &&
             outerJson(*courseBytes)[1]["asobmashow"]["schemaVersion"] == 4,
         "T2-R1: course stages retain per-document ordinary and abort capabilities");
  if (courseBytes) {
    const auto courseDecoded = codec.decode(*courseBytes, {.stageKeyModes = {7, 7}});
    expect(courseDecoded.course && courseDecoded.course->timeBounds.back().aborted == true &&
               !courseDecoded.course->timeBounds.front().aborted.has_value(),
           "T2-R1: mixed-version course retains its essential terminal evidence");
  }
}

void testNegativeAbortCodecBounds() {
  replay::BeatorajaReplayCodec codec;
  auto document = chartDocument();
  document.playback.input.clear();
  document.playback.touchSamples.clear();
  document.playback.laneCoverEvents.clear();
  std::string diagnostic;
  for (const auto completion : {-30'000'000LL, -1'000'000LL, -1LL}) {
    document.timeBounds = {completion, true};
    document.playback.input = {{.songTimeMicros = completion,
        .control = {.kind = replay::LogicalControlKind::Lane, .player = 1, .lane = 0},
        .pressed = true}};
    document.playback.touchSamples = {{.action = replay::ReplayTouchAction::Down,
        .fingerId = 1, .songTimeMicros = completion, .x = 0.5F, .y = 0.5F}};
    document.playback.laneCoverEvents = {{.songTimeMicros = completion,
        .noteStartPositionPercent = 20}};
    const auto bytes = codec.encodeChart(document, 0, diagnostic);
    expect(bytes.has_value(), "pre-song abort encodes using signed completion");
    if (!bytes) continue;
    const auto json = outerJson(*bytes);
    expect(json["asobmashow"]["schemaVersion"] == 4,
           "pre-song abort uses existing terminal schema");
    for (const auto stream : {"input", "touchSamples", "laneCoverEvents"}) {
      auto late = json;
      late["asobmashow"][stream][0]["songTimeMicros"] = completion + 1;
      expect(!codec.decode(encodeJson(late), {.stageKeyModes = {7}}).chart,
             "decoder rejects post-abort stream evidence without shifting terminal time");
    }
    for (const auto trusted : {false, true}) {
      const auto decoded = codec.decode(*bytes, trusted ? context(document) :
          replay::ReplayDecodeContext{.stageKeyModes = {7}});
      expect(decoded.chart && decoded.chart->timeBounds == document.timeBounds,
             "trusted and self-contained decode retain signed abort timestamp");
    }
    auto malformed = json;
    malformed["asobmashow"]["aborted"] = false;
    expect(!codec.decode(encodeJson(malformed), {.stageKeyModes = {7}}).chart,
           "negative non-abort terminal document fails closed");
    malformed["asobmashow"].erase("aborted");
    malformed["asobmashow"]["schemaVersion"] = 3;
    expect(!codec.decode(encodeJson(malformed), {.stageKeyModes = {7}}).chart,
           "negative completion cannot be downgraded to ordinary schema");
    malformed = json;
    malformed["asobmashow"]["input"] = Json::array();
    malformed["asobmashow"]["touchSamples"] = Json::array();
    malformed["asobmashow"]["laneCoverEvents"] = Json::array();
    malformed["asobmashow"]["completionSongTimeMicros"] = -30'000'001LL;
    expect(!codec.decode(encodeJson(malformed), {.stageKeyModes = {7}}).chart,
           "empty abort before supported pre-roll fails closed");
    auto limits = replay::kReplayLimits;
    limits.minimumSongTimeMicros = completion + 1;
    replay::BeatorajaReplayCodec restricted(limits);
    expect(!restricted.decode(*bytes, context(document)).chart &&
               !restricted.decode(*bytes, {.stageKeyModes = {7}}).chart,
           "trusted and self-contained decode enforce narrower configured pre-roll");
  }
}

int main() {
  testPopnFiveKeyStockShuffleSurvivesChartApplication();
  testStockDoublePlayPatternsUseGlobalRowsAndCanonicalLocalRows();
  testStockKeyboardProducerWheelDisplayRows();
  testNegativeAbortCodecBounds();
  testAbortedCompletionSurvivesCodecRoundTrip();
  testIndependentStockFixtures();
  testLocalChartRoundTripAndStockProjection();
  testStockKeyEncodingMatchesIndependentJavaBytes();
  testManualAssignmentUsesStockNormalAndExactExtension();
  testEmptyCompletedReplayAndInclusivePreRoll();
  testCourseRoundTripAndAggregateLimits();
  testDoublePlayAndKeyMapping();
  testNonStockChartProjectsBeatorajaKeyInput();
  testOldRulesetIsExplicitlyObsolete();
  testSupportedAsoExtensionIsAuthoritative();
  testLaneCoverStateRoundTripsAndLegacyEventsUseSetupState();
  testFineAndLowerCoverStateRoundTrip();
  testStockLiftAndHiddenSurviveImportAndExport();
  testContextAndUntrustedStructureFailClosed();
  if (failures != 0) {
    std::cerr << failures << " Beatoraja replay codec test(s) failed\n";
    return 1;
  }
  std::cout << "Beatoraja replay codec tests passed\n";
  return 0;
}
