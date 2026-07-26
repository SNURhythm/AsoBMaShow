#include "replay/Base64Url.h"
#include "replay/BeatorajaReplayCodec.h"
#include "replay/GzipCodec.h"

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

replay::ReplayPlaybackData extensionReplay() {
  replay::ReplayPlaybackData value;
  value.setup.chartMd5 = "0123456789abcdef0123456789abcdef";
  value.setup.chartSha256 =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  value.setup.keyMode = 7;
  value.setup.longNoteMode = 2;
  value.setup.hasUndefinedLongNotes = true;
  value.setup.randomSeed = 17;
  value.setup.randomPrng = "mt19937";
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
  // chart  97f1175b7bc802728887191d9cc9c29cdf57fb9c3232b056a2e748b5ddfd7535
  // course 06fb9857647feef4fa7df2086f17f904e8173c22fae084e096c9b45d8d5dd187
  // keys   ec101f21efe5c18fb8562ca495f034ccfa75dbab304ac00de647fd3691a8bc1d
  replay::BeatorajaReplayCodec codec;
  const auto chartBytes = readFixture("beatoraja-chart.brd");
  const auto chart = codec.decode(chartBytes, 7);
  expect(chart.chart.has_value(), "stock Beatoraja chart fixture decodes");
  expect(!chart.course.has_value(), "chart fixture is not a course");
  expect(chart.stockOnly, "stock fixture is identified as stock-only");
  expect(!chart.unsupportedAsoExtension,
         "stock fixture has no unsupported extension");
  expect(chart.diagnostic.empty(), "stock chart fixture has no diagnostic");
  if (chart.chart) {
    const auto &value = *chart.chart;
    expectEqual(value.setup.chartSha256, std::string(64, 'a'),
                "stock SHA-256 maps exactly");
    expectEqual(value.setup.keyMode, 7, "expected chart mode drives key map");
    expectEqual(value.setup.longNoteMode, 1, "stock LN mode maps exactly");
    expectEqual(value.setup.randomValues, std::vector<int>({4, 2, 7}),
                "stock RANDOM sequence maps exactly");
    expectEqual(value.setup.playOption, std::optional<std::string>("RANDOM"),
                "stock random option maps exactly");
    expectEqual(value.setup.playOptionSeed,
                std::optional<std::int64_t>(0x123456),
                "stock random seed maps exactly");
    expectEqual(value.setup.initialGaugeType, GaugeType::Hard,
                "stock gauge maps exactly");
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
  if (decoded.chart) {
    expectEqual(
        *decoded.chart, source,
        "Aso playback setup, raw input, touch, and timed cover round-trip");
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
    if (courseDecoded.course) {
      expectEqual(*courseDecoded.course, course,
                  "course stages and inter-stage rests round-trip");
    }
  }
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

void testAllMissReplayRoundTripsWithEmptyInput() {
  replay::BeatorajaReplayCodec codec;
  auto source = extensionReplay();
  source.input.clear();
  std::string diagnostic;

  const auto chart = codec.encodeChart(source, 1'725'000'000'123LL, diagnostic);
  expect(chart.has_value(), "all-miss chart replay encodes with empty input");
  if (chart.has_value()) {
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

  replay::CourseReplayPlaybackData badCourse;
  badCourse.stages = {source, source};
  badCourse.restMicrosAfterStage = {1};
  expect(!codec.encodeCourse(badCourse, 1000, diagnostic),
         "course stage/rest envelope mismatch is rejected on encode");

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
  testKeyModeTables();
  testPrimitiveCodecs();
  testAsoExtensionRoundTripsWithoutBreakingStock();
  testPreRollInputEncodesForChartsAndCourses();
  testAllMissReplayRoundTripsWithEmptyInput();
  testMalformedAndBoundedInputs();
  if (failures != 0) {
    std::cerr << failures << " Beatoraja replay codec test(s) failed\n";
    return 1;
  }
  std::cout << "Beatoraja replay codec tests passed\n";
  return 0;
}
