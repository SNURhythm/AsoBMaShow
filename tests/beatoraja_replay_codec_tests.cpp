#include "replay/Base64Url.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/GzipCodec.h"
#include "bms_parser.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
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

template <typename T, typename U>
void expectEqual(const T &actual, const U &expected, std::string_view message) {
  expect(actual == expected, message);
}

Bytes bytes(std::string_view value) {
  Bytes result(value.size());
  std::transform(value.begin(), value.end(), result.begin(),
                 [](char ch) { return static_cast<std::byte>(ch); });
  return result;
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
  Bytes result(raw.size());
  std::transform(raw.begin(), raw.end(), result.begin(),
                 [](char value) { return static_cast<std::byte>(value); });
  return result;
}

Json outerJson(const Bytes &encoded) {
  std::string diagnostic;
  const auto decompressed =
      replay::gzipDecompressBounded(encoded, 4U * 1024U * 1024U, diagnostic);
  expect(decompressed.has_value(), "outer fixture gzip decompresses");
  return decompressed ? Json::parse(string(*decompressed)) : Json{};
}

Bytes encodeJson(const Json &document) {
  const std::string serialized = document.dump();
  std::string diagnostic;
  const auto result = replay::gzipCompress(bytes(serialized), diagnostic);
  expect(result.has_value(), "test JSON gzip encoding succeeds");
  return result.value_or(Bytes{});
}

replay::LogicalControl lane(int player, int index) {
  return {.kind = replay::LogicalControlKind::Lane,
          .player = player,
          .lane = index};
}

replay::LogicalControl control(replay::LogicalControlKind kind, int player) {
  return {.kind = kind, .player = player, .lane = -1};
}

void appendRecord(Bytes &records, std::int8_t signedCode, std::int64_t time);

std::optional<Bytes> stockKeyRecords(const Json &stock) {
  std::string diagnostic;
  const auto compressed = replay::base64UrlDecodeBounded(
      stock.at("keyinput").get<std::string>(), 4U * 1024U * 1024U,
      diagnostic);
  return compressed ? replay::gzipDecompressBounded(
                          *compressed, 16U * 1024U * 1024U, diagnostic)
                    : std::nullopt;
}

replay::ReplayPlaybackData extensionReplay() {
  replay::ReplayPlaybackData value;
  value.setup.chartMd5 = "0123456789abcdef0123456789abcdef";
  value.setup.chartSha256 =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  value.setup.keyMode = 7;
  value.setup.longNoteMode = 2;
  value.setup.hasUndefinedLongNotes = true;
  value.setup.randomSeed = 17;
  value.setup.randomPrng = bms_parser::Parser::RandomPrngId;
  value.setup.randomValues = {3, 1, 4};
  value.setup.playOption = "R-RANDOM";
  value.setup.playOptionSeed = 0x123456;
  value.setup.playOption2 = "NORMAL";
  value.setup.playOption2Seed = 0x234567;
  value.setup.assistOption = "OFF";
  value.setup.initialGaugeType = GaugeType::ExHard;
  value.setup.gaugeProfile = GaugeProfile::Standard;
  value.setup.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  value.setup.gaugeAutoShiftLowerBound = GaugeType::Easy;
  value.setup.playbackRulesetId = "asobmashow";
  value.setup.playbackRulesetRevision = 11;
  value.setup.playbackRatePercent = 125;
  value.setup.playbackMode = audio::PlaybackMode::TimeStretch;
  value.setup.candidateSelection = gameplay::CandidateSelectionMode::Score;
  value.setup.judgeWindowScalePercent = 90;
  value.setup.startingGaugePercent = 42.5F;
  GaugeStateSnapshot startingGauge{
      .gaugeType = GaugeType::Hard,
      .selectedGaugeType = GaugeType::ExHard,
      .gaugeAutoShiftLowerBound = GaugeType::Easy,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::BestClear,
      .currentGauge = 100.0F,
  };
  startingGauge.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 20.0F;
  startingGauge.gaugeValues[gaugeTypeIndex(GaugeType::Hard)] = 100.0F;
  startingGauge.gaugeValues[gaugeTypeIndex(GaugeType::ExHard)] = 100.0F;
  startingGauge.gaugeSurvivalFailed[gaugeTypeIndex(GaugeType::ExHard)] = true;
  value.setup.startingGaugeState = startingGauge;
  value.setup.clubMode = true;
  value.setup.initialLaneCoverPercent = 37;
  value.setup.laneCoverEnabled = true;

  value.input = {
      {.songTimeMicros = 1000, .control = lane(1, 0), .pressed = true},
      {.songTimeMicros = 1500, .control = lane(1, 0), .pressed = false},
      {.songTimeMicros = 2000,
       .control = control(replay::LogicalControlKind::ScratchClockwise, 1),
       .pressed = true},
      {.songTimeMicros = 2500,
       .control = control(replay::LogicalControlKind::ScratchClockwise, 1),
       .pressed = false},
      {.songTimeMicros = 3000,
       .control =
           control(replay::LogicalControlKind::ScratchCounterClockwise, 1),
       .pressed = true},
      {.songTimeMicros = 3500,
       .control =
           control(replay::LogicalControlKind::ScratchCounterClockwise, 1),
       .pressed = false},
      {.songTimeMicros = 4000,
       .control = control(replay::LogicalControlKind::Start, 1),
       .pressed = true},
      {.songTimeMicros = 4100,
       .control = control(replay::LogicalControlKind::Select, 1),
       .pressed = true},
      {.songTimeMicros = 4200,
       .control = control(replay::LogicalControlKind::Select, 1),
       .pressed = false},
      {.songTimeMicros = 4300,
       .control = control(replay::LogicalControlKind::Start, 1),
       .pressed = false},
  };
  value.touchSamples = {
      {.action = replay::ReplayTouchAction::Down,
       .fingerId = 7,
       .songTimeMicros = 5000,
       .x = 0.25F,
       .y = 0.75F},
      {.action = replay::ReplayTouchAction::Up,
       .fingerId = 7,
       .songTimeMicros = 5500,
       .x = 0.5F,
       .y = 0.25F},
  };
  value.laneCoverEvents = {
      {.songTimeMicros = 6000,
       .noteStartPositionPercent = 41,
       .resetVisibleTimeReference = false},
      {.songTimeMicros = 6500,
       .noteStartPositionPercent = 53,
       .resetVisibleTimeReference = true},
  };
  return value;
}

void testIndependentBeatorajaFixtures() {
  // Generated by JudgedPlaybackData.shrink() and libGDX Json from Beatoraja
  // commit 5f46fe198e88abbefe9215ca2de397aef8f54bd8. Source paths:
  // src/bms/player/beatoraja/JudgedPlaybackData.java
  // src/bms/player/beatoraja/PlayDataAccessor.java
  // src/bms/player/beatoraja/input/KeyInputLog.java
  // SHA-256:
  // chart  c4f2a22b571a9bc31f5df0290ce9ab80cd39ad42a8c27d708707f8fe7f170ba7
  // course 75fce78c355a62cf9b21a5f971e019a5682a130c0f941ac2e4ced233ad2d0b08
  // keys   ec101f21efe5c18fb8562ca495f034ccfa75dbab304ac00de647fd3691a8bc1d
  replay::BeatorajaReplayCodec codec;
  const auto chartBytes = readFixture("beatoraja-chart.brd");
  const auto chart = codec.decode(chartBytes, 7);
  expect(chart.chart.has_value(), "stock Beatoraja chart fixture decodes");
  expect(!chart.course.has_value(), "chart fixture is not a course");
  expect(chart.stockOnly, "stock fixture is identified as stock-only");
  expect(!chart.unsupportedAsoExtension,
         "stock fixture has no unsupported extension");
  expect(chart.stageSources ==
                 std::vector{replay::ReplayStageDecodeSource::Stock} &&
             !chart.replayPathHasUndefinedLongNotes().has_value(),
         "stock chart reports that extension-owned setup facts are unknown");
  expect(chart.diagnostic.empty(), "stock chart fixture has no diagnostic");
  if (chart.chart) {
    const auto &value = *chart.chart;
    expectEqual(value.setup.chartSha256, std::string(64, 'a'),
                "stock SHA-256 maps exactly");
    expectEqual(value.setup.keyMode, 7, "expected chart mode drives key map");
    expectEqual(value.setup.longNoteMode, 2,
                "stock CN mode restores the application value");
    expectEqual(value.setup.randomValues, std::vector<int>({4, 2, 7}),
                "stock RANDOM sequence maps exactly");
    expectEqual(value.setup.playOption, std::optional<std::string>("RANDOM"),
                "stock random option maps exactly");
    expectEqual(value.setup.playOptionSeed,
                std::optional<std::int64_t>(0x123456),
                "stock random seed maps exactly");
    expectEqual(value.setup.initialGaugeType, GaugeType::Hard,
                "stock gauge maps exactly");
    expectEqual(value.setup.startingGaugePercent, 100.0F,
                "stock HARD gauge starts at one hundred percent");
    expectEqual(value.setup.playbackRulesetId, std::string("beatoraja"),
                "stock replay selects Beatoraja interpretation");
    expectEqual(value.setup.playbackRulesetRevision, 2,
                "stock replay selects the supported Beatoraja revision");
    expectEqual(value.setup.playbackMode, audio::PlaybackMode::PitchShift,
                "stock replay uses normal pitch-shift playback");
    expectEqual(value.setup.candidateSelection,
                gameplay::CandidateSelectionMode::Lowest,
                "stock replay uses Beatoraja's lowest-note selection");
    expectEqual(value.setup.initialLaneCoverPercent, 37,
                "stock config lane cover maps exactly");
    expect(value.setup.laneCoverEnabled,
           "stock config lane cover enable defaults correctly");
    expectEqual(value.input.size(), std::size_t(6),
                "stock lane and both scratch directions decode");
    if (value.input.size() == 6) {
      expectEqual(value.input[2].control.kind,
                  replay::LogicalControlKind::ScratchClockwise,
                  "first stock scratch code keeps direction");
      expectEqual(value.input[4].control.kind,
                  replay::LogicalControlKind::ScratchCounterClockwise,
                  "second stock scratch code keeps direction");
    }
  }

  const auto innerExpected = readFixture("beatoraja-keyinput.bin");
  const auto chartJson = outerJson(chartBytes);
  std::string diagnostic;
  const auto innerCompressed = replay::base64UrlDecodeBounded(
      chartJson.at("keyinput").get<std::string>(), 1024, diagnostic);
  expect(innerCompressed.has_value(), "stock Base64URL keyinput decodes");
  const auto inner =
      innerCompressed
          ? replay::gzipDecompressBounded(*innerCompressed, 1024, diagnostic)
          : std::nullopt;
  expectEqual(inner, std::optional<Bytes>(innerExpected),
              "stock key records match Java fixture byte-for-byte");

  const auto course = codec.decode(readFixture("beatoraja-course.brd"), 7);
  expect(course.course.has_value(), "stock Beatoraja course fixture decodes");
  expect(!course.chart.has_value(), "course fixture is not a chart");
  expect(course.stockOnly, "stock course fixture is stock-only");
  if (course.course) {
    expectEqual(course.course->stages.size(), std::size_t(2),
                "course fixture preserves both stages");
    expectEqual(course.course->stages[0].setup.chartSha256,
                std::string(64, 'a'), "first course stage SHA maps");
    expectEqual(course.course->stages[1].setup.chartSha256,
                std::string(64, 'b'), "second course stage SHA maps");
    expectEqual(course.course->stages[1].setup.playOption,
                std::optional<std::string>("R-RANDOM"),
                "second course stage option maps");
  }
}

void testDoublePlayOptions() {
  replay::BeatorajaReplayCodec codec;
  Json stockFlip = outerJson(readFixture("beatoraja-chart.brd"));
  stockFlip["doubleoption"] = 1;
  const auto decodedFlip = codec.decode(encodeJson(stockFlip), 14);
  expect(decodedFlip.chart.has_value(), "stock Beatoraja DP FLIP decodes");
  if (decodedFlip.chart) {
    expectEqual(decodedFlip.chart->setup.doublePlayOption,
                replay::DoublePlayOption::Flip,
                "stock Beatoraja DP FLIP maps exactly");
  }

  Json stockBattle = stockFlip;
  stockBattle["doubleoption"] = 2;
  const auto decodedBattle = codec.decode(encodeJson(stockBattle), 14);
  expect(!decodedBattle.chart.has_value() &&
             decodedBattle.diagnostic.find("double-play option") !=
                 std::string::npos,
         "unsupported Beatoraja BATTLE replay fails closed");

  auto source = extensionReplay();
  source.setup.keyMode = 14;
  source.setup.doublePlayOption = replay::DoublePlayOption::Flip;
  std::string diagnostic;
  const auto encoded =
      codec.encodeChart(source, 1'725'000'000'123LL, diagnostic);
  expect(encoded.has_value(), "Aso DP FLIP replay encodes");
  if (encoded) {
    expectEqual(outerJson(*encoded).at("doubleoption").get<int>(), 1,
                "Aso DP FLIP projects to the stock Beatoraja field");
    const auto decoded = codec.decode(*encoded, 14);
    expect(decoded.chart.has_value() && *decoded.chart == source,
           "Aso DP FLIP setup round-trips through the extension");
  }
}

void expectMapping(int keyMode, const replay::LogicalControl &logical,
                   int keyCode, std::string_view message) {
  const auto encoded =
      replay::BeatorajaReplayCodec::beatorajaKeyCode(logical, keyMode);
  expectEqual(encoded, std::optional<int>(keyCode), message);
  if (encoded) {
    expectEqual(replay::BeatorajaReplayCodec::logicalControl(*encoded, keyMode),
                std::optional<replay::LogicalControl>(logical),
                "stock key code reverses to logical control");
  }
}

void testKeyModeTables() {
  expectMapping(5, lane(1, 0), 0, "5-key first lane maps");
  expectMapping(5, lane(1, 4), 4, "5-key last lane maps");
  expectMapping(5, control(replay::LogicalControlKind::ScratchClockwise, 1), 5,
                "5-key clockwise scratch maps");
  expectMapping(5,
                control(replay::LogicalControlKind::ScratchCounterClockwise, 1),
                6, "5-key counter-clockwise scratch maps");

  expectMapping(7, lane(1, 6), 6, "7-key last lane maps");
  expectMapping(7, control(replay::LogicalControlKind::ScratchClockwise, 1), 7,
                "7-key clockwise scratch maps");
  expectMapping(7,
                control(replay::LogicalControlKind::ScratchCounterClockwise, 1),
                8, "7-key counter-clockwise scratch maps");

  expectMapping(9, lane(1, 0), 0, "9-key first lane maps");
  expectMapping(9, lane(1, 8), 8, "9-key last lane maps");
  expect(!replay::BeatorajaReplayCodec::beatorajaKeyCode(
             control(replay::LogicalControlKind::ScratchClockwise, 1), 9),
         "9-key has no scratch code");

  expectMapping(10, lane(1, 4), 4, "10-key 1P last lane maps");
  expectMapping(10, lane(2, 0), 7, "10-key 2P first lane maps");
  expectMapping(10, lane(2, 4), 11, "10-key 2P last lane maps");
  expectMapping(10, control(replay::LogicalControlKind::ScratchClockwise, 2),
                12, "10-key 2P clockwise scratch maps");
  expectMapping(10,
                control(replay::LogicalControlKind::ScratchCounterClockwise, 2),
                13, "10-key 2P counter-clockwise scratch maps");

  expectMapping(14, lane(1, 6), 6, "14-key 1P last lane maps");
  expectMapping(14, lane(2, 0), 9, "14-key 2P first lane maps");
  expectMapping(14, lane(2, 6), 15, "14-key 2P last lane maps");
  expectMapping(14, control(replay::LogicalControlKind::ScratchClockwise, 2),
                16, "14-key 2P clockwise scratch maps");
  expectMapping(14,
                control(replay::LogicalControlKind::ScratchCounterClockwise, 2),
                17, "14-key 2P counter-clockwise scratch maps");

  expect(!replay::BeatorajaReplayCodec::beatorajaKeyCode(
             control(replay::LogicalControlKind::Start, 1), 7),
         "stock Beatoraja keylog has no Start code");
  expect(!replay::BeatorajaReplayCodec::beatorajaKeyCode(
             control(replay::LogicalControlKind::Select, 1), 7),
         "stock Beatoraja keylog has no Select code");
  expect(!replay::BeatorajaReplayCodec::logicalControl(99, 7),
         "unknown stock key code is rejected");
}

void testPrimitiveCodecs() {
  const Bytes source = bytes("gzip and Base64URL fixture payload");
  std::string diagnostic;
  const auto compressed = replay::gzipCompress(source, diagnostic);
  expect(compressed.has_value(), "gzip compression succeeds");
  if (compressed) {
    expect(compressed->size() >= 18, "gzip output has framing and trailer");
    expect((*compressed)[0] == std::byte{0x1f} &&
               (*compressed)[1] == std::byte{0x8b},
           "gzip output has stock-compatible magic");
    expectEqual(
        replay::gzipDecompressBounded(*compressed, source.size(), diagnostic),
        std::optional<Bytes>(source), "bounded gzip round-trips");
    expect(!replay::gzipDecompressBounded(*compressed, source.size() - 1,
                                          diagnostic),
           "bounded gzip rejects expansion past the limit");

    Bytes corrupt = *compressed;
    corrupt.back() ^= std::byte{0x01};
    expect(!replay::gzipDecompressBounded(corrupt, 1024, diagnostic),
           "gzip rejects a corrupt trailer");
  }
  expect(!replay::gzipDecompressBounded(bytes("not gzip"), 1024, diagnostic),
         "gzip rejects malformed framing");

  const Bytes binary = {std::byte{0xfb}, std::byte{0xff}};
  expectEqual(replay::base64UrlEncode(binary), std::string("-_8="),
              "Base64URL uses URL-safe padded alphabet");
  expectEqual(replay::base64UrlDecodeBounded("-_8=", 2, diagnostic),
              std::optional<Bytes>(binary), "padded Base64URL decodes");
  expectEqual(replay::base64UrlDecodeBounded("-_8", 2, diagnostic),
              std::optional<Bytes>(binary), "unpadded Base64URL decodes");
  expect(!replay::base64UrlDecodeBounded("+/8=", 2, diagnostic),
         "non-URL-safe Base64 alphabet is rejected");
  expect(!replay::base64UrlDecodeBounded("-_9", 2, diagnostic),
         "non-canonical Base64URL tail bits are rejected");
  expect(!replay::base64UrlDecodeBounded("-_8=\n", 2, diagnostic),
         "Base64URL whitespace is rejected");
  expect(!replay::base64UrlDecodeBounded("-_8=", 1, diagnostic),
         "Base64URL output bound is enforced before allocation");
}

void testAsoExtensionRoundTripsWithoutBreakingStock() {
  replay::BeatorajaReplayCodec codec;
  const auto source = extensionReplay();
  std::string diagnostic;
  const auto encoded =
      codec.encodeChart(source, 1'725'000'000'123LL, diagnostic);
  expect(encoded.has_value(), "Aso chart replay encodes");
  const auto encodedAgain =
      codec.encodeChart(source, 1'725'000'000'123LL, diagnostic);
  expectEqual(encodedAgain, encoded,
              "deterministic replay encoding supports idempotent retry");
  if (!encoded) {
    return;
  }
  if (const char *compatibilityOutput =
          std::getenv("ASOBMASHOW_CODEC_FIXTURE_OUTPUT");
      compatibilityOutput != nullptr && compatibilityOutput[0] != '\0') {
    std::ofstream stream(compatibilityOutput, std::ios::binary);
    stream.write(reinterpret_cast<const char *>(encoded->data()),
                 static_cast<std::streamsize>(encoded->size()));
    expect(stream.good(), "optional Java compatibility fixture is written");
  }

  const Json stock = outerJson(*encoded);
  expect(stock.is_object(), "stock reader sees the chart object envelope");
  expect(stock.contains("sha256") && stock.contains("mode") &&
             stock.contains("keyinput") && stock.contains("gauge") &&
             stock.contains("config"),
         "stock reader sees all required stock fields");
  expect(stock.contains("asobmashow"), "Aso extension is namespaced");
  // This deliberately models Beatoraja's setIgnoreUnknownFields(true): only
  // stock fields are consumed, and the extension is never consulted.
  expectEqual(stock.at("sha256").get<std::string>(), source.setup.chartSha256,
              "stock-compatible reader ignores extension and reads SHA");
  expectEqual(stock.at("mode").get<int>(), 1,
              "application CN projects to Beatoraja stock mode one");
  expectEqual(stock.at("config").at("lanecover").get<float>(), 0.37F,
              "initial cover remains in stock config");
  const auto expectedStockRecords = readFixture("beatoraja-keyinput.bin");
  const auto encodedStockKeyGzip = replay::base64UrlDecodeBounded(
      stock.at("keyinput").get<std::string>(), 1024, diagnostic);
  const auto encodedStockRecords =
      encodedStockKeyGzip ? replay::gzipDecompressBounded(*encodedStockKeyGzip,
                                                          1024, diagnostic)
                          : std::nullopt;
  expectEqual(
      encodedStockRecords, std::optional<Bytes>(expectedStockRecords),
      "Aso encoder emits Java-compatible signed little-endian key records");

  const auto decoded = codec.decode(*encoded);
  expect(decoded.chart.has_value(), "supported Aso chart decodes");
  expect(!decoded.stockOnly, "supported extension is not stock-only");
  expect(!decoded.unsupportedAsoExtension,
         "supported extension is not flagged unsupported");
  expect(decoded.stageSources ==
                 std::vector{replay::ReplayStageDecodeSource::AsoExtension} &&
             decoded.replayPathHasUndefinedLongNotes() == true,
         "supported chart reports extension-owned setup facts as known");
  if (decoded.chart) {
    expectEqual(
        *decoded.chart, source,
        "Aso playback setup, raw input, touch, and timed cover round-trip");
    const auto &starting = decoded.chart->setup.startingGaugeState;
    expect(starting.has_value() && starting->gaugeType == GaugeType::Hard &&
               starting->currentGauge == 100.0F &&
               starting->gaugeValues[gaugeTypeIndex(GaugeType::Normal)] ==
                   20.0F &&
               starting->gaugeValues[gaugeTypeIndex(GaugeType::Hard)] ==
                   100.0F &&
               starting->gaugeSurvivalFailed[gaugeTypeIndex(
                   GaugeType::ExHard)],
           "Aso extension preserves independent starting gauge state");
  }

  auto hcn = source;
  hcn.setup.longNoteMode = 3;
  const auto encodedHcn =
      codec.encodeChart(hcn, 1'725'000'000'123LL, diagnostic);
  expect(encodedHcn.has_value(), "application HCN replay encodes");
  if (encodedHcn) {
    expectEqual(outerJson(*encodedHcn).at("mode").get<int>(), 2,
                "application HCN projects to Beatoraja stock mode two");
    const auto decodedHcn = codec.decode(*encodedHcn);
    expect(decodedHcn.chart.has_value() &&
               decodedHcn.chart->setup.longNoteMode == 3,
           "application HCN round-trips through BRD");
  }

  replay::CourseReplayPlaybackData course;
  course.stages = {source, source};
  course.stages[1].setup.chartSha256 = std::string(64, 'b');
  course.restMicrosAfterStage = {2'000'000, 0};
  const auto courseEncoded =
      codec.encodeCourse(course, 1'725'000'000'123LL, diagnostic);
  expect(courseEncoded.has_value(), "Aso course replay encodes");
  if (courseEncoded) {
    const Json stockCourse = outerJson(*courseEncoded);
    expect(stockCourse.is_array() && stockCourse.size() == 2,
           "stock reader sees a normal two-stage JudgedPlaybackData array");
    const auto courseDecoded = codec.decode(*courseEncoded);
    expect(courseDecoded.course.has_value(), "supported Aso course decodes");
    expect(courseDecoded.stageSources ==
                   std::vector{
                       replay::ReplayStageDecodeSource::AsoExtension,
                       replay::ReplayStageDecodeSource::AsoExtension} &&
               courseDecoded.replayPathHasUndefinedLongNotes() == true,
           "supported course aggregates its known undefined-LN facts");
    if (courseDecoded.course) {
      expectEqual(*courseDecoded.course, course,
                  "course stages and inter-stage rests round-trip");
    }
  }
}

void testReplayOnlyScratchHandoffExtensionIsStrictAndStockCompatible() {
  replay::BeatorajaReplayCodec codec;
  auto marked = extensionReplay();
  marked.input = {
      {.songTimeMicros = 100,
       .control =
           control(replay::LogicalControlKind::ScratchCounterClockwise, 1),
       .pressed = true},
      {.songTimeMicros = 200,
       .control =
           control(replay::LogicalControlKind::ScratchCounterClockwise, 1),
       .pressed = false,
       .replayOnly = true},
      {.songTimeMicros = 200, .control = lane(1, 0), .pressed = true},
      {.songTimeMicros = 200,
       .control = control(replay::LogicalControlKind::ScratchClockwise, 1),
       .pressed = true,
       .replayOnly = true},
      {.songTimeMicros = 250, .control = lane(1, 0), .pressed = false},
      {.songTimeMicros = 300,
       .control = control(replay::LogicalControlKind::ScratchClockwise, 1),
       .pressed = false},
  };
  auto physical = marked;
  physical.input[1].replayOnly = false;
  physical.input[3].replayOnly = false;

  std::string diagnostic;
  const auto markedBytes = codec.encodeChart(marked, 1000, diagnostic);
  const auto physicalBytes = codec.encodeChart(physical, 1000, diagnostic);
  expect(markedBytes.has_value() && physicalBytes.has_value(),
         "valid marked and physical scratch handoffs encode");
  if (!markedBytes || !physicalBytes) {
    return;
  }

  const Json markedJson = outerJson(*markedBytes);
  const Json physicalJson = outerJson(*physicalBytes);
  expectEqual(stockKeyRecords(markedJson), stockKeyRecords(physicalJson),
              "replay-only metadata does not change stock keyinput");
  expect(markedJson["asobmashow"]["input"][1].value("replayOnly", false) &&
             markedJson["asobmashow"]["input"][3].value("replayOnly", false),
         "only the Aso input extension carries replay-only metadata");
  const auto decoded = codec.decode(*markedBytes, 7);
  expect(decoded.chart.has_value() && *decoded.chart == marked,
         "valid replay-only scratch handoff round-trips exactly");

  Json legacyExtension = markedJson;
  for (auto &item : legacyExtension["asobmashow"]["input"]) {
    item.erase("replayOnly");
  }
  const auto decodedLegacy = codec.decode(encodeJson(legacyExtension), 7);
  expect(decodedLegacy.chart.has_value() &&
             std::ranges::none_of(decodedLegacy.chart->input,
                                  &replay::InputTransition::replayOnly),
         "missing replay-only extension fields default to false");

  Json wrongType = markedJson;
  wrongType["asobmashow"]["input"][1]["replayOnly"] = "true";
  const auto wrongTypeDecoded = codec.decode(encodeJson(wrongType), 7);
  expect(!wrongTypeDecoded.chart && !wrongTypeDecoded.course &&
             !wrongTypeDecoded.diagnostic.empty(),
         "wrong-type replay-only marker is rejected");

  const auto rejected = [&](replay::ReplayPlaybackData candidate,
                            std::string_view message) {
    diagnostic.clear();
    expect(!codec.encodeChart(candidate, 1000, diagnostic), message);
    expect(!diagnostic.empty(),
           "invalid replay-only handoff reports an encode diagnostic");
  };

  auto arbitraryLane = marked;
  arbitraryLane.input = {
      {.songTimeMicros = 100, .control = lane(1, 0), .pressed = true},
      {.songTimeMicros = 200,
       .control = lane(1, 0),
       .pressed = false,
       .replayOnly = true},
      {.songTimeMicros = 200,
       .control = lane(1, 0),
       .pressed = true,
       .replayOnly = true},
  };
  rejected(arbitraryLane,
           "replay-only marker cannot suppress arbitrary lane input");

  auto mismatchedTime = marked;
  mismatchedTime.input[3].songTimeMicros = 201;
  rejected(mismatchedTime, "replay-only scratch pair must share one timestamp");

  auto sameDirection = marked;
  sameDirection.input[3].control = sameDirection.input[1].control;
  sameDirection.input[5].control = sameDirection.input[1].control;
  rejected(sameDirection,
           "replay-only scratch pair must change logical direction");

  auto markedInitialPress = marked;
  markedInitialPress.input[0].replayOnly = true;
  rejected(markedInitialPress,
           "replay-only scratch pair must begin by releasing a held owner");
}

void testPreRollInputEncodesForChartsAndCourses() {
  replay::BeatorajaReplayCodec codec;
  auto source = extensionReplay();
  source.input[0].songTimeMicros = -2'000'000;
  source.input[1].songTimeMicros = -1'500'000;
  source.touchSamples[0].songTimeMicros = -1'000'000;
  source.touchSamples[1].songTimeMicros = -900'000;
  source.laneCoverEvents[0].songTimeMicros = -800'000;
  source.laneCoverEvents[1].songTimeMicros = -700'000;
  std::string diagnostic;

  const auto chart = codec.encodeChart(source, 1'725'000'000'123LL, diagnostic);
  expect(chart.has_value(), "chart replay accepts recorder pre-roll input");
  if (chart.has_value()) {
    const auto stockRecords = stockKeyRecords(outerJson(*chart));
    bool stockTimesAreNonnegative = stockRecords.has_value();
    if (stockRecords) {
      for (std::size_t offset = 0; offset < stockRecords->size(); offset += 9) {
        std::uint64_t rawTime = 0;
        for (int shift = 0; shift < 64; shift += 8) {
          rawTime |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(
                         (*stockRecords)[offset + 1 + shift / 8]))
                     << shift;
        }
        stockTimesAreNonnegative &= static_cast<std::int64_t>(rawTime) >= 0;
      }
    }
    expect(stockTimesAreNonnegative,
           "Beatoraja stock key records exclude negative pre-roll times");
    const auto decoded = codec.decode(*chart, 7);
    expect(decoded.chart.has_value() &&
               decoded.chart->input[0].songTimeMicros == -2'000'000 &&
               decoded.chart->input[1].songTimeMicros == -1'500'000 &&
               decoded.chart->touchSamples[0].songTimeMicros == -1'000'000 &&
               decoded.chart->laneCoverEvents[0].songTimeMicros == -800'000,
           "chart replay preserves pre-roll input and setup timestamps");
  }

  replay::CourseReplayPlaybackData course;
  course.stages = {source};
  course.restMicrosAfterStage = {0};
  const auto encodedCourse =
      codec.encodeCourse(course, 1'725'000'000'123LL, diagnostic);
  expect(encodedCourse.has_value(),
         "course replay accepts recorder pre-roll input");
  if (encodedCourse.has_value()) {
    const auto decoded = codec.decode(*encodedCourse, 7);
    expect(decoded.course.has_value() &&
               decoded.course->stages[0].input[0].songTimeMicros == -2'000'000,
           "course replay preserves pre-roll input timestamps");
  }

  source.input[0].songTimeMicros = -30'000'001;
  expect(!codec.encodeChart(source, 1'725'000'000'123LL, diagnostic),
         "input before the recorder pre-roll remains invalid");
}

void testStockProjectionCarriesPreRollHoldsAcrossTimeZero() {
  replay::BeatorajaReplayCodec codec;
  auto source = extensionReplay();
  source.input = {
      {.songTimeMicros = -2'000'000, .control = lane(1, 0), .pressed = true},
      {.songTimeMicros = 1'000, .control = lane(1, 0), .pressed = false},
  };
  std::string diagnostic;

  const auto encoded =
      codec.encodeChart(source, 1'725'000'000'123LL, diagnostic);
  expect(encoded.has_value(), "pre-roll hold replay encodes");
  if (!encoded) {
    return;
  }

  Bytes expectedStock;
  appendRecord(expectedStock, 1, 0);
  appendRecord(expectedStock, -1, 1'000);
  expectEqual(stockKeyRecords(outerJson(*encoded)),
              std::optional<Bytes>(expectedStock),
              "stock replay starts controls held across time zero");

  const auto decoded = codec.decode(*encoded, 7);
  expect(decoded.chart.has_value() && decoded.chart->input == source.input,
         "Aso extension keeps the exact negative pre-roll transition");
}

void testEncodeValidatesRandomAndGaugeEnums() {
  replay::BeatorajaReplayCodec codec;
  std::string diagnostic;
  const auto rejected = [&](replay::ReplayPlaybackData candidate,
                            std::string_view message) {
    diagnostic.clear();
    expect(!codec.encodeChart(candidate, 1'000, diagnostic), message);
    expect(!diagnostic.empty(),
           "invalid replay setup reports an encode diagnostic");
  };

  auto supportedPrng = extensionReplay();
  expect(codec.encodeChart(supportedPrng, 1'000, diagnostic).has_value(),
         "the parser-declared replay PRNG remains encodable");

  auto missingLaneCoverState = extensionReplay();
  missingLaneCoverState.setup.initialLaneCoverPercent.reset();
  rejected(missingLaneCoverState,
           "a durable replay cannot omit its initial lane-cover state");

  auto resolvedGaugeProfile = extensionReplay();
  resolvedGaugeProfile.setup.keyMode = 5;
  resolvedGaugeProfile.setup.gaugeProfile = GaugeProfile::Standard;
  resolvedGaugeProfile.setup.startingGaugeState->gaugeProfile =
      GaugeProfile::Standard5Keys;
  expect(codec.encodeChart(resolvedGaugeProfile, 1'000, diagnostic).has_value(),
         "a production gauge snapshot may carry the key-mode-resolved "
         "profile");

  auto carriedPmsGauge = extensionReplay();
  carriedPmsGauge.setup.keyMode = 9;
  carriedPmsGauge.setup.initialGaugeType = GaugeType::Normal;
  carriedPmsGauge.setup.gaugeProfile = GaugeProfile::Standard9Keys;
  carriedPmsGauge.setup.gaugeAutoShift = GaugeAutoShiftMode::None;
  carriedPmsGauge.setup.playbackRulesetId = "beatoraja";
  carriedPmsGauge.setup.playbackRulesetRevision =
      RulesetDescriptor::For(GameplayRuleset::Beatoraja).version;
  carriedPmsGauge.setup.startingGaugePercent = 120.0F;
  carriedPmsGauge.setup.startingGaugeState = GaugeStateSnapshot{
      .gaugeType = GaugeType::Normal,
      .selectedGaugeType = GaugeType::Normal,
      .gaugeAutoShiftLowerBound = GaugeType::Easy,
      .gaugeProfile = GaugeProfile::Standard9Keys,
      .gaugeAutoShift = GaugeAutoShiftMode::None,
      .currentGauge = 120.0F,
  };
  carriedPmsGauge.setup.startingGaugeState->gaugeValues = {
      30.0F, 30.0F, 120.0F, 100.0F, 100.0F, 100.0F};
  carriedPmsGauge.input = {
      {.songTimeMicros = 1'000, .control = lane(1, 0), .pressed = true},
      {.songTimeMicros = 2'000, .control = lane(1, 0), .pressed = false},
  };
  auto initialPmsGauge = carriedPmsGauge;
  initialPmsGauge.setup.startingGaugePercent = 30.0F;
  initialPmsGauge.setup.startingGaugeState->currentGauge = 30.0F;
  initialPmsGauge.setup.startingGaugeState
      ->gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 30.0F;
  carriedPmsGauge.setup.chartSha256 = std::string(64, 'b');
  replay::CourseReplayPlaybackData pmsCourse{
      .stages = {initialPmsGauge, carriedPmsGauge},
      .restMicrosAfterStage = {0, 0},
  };
  expect(codec.encodeCourse(pmsCourse, 1'000, diagnostic).has_value(),
         "course encode accepts a carried 9-key groove gauge at its 120 "
         "percent profile maximum");

  auto lr2CourseStage = extensionReplay();
  lr2CourseStage.setup.playbackRulesetId =
      RulesetDescriptor::Current().id;
  lr2CourseStage.setup.playbackRulesetRevision =
      RulesetDescriptor::Current().version;
  lr2CourseStage.setup.initialGaugeType = GaugeType::Normal;
  lr2CourseStage.setup.gaugeProfile = GaugeProfile::CourseDefault;
  lr2CourseStage.setup.gaugeAutoShift = GaugeAutoShiftMode::None;
  lr2CourseStage.setup.gaugeAutoShiftLowerBound =
      GaugeType::AssistedEasy;
  lr2CourseStage.setup.startingGaugePercent = 100.0F;
  lr2CourseStage.setup.startingGaugeState = GaugeStateSnapshot{
      .gaugeType = GaugeType::Normal,
      .selectedGaugeType = GaugeType::Normal,
      .gaugeAutoShiftLowerBound = GaugeType::AssistedEasy,
      .gaugeProfile = GaugeProfile::CourseLR2,
      .gaugeAutoShift = GaugeAutoShiftMode::None,
      .currentGauge = 100.0F,
  };
  lr2CourseStage.setup.startingGaugeState->gaugeValues.fill(100.0F);
  replay::CourseReplayPlaybackData lr2Course{
      .stages = {lr2CourseStage},
      .restMicrosAfterStage = {0},
  };
  expect(codec.encodeCourse(lr2Course, 1'000, diagnostic).has_value(),
         "course encode accepts the production LR2 snapshot resolution of a "
         "generic course gauge profile");

  auto maximumRandomValues = extensionReplay();
  maximumRandomValues.setup.randomValues.assign(100'000, 0);
  expect(codec.encodeChart(maximumRandomValues, 1'000, diagnostic).has_value(),
         "encode accepts exactly one hundred thousand RANDOM values");

  auto excessiveRandomValues = extensionReplay();
  excessiveRandomValues.setup.randomValues.assign(100'001, 0);
  rejected(excessiveRandomValues,
           "encode rejects more than one hundred thousand RANDOM values");

  auto unsupportedPrng = extensionReplay();
  unsupportedPrng.setup.randomPrng = "unsupported-prng";
  rejected(unsupportedPrng,
           "encode rejects a present PRNG unsupported by the parser");

  auto invalidInitialGauge = extensionReplay();
  invalidInitialGauge.setup.startingGaugeState.reset();
  invalidInitialGauge.setup.initialGaugeType = static_cast<GaugeType>(99);
  rejected(invalidInitialGauge,
           "encode rejects an out-of-range initial gauge type");

  auto invalidGaugeLowerBound = extensionReplay();
  invalidGaugeLowerBound.setup.startingGaugeState.reset();
  invalidGaugeLowerBound.setup.gaugeAutoShiftLowerBound =
      static_cast<GaugeType>(99);
  rejected(invalidGaugeLowerBound,
           "encode rejects an out-of-range gauge auto-shift lower bound");

  auto invalidStartingGauge = extensionReplay();
  invalidStartingGauge.setup.startingGaugeState->gaugeType =
      static_cast<GaugeType>(99);
  invalidStartingGauge.setup.startingGaugeState->currentGauge = 20.0F;
  rejected(invalidStartingGauge,
           "encode rejects an out-of-range active starting gauge type");

  auto invalidLegacyGauge = extensionReplay();
  invalidLegacyGauge.legacy = replay::LegacyPlaybackTrack{
      .events = {{.action = replay::LegacyPlaybackAction::Gauge,
                  .songTimeMicros = 1'000,
                  .gauge = 20.0F,
                  .gaugeType = static_cast<GaugeType>(99)}}};
  rejected(invalidLegacyGauge,
           "encode rejects an out-of-range legacy gauge type");
}

void testCourseEncodeRejectsNegativeRestDuration() {
  replay::BeatorajaReplayCodec codec;
  replay::CourseReplayPlaybackData course;
  course.stages = {extensionReplay()};
  course.restMicrosAfterStage = {-1};
  std::string diagnostic;

  expect(!codec.encodeCourse(course, 1'000, diagnostic),
         "course encode rejects a negative post-stage rest duration");
  expect(!diagnostic.empty(),
         "negative post-stage rest reports an encode diagnostic");
}

void testAllMissReplayRoundTripsWithEmptyInput() {
  replay::BeatorajaReplayCodec codec;
  auto source = extensionReplay();
  source.input.clear();
  std::string diagnostic;

  const auto chart = codec.encodeChart(source, 1'725'000'000'123LL, diagnostic);
  expect(chart.has_value(), "all-miss chart replay encodes with empty input");
  if (chart.has_value()) {
    Bytes sentinel;
    appendRecord(sentinel, -1, 0);
    expectEqual(stockKeyRecords(outerJson(*chart)),
                std::optional<Bytes>(sentinel),
                "all-miss stock replay contains a harmless release sentinel");
    const auto decoded = codec.decode(*chart, 7);
    expect(decoded.chart.has_value() && decoded.chart->input.empty(),
           "all-miss chart replay decodes with empty input");
  }

  replay::CourseReplayPlaybackData course;
  course.stages = {source};
  course.restMicrosAfterStage = {0};
  const auto encodedCourse =
      codec.encodeCourse(course, 1'725'000'000'123LL, diagnostic);
  expect(encodedCourse.has_value(),
         "all-miss course stage encodes with empty input");
  if (encodedCourse.has_value()) {
    const auto decoded = codec.decode(*encodedCourse, 7);
    expect(decoded.course.has_value() &&
               decoded.course->stages.front().input.empty(),
           "all-miss course stage decodes with empty input");
  }
}

void testManualAssignmentProjectsToStockNormal() {
  replay::BeatorajaReplayCodec codec;
  auto source = extensionReplay();
  source.setup.playOption = "ASSIGN:S2134567";
  source.setup.playOptionSeed.reset();
  source.input = {
      {.songTimeMicros = 1000, .control = lane(1, 0), .pressed = true},
      {.songTimeMicros = 1500, .control = lane(1, 0), .pressed = false},
      {.songTimeMicros = 2000,
       .control =
           control(replay::LogicalControlKind::ScratchCounterClockwise, 1),
       .pressed = true},
      {.songTimeMicros = 2500,
       .control =
           control(replay::LogicalControlKind::ScratchCounterClockwise, 1),
       .pressed = false},
  };

  std::string diagnostic;
  const auto encoded =
      codec.encodeChart(source, 1'725'000'000'123LL, diagnostic);
  expect(encoded.has_value(),
         "manual assignment replay encodes through the Aso extension");
  if (encoded.has_value()) {
    const Json stock = outerJson(*encoded);
    expectEqual(stock.at("randomoption").get<int>(), 0,
                "manual assignment projects to stock NORMAL");

    const auto compressed = replay::base64UrlDecodeBounded(
        stock.at("keyinput").get<std::string>(), 1024, diagnostic);
    const auto records =
        compressed
            ? replay::gzipDecompressBounded(*compressed, 1024, diagnostic)
            : std::nullopt;
    Bytes expected;
    appendRecord(expected, 2, 1000);
    appendRecord(expected, -2, 1500);
    appendRecord(expected, 9, 2000);
    appendRecord(expected, -9, 2500);
    expectEqual(records, std::optional<Bytes>(expected),
                "stock input maps assigned destinations to source lanes");

    const auto decoded = codec.decode(*encoded, 7);
    expect(decoded.chart.has_value(), "manual assignment replay decodes");
    if (decoded.chart.has_value()) {
      expectEqual(decoded.chart->setup.playOption, source.setup.playOption,
                  "Aso extension keeps the exact manual assignment");
      expectEqual(decoded.chart->input, source.input,
                  "Aso extension keeps original assigned input");
    }
  }

  for (const auto &invalid : std::array{
           std::pair{7, "ASSIGN:S123456"},
           std::pair{7, "ASSIGN:S1134567"},
           std::pair{7, "ASSIGN:S1234568"},
           std::pair{7, "ASSIGN:S12345"},
           std::pair{9, "ASSIGN:123456789"},
       }) {
    auto candidate = extensionReplay();
    candidate.setup.keyMode = invalid.first;
    candidate.setup.playOption = invalid.second;
    candidate.input.clear();
    diagnostic.clear();
    expect(!codec.encodeChart(candidate, 1'725'000'000'123LL, diagnostic),
           "invalid or unsupported manual assignment is rejected");
  }
}

Json minimalStock(std::string keyInput, Json extension = nullptr) {
  Json document = {
      {"player", "fixture"},
      {"sha256", std::string(64, 'a')},
      {"mode", 0},
      {"keyinput", std::move(keyInput)},
      {"gauge", 2},
      {"rand", Json::array()},
      {"date", 1},
      {"randomoption", 0},
      {"randomoptionseed", -1},
      {"config", {{"lanecover", 0.2}, {"enablelanecover", true}}},
  };
  if (!extension.is_null()) {
    document["asobmashow"] = std::move(extension);
  }
  return document;
}

std::string encodedInner(std::span<const std::byte> records) {
  std::string diagnostic;
  const auto compressed = replay::gzipCompress(records, diagnostic);
  expect(compressed.has_value(), "test inner gzip compression succeeds");
  return compressed ? replay::base64UrlEncode(*compressed) : std::string{};
}

void appendRecord(Bytes &records, std::int8_t signedCode, std::int64_t time) {
  records.push_back(static_cast<std::byte>(signedCode));
  for (int shift = 0; shift < 64; shift += 8) {
    records.push_back(static_cast<std::byte>(
        (static_cast<std::uint64_t>(time) >> shift) & 0xffU));
  }
}

void expectDecodeRejected(replay::BeatorajaReplayCodec &codec,
                          const Bytes &value, std::string_view message,
                          std::optional<int> keyMode = 7) {
  const auto outcome = codec.decode(value, keyMode);
  expect(!outcome.chart && !outcome.course, message);
  expect(!outcome.diagnostic.empty(), "rejected replay reports diagnostic");
}

void testStockDecodeRejectsZeroKeyRecords() {
  replay::BeatorajaReplayCodec codec;
  expectDecodeRejected(
      codec, encodeJson(minimalStock(encodedInner(Bytes{}))),
      "stock replay with zero nine-byte key records is rejected");
}

void expectCourseAggregateRejected(
    const replay::CourseReplayPlaybackData &course,
    const replay::ReplayCodecLimits &limits, std::string_view track) {
  replay::BeatorajaReplayCodec strict(limits);
  std::string diagnostic;
  expect(!strict.encodeCourse(course, 1'000, diagnostic),
         std::string("course encode enforces the aggregate ") +
             std::string(track) + " limit");
  expect(!diagnostic.empty(),
         "aggregate encode rejection reports a diagnostic");

  replay::BeatorajaReplayCodec permissive;
  const auto encoded = permissive.encodeCourse(course, 1'000, diagnostic);
  expect(encoded.has_value(), "aggregate-limit fixture encodes permissively");
  if (!encoded) {
    return;
  }
  const auto decoded = strict.decode(*encoded);
  expect(!decoded.chart && !decoded.course,
         std::string("course decode enforces the aggregate ") +
             std::string(track) + " limit");
  expect(!decoded.diagnostic.empty(),
         "aggregate decode rejection reports a diagnostic");
}

void testCourseCodecEnforcesAggregateTrackLimits() {
  auto stage = extensionReplay();
  stage.input.resize(2);
  stage.touchSamples.clear();
  stage.laneCoverEvents.clear();
  replay::CourseReplayPlaybackData course{.stages = {stage, stage},
                                          .restMicrosAfterStage = {0, 0}};
  replay::ReplayCodecLimits limits;
  limits.maxInputTransitions = 3;
  expectCourseAggregateRejected(course, limits, "input-transition");

  stage.input.clear();
  stage.touchSamples = {{.action = replay::ReplayTouchAction::Down,
                         .fingerId = 1,
                         .songTimeMicros = 1'000,
                         .x = 0.5F,
                         .y = 0.5F}};
  course.stages = {stage, stage};
  limits = {};
  limits.maxTouchSamples = 1;
  expectCourseAggregateRejected(course, limits, "touch-sample");

  stage.touchSamples.clear();
  stage.laneCoverEvents = {{.songTimeMicros = 1'000,
                            .noteStartPositionPercent = 50,
                            .resetVisibleTimeReference = false}};
  course.stages = {stage, stage};
  limits = {};
  limits.maxLaneCoverEvents = 1;
  expectCourseAggregateRejected(course, limits, "lane-cover-event");

  stage.laneCoverEvents.clear();
  stage.legacy = replay::LegacyPlaybackTrack{
      .events = {{.action = replay::LegacyPlaybackAction::Gauge,
                  .songTimeMicros = 1'000,
                  .judgement = Judgement::None,
                  .gauge = 20.0F,
                  .gaugeType = GaugeType::Normal}}};
  course.stages = {stage, stage};
  limits = {};
  limits.maxInputTransitions = 1;
  expectCourseAggregateRejected(course, limits, "legacy-event");
}

void testMalformedAndBoundedInputs() {
  replay::BeatorajaReplayCodec codec;
  expectDecodeRejected(codec, bytes("not gzip"),
                       "malformed outer gzip is rejected");

  Json badBase64 = minimalStock("@@@");
  expectDecodeRejected(codec, encodeJson(badBase64),
                       "invalid inner Base64URL is rejected");
  Json badInnerGzip = minimalStock(replay::base64UrlEncode(bytes("bad gzip")));
  expectDecodeRejected(codec, encodeJson(badInnerGzip),
                       "malformed inner gzip is rejected");

  Json partial = minimalStock(encodedInner(Bytes(8, std::byte{0})));
  expectDecodeRejected(codec, encodeJson(partial),
                       "partial nine-byte key record is rejected");

  Bytes zeroRecord;
  appendRecord(zeroRecord, 0, 1000);
  expectDecodeRejected(codec,
                       encodeJson(minimalStock(encodedInner(zeroRecord))),
                       "zero signed key byte is rejected");
  Bytes minRecord;
  appendRecord(minRecord, std::numeric_limits<std::int8_t>::min(), 1000);
  expectDecodeRejected(codec, encodeJson(minimalStock(encodedInner(minRecord))),
                       "INT8_MIN key byte is rejected");
  Bytes invalidCode;
  appendRecord(invalidCode, 20, 1000);
  expectDecodeRejected(codec,
                       encodeJson(minimalStock(encodedInner(invalidCode))),
                       "mode-incompatible stock key code is rejected");
  Bytes decreasing;
  appendRecord(decreasing, 1, 2000);
  appendRecord(decreasing, -1, 1000);
  expectDecodeRejected(codec,
                       encodeJson(minimalStock(encodedInner(decreasing))),
                       "decreasing key timestamps are rejected");

  auto equalTimeLegacy = extensionReplay();
  equalTimeLegacy.legacy = replay::LegacyPlaybackTrack{
      .events =
          {
              {.action = replay::LegacyPlaybackAction::Press,
               .lane = 1,
               .songTimeMicros = 1'000,
               .gauge = 20.0F},
              {.action = replay::LegacyPlaybackAction::Gauge,
               .songTimeMicros = 1'000,
               .gauge = 21.0F},
          },
  };
  std::string legacyDiagnostic;
  const auto equalTimeLegacyBytes =
      codec.encodeChart(equalTimeLegacy, 1'000, legacyDiagnostic);
  expect(equalTimeLegacyBytes.has_value(),
         "equal-time legacy playback events remain valid");
  if (equalTimeLegacyBytes) {
    Json decreasingLegacy = outerJson(*equalTimeLegacyBytes);
    decreasingLegacy["asobmashow"]["legacy"]["events"][1]["songTimeMicros"] =
        999;
    expectDecodeRejected(codec, encodeJson(decreasingLegacy),
                         "decreasing legacy playback timestamps are rejected");
  }

  Bytes redundant;
  appendRecord(redundant, 1, 1000);
  appendRecord(redundant, 1, 1001);
  appendRecord(redundant, -1, 1002);
  appendRecord(redundant, -1, 1003);
  const auto tolerant =
      codec.decode(encodeJson(minimalStock(encodedInner(redundant))), 7);
  expect(tolerant.chart.has_value(),
         "redundant stock press/release records are tolerated");
  if (tolerant.chart) {
    expectEqual(tolerant.chart->input.size(), std::size_t(2),
                "redundant stock transitions collapse to effective changes");
  }

  replay::ReplayCodecLimits tinyOuter;
  tinyOuter.maxCompressedBytes = 8;
  replay::BeatorajaReplayCodec tinyOuterCodec(tinyOuter);
  expectDecodeRejected(tinyOuterCodec, readFixture("beatoraja-chart.brd"),
                       "compressed replay byte limit is enforced");

  replay::ReplayCodecLimits tinyJson;
  tinyJson.maxJsonBytes = 32;
  replay::BeatorajaReplayCodec tinyJsonCodec(tinyJson);
  expectDecodeRejected(tinyJsonCodec, readFixture("beatoraja-chart.brd"),
                       "outer JSON expansion limit is enforced");

  replay::ReplayCodecLimits tinyKeys;
  tinyKeys.maxKeyInputBytes = 8;
  replay::BeatorajaReplayCodec tinyKeysCodec(tinyKeys);
  expectDecodeRejected(tinyKeysCodec, readFixture("beatoraja-chart.brd"),
                       "inner keyinput expansion limit is enforced");

  replay::ReplayCodecLimits oneTransition;
  oneTransition.maxInputTransitions = 1;
  replay::BeatorajaReplayCodec oneTransitionCodec(oneTransition);
  expectDecodeRejected(oneTransitionCodec, readFixture("beatoraja-chart.brd"),
                       "input transition count limit is enforced");

  Json deep = minimalStock(encodedInner(redundant));
  Json nested = true;
  for (int i = 0; i < 70; ++i) {
    nested = Json{{"next", std::move(nested)}};
  }
  deep["unknown"] = std::move(nested);
  expectDecodeRejected(codec, encodeJson(deep),
                       "excessive JSON depth is rejected");

  Json excessiveCourse = Json::array();
  const Json stockStage = outerJson(readFixture("beatoraja-chart.brd"));
  for (int index = 0; index < 257; ++index) {
    excessiveCourse.push_back(stockStage);
  }
  const auto excessiveCourseDecoded =
      codec.decode(encodeJson(excessiveCourse), 7);
  expect(!excessiveCourseDecoded.course.has_value() &&
             !excessiveCourseDecoded.diagnostic.empty(),
         "course decode rejects more than 256 stages");

  auto source = extensionReplay();
  std::string diagnostic;
  auto encoded = codec.encodeChart(source, 1000, diagnostic);
  expect(encoded.has_value(), "valid extension fixture encodes for mutations");
  if (!encoded) {
    return;
  }

  Json unknownStock = outerJson(*encoded);
  unknownStock["futureStockField"] = Json{{"nested", 42}};
  const auto acceptedUnknown = codec.decode(encodeJson(unknownStock));
  expect(acceptedUnknown.chart.has_value(),
         "unknown stock JSON fields are accepted");

  Json unknownExtension = outerJson(*encoded);
  unknownExtension["asobmashow"]["schemaVersion"] = 999;
  const auto unsupported = codec.decode(encodeJson(unknownExtension), 7);
  expect(unsupported.chart.has_value(),
         "unknown Aso extension retains safe stock playback");
  expect(unsupported.unsupportedAsoExtension,
         "unknown Aso extension is explicitly flagged");
  expect(unsupported.stockOnly,
         "unknown Aso extension is treated as stock-only playback");
  expect(unsupported.stageSources ==
                 std::vector{replay::ReplayStageDecodeSource::Stock} &&
             !unsupported.replayPathHasUndefinedLongNotes().has_value(),
         "unknown Aso setup falls back to stock while its extension-owned "
         "facts remain unknown");

  replay::CourseReplayPlaybackData futureCourse;
  futureCourse.stages = {source, source};
  futureCourse.stages[1].setup.chartSha256 = std::string(64, 'b');
  futureCourse.stages[1].setup.keyMode = 14;
  futureCourse.restMicrosAfterStage = {1000, 0};
  const auto futureCourseEncoded =
      codec.encodeCourse(futureCourse, 1000, diagnostic);
  expect(futureCourseEncoded.has_value(),
         "mixed-key-mode course fixture encodes");
  if (futureCourseEncoded) {
    Json futureDocument = outerJson(*futureCourseEncoded);
    for (auto &stage : futureDocument) {
      stage["asobmashow"]["schemaVersion"] = 999;
    }
    const std::array expectedModes{7, 14};
    const auto futureDecoded = codec.decode(
        encodeJson(futureDocument), std::span<const int>(expectedModes));
    expect(futureDecoded.course.has_value() &&
               futureDecoded.course->stages[0].setup.keyMode == 7 &&
               futureDecoded.course->stages[1].setup.keyMode == 14 &&
               futureDecoded.unsupportedAsoExtension &&
               futureDecoded.stageSources ==
                   std::vector{
                       replay::ReplayStageDecodeSource::Stock,
                       replay::ReplayStageDecodeSource::Stock},
           "future course extension falls back with per-stage chart key modes");
  }

  Json badCover = outerJson(*encoded);
  badCover["asobmashow"]["laneCoverEvents"][0]["noteStartPositionPercent"] =
      101;
  expectDecodeRejected(codec, encodeJson(badCover),
                       "out-of-range timed lane cover is rejected",
                       std::nullopt);

  Json nonFiniteTouch = outerJson(*encoded);
  nonFiniteTouch["asobmashow"]["touchSamples"][0]["x"] = "NaN";
  expectDecodeRejected(codec, encodeJson(nonFiniteTouch),
                       "non-finite or non-numeric touch coordinate is rejected",
                       std::nullopt);

  Json envelopeMismatch = outerJson(*encoded);
  envelopeMismatch["asobmashow"]["envelope"] = "course-stage";
  expectDecodeRejected(codec, encodeJson(envelopeMismatch),
                       "chart/course extension envelope mismatch is rejected",
                       std::nullopt);

  Json stockShaMismatch = outerJson(*encoded);
  stockShaMismatch["sha256"] = std::string(64, 'b');
  expectDecodeRejected(codec, encodeJson(stockShaMismatch),
                       "stock and extension chart SHA mismatch is rejected",
                       std::nullopt);

  Json stockLongNoteMismatch = outerJson(*encoded);
  stockLongNoteMismatch["mode"] = 2;
  expectDecodeRejected(codec, encodeJson(stockLongNoteMismatch),
                       "stock and extension LN mode mismatch is rejected",
                       std::nullopt);

  for (auto [field, replacement, message] :
       std::array<std::tuple<std::string_view, Json, std::string_view>, 4>{
           std::tuple{"randomoption", Json(2),
                      "stock and extension random option mismatch is rejected"},
           std::tuple{"randomoptionseed", Json(123),
                      "stock and extension random seed mismatch is rejected"},
           std::tuple{"gauge", Json(0),
                      "stock and extension gauge mismatch is rejected"},
           std::tuple{"rand", Json::array({9, 8, 7}),
                      "stock and extension random sequence mismatch is rejected"},
       }) {
    Json mismatch = outerJson(*encoded);
    mismatch[std::string(field)] = std::move(replacement);
    expectDecodeRejected(codec, encodeJson(mismatch), message, std::nullopt);
  }

  Json stockCoverMismatch = outerJson(*encoded);
  stockCoverMismatch["config"]["lanecover"] = 0.25;
  expectDecodeRejected(codec, encodeJson(stockCoverMismatch),
                       "stock and extension lane-cover mismatch is rejected",
                       std::nullopt);

  Json stockCoverEnabledMismatch = outerJson(*encoded);
  stockCoverEnabledMismatch["config"]["enablelanecover"] = false;
  expectDecodeRejected(
      codec, encodeJson(stockCoverEnabledMismatch),
      "stock and extension lane-cover enable mismatch is rejected",
      std::nullopt);

  Bytes otherInput;
  appendRecord(otherInput, 2, 1000);
  appendRecord(otherInput, -2, 1500);
  Json stockInputMismatch = outerJson(*encoded);
  stockInputMismatch["keyinput"] = encodedInner(otherInput);
  expectDecodeRejected(codec, encodeJson(stockInputMismatch),
                       "stock and extension input mismatch is rejected",
                       std::nullopt);

  replay::CourseReplayPlaybackData badCourse;
  badCourse.stages = {source, source};
  badCourse.restMicrosAfterStage = {1};
  expect(!codec.encodeCourse(badCourse, 1000, diagnostic),
         "course stage/rest envelope mismatch is rejected on encode");

  replay::CourseReplayPlaybackData excessiveCourseReplay;
  excessiveCourseReplay.stages.assign(257, source);
  excessiveCourseReplay.restMicrosAfterStage.assign(257, 0);
  expect(!codec.encodeCourse(excessiveCourseReplay, 1000, diagnostic),
         "course encode rejects more than 256 stages");

  replay::ReplayCodecLimits noTouch;
  noTouch.maxTouchSamples = 1;
  replay::BeatorajaReplayCodec noTouchCodec(noTouch);
  expectDecodeRejected(noTouchCodec, *encoded,
                       "touch sample count bound is enforced", std::nullopt);

  replay::ReplayCodecLimits noCover;
  noCover.maxLaneCoverEvents = 1;
  replay::BeatorajaReplayCodec noCoverCodec(noCover);
  expectDecodeRejected(noCoverCodec, *encoded,
                       "timed cover event count bound is enforced",
                       std::nullopt);
}

} // namespace

int main() {
  testIndependentBeatorajaFixtures();
  testDoublePlayOptions();
  testKeyModeTables();
  testPrimitiveCodecs();
  testAsoExtensionRoundTripsWithoutBreakingStock();
  testReplayOnlyScratchHandoffExtensionIsStrictAndStockCompatible();
  testPreRollInputEncodesForChartsAndCourses();
  testStockProjectionCarriesPreRollHoldsAcrossTimeZero();
  testEncodeValidatesRandomAndGaugeEnums();
  testCourseEncodeRejectsNegativeRestDuration();
  testAllMissReplayRoundTripsWithEmptyInput();
  testManualAssignmentProjectsToStockNormal();
  testStockDecodeRejectsZeroKeyRecords();
  testCourseCodecEnforcesAggregateTrackLimits();
  testMalformedAndBoundedInputs();
  if (failures != 0) {
    std::cerr << failures << " Beatoraja replay codec test(s) failed\n";
    return 1;
  }
  std::cout << "Beatoraja replay codec tests passed\n";
  return 0;
}
