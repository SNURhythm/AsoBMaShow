#include "replay/Base64Url.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/GzipCodec.h"

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

} // namespace

int main() {
  testIndependentStockFixtures();
  testLocalChartRoundTripAndStockProjection();
  testStockKeyEncodingMatchesIndependentJavaBytes();
  testManualAssignmentUsesStockNormalAndExactExtension();
  testEmptyCompletedReplayAndInclusivePreRoll();
  testCourseRoundTripAndAggregateLimits();
  testDoublePlayAndKeyMapping();
  testNonStockChartProjectsBeatorajaKeyInput();
  testSupportedAsoExtensionIsAuthoritative();
  testLaneCoverStateRoundTripsAndLegacyEventsUseSetupState();
  testContextAndUntrustedStructureFailClosed();
  if (failures != 0) {
    std::cerr << failures << " Beatoraja replay codec test(s) failed\n";
    return 1;
  }
  std::cout << "Beatoraja replay codec tests passed\n";
  return 0;
}
