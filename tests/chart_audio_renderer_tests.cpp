#include "PlayOptionUtils.h"
#include "RAII.h"
#include "Uuid.h"
#include "targets.h"
#include "audio/ChartAudioRenderer.h"
#include "audio/MusicPreparation.h"
#include "audio/ProgressiveMusicSource.h"
#include "Utils.h"
#include "audio/ClubBeat.h"
#include "audio/SoundFileIO.h"

#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace audio_allocation_guard {
thread_local bool enabled = false;
thread_local std::size_t rejected = 0;
constexpr std::size_t maximumAllocation = 8U * 1024U * 1024U;
}

void *operator new(std::size_t bytes) {
  if (audio_allocation_guard::enabled &&
      bytes > audio_allocation_guard::maximumAllocation) {
    ++audio_allocation_guard::rejected;
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(bytes == 0 ? 1 : bytes)) {
    return memory;
  }
  throw std::bad_alloc();
}

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

class AllocationGuard {
public:
  AllocationGuard() {
    audio_allocation_guard::rejected = 0;
    audio_allocation_guard::enabled = true;
  }
  ~AllocationGuard() { audio_allocation_guard::enabled = false; }
};

std::string readText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void writeText(const std::filesystem::path &path, std::string_view text) {
  std::ofstream output(path, std::ios::binary);
  output << text;
  if (!output) throw std::runtime_error("Could not write fixture");
}

std::unique_ptr<bms_parser::Chart>
chartFixture(const std::filesystem::path &root, std::string_view name,
             std::string_view bpm = "120", std::size_t sampleFrames = 441) {
  const auto directory = root / std::string(name);
  std::filesystem::create_directories(directory);
  const auto soundPath = directory / "click.wav";
  SF_INFO info{};
  info.samplerate = 44100;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  auto sound = asobmashow::audio::openSoundFileHandle(soundPath, SFM_WRITE, info);
  if (!sound) throw std::runtime_error("Could not create WAV fixture");
  const std::vector<short> samples(sampleFrames, 12000);
  if (sf_writef_short(sound.get(), samples.data(), samples.size()) !=
          static_cast<sf_count_t>(sampleFrames) ||
      sf_close(sound.release()) != 0) {
    throw std::runtime_error("Could not write WAV fixture");
  }
  const auto chartPath = directory / "chart.bms";
  writeText(chartPath, "#PLAYER 1\n#TITLE " + std::string(name) + "\n#BPM " +
                           std::string(bpm) + "\n#WAV01 click.wav\n#00011:01\n");
  std::atomic_bool cancelled{false};
  auto chart = play_options::parseChart(chartPath, cancelled, "audio fixture");
  if (!chart) throw std::runtime_error("Could not parse audio fixture");
  return chart;
}

chart_audio::RenderResult guardedRender(
    const bms_parser::Chart &chart, const std::filesystem::path &output,
    const chart_audio::RenderOptions &options = {}) {
  AllocationGuard guard;
  try {
    return chart_audio::RenderChartAudioToWav(chart, output, options);
  } catch (const std::exception &error) {
    expect(false, std::string("Renderer threw instead of rejecting input: ") +
                      error.what());
    return {.message = "Unexpected render exception"};
  }
}

void testNormalDecodedRender(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "normal");
  const auto output = chart->Meta.Folder / "result.wav";
  const auto result = guardedRender(*chart, output);
  expect(result.success, "normal real parsed chart renders successfully");
  expect(audio_allocation_guard::rejected == 0,
         "normal render uses bounded fixture allocations");
  SF_INFO info{};
  auto sound = asobmashow::audio::openSoundFileHandle(output, SFM_READ, info);
  expect(sound && info.frames == 88200 && info.samplerate == 44100 &&
             info.channels == 2,
         "normal WAV preserves two-second chart and stereo output");
  if (sound) {
    short samples[2]{};
    expect(sf_readf_short(sound.get(), samples, 1) == 1 && samples[0] > 11000 &&
               samples[0] == samples[1],
           "actual decoded keysound reaches both output channels");
  }
}

void replaceKeysound(const bms_parser::Chart &chart, int rate, int channels,
                     const std::vector<short> &samples,
                     const char *filename = "click.wav") {
  SF_INFO info{};
  info.samplerate = rate;
  info.channels = channels;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  auto sound = asobmashow::audio::openSoundFileHandle(
      chart.Meta.Folder / filename, SFM_WRITE, info);
  if (!sound || sf_write_short(sound.get(), samples.data(), samples.size()) !=
                    static_cast<sf_count_t>(samples.size()))
    throw std::runtime_error("Could not replace keysound fixture");
}

void testResampledWaveform(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "resampled-waveform");
  const auto output = chart->Meta.Folder / "result.wav";
  for (const int channels : {1, 2}) {
    for (const int percent : {50, 100, 200}) {
      // All three combinations have the same exact 1:2 source/output ratio.
      // The decoder scales PCM16 by 32767/32768, so +/-2 decodes to +/-1.
      replaceKeysound(*chart, 22050 * 100 / percent, channels,
                      channels == 1 ? std::vector<short>{-2, 0}
                                    : std::vector<short>{-2, 2, 0, 0});
      chart_audio::RenderOptions options;
      options.playback.percent = percent;
      expect(guardedRender(*chart, output, options).success,
             "fractional-rate waveform renders");
      SF_INFO info{};
      auto sound = asobmashow::audio::openSoundFileHandle(output, SFM_READ, info);
      std::vector<short> samples(10);
      expect(sound && sf_readf_short(sound.get(), samples.data(), 5) == 5,
             "fractional-rate waveform can be read");
      expect(samples == (channels == 1
                             ? std::vector<short>{-1, -1, -1, -1, 0, 0, 0, 0, 0, 0}
                             : std::vector<short>{-1, 1, -1, 1, 0, 0, 0, 0, 0, 0}),
             "offline resampling rounds signed half samples away from zero");
    }
  }
}

void testFractionalPlaybackWaveform(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "fractional-playback-waveform");
  replaceKeysound(*chart, 48000, 1, {-101, 201, -301, 401, -501});
  const auto output = chart->Meta.Folder / "result.wav";
  for (const int percent : {105, 195}) {
    // Decoded PCM is {-100, 200, -300, 400, -500}. The exact source
    // positions advance by 8/7 at 105%, or by 104/49 at 195%.
    const std::vector<short> expected = percent == 105
        ? std::vector<short>{-100, 129, -100, 14, -500}
        : std::vector<short>{-100, -214, -500};
    chart_audio::RenderOptions options;
    options.playback.percent = percent;
    options.maxMixedFrames = expected.size();
    expect(guardedRender(*chart, output, options).success,
           "fractional playback fits its exact mixed-frame budget");
    SF_INFO info{};
    auto sound = asobmashow::audio::openSoundFileHandle(output, SFM_READ, info);
    std::vector<short> samples((expected.size() + 1) * 2);
    expect(sound && sf_readf_short(sound.get(), samples.data(), expected.size() + 1) ==
                        static_cast<sf_count_t>(expected.size() + 1),
           "fractional playback waveform can be read");
    for (std::size_t i = 0; i < expected.size(); ++i)
      expect(samples[i * 2] == expected[i] && samples[i * 2 + 1] == expected[i],
             "fractional playback preserves signed interpolation and the final frame");
    expect(samples[expected.size() * 2] == 0 && samples.back() == 0,
           "fractional playback stops at the exact projected frame count");
    --options.maxMixedFrames;
    expect(!guardedRender(*chart, output, options).success,
           "fractional playback rejects a one-frame-short work budget");
  }
}

void testDirectMixHeadroom(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "direct-mix-headroom");
  replaceKeysound(*chart, 44100, 2, {20000, -20000});
  replaceKeysound(*chart, 44100, 2, {-20000, 20000}, "negative.wav");
  chart->WavTable[2] = "negative.wav";
  chart->ReferencedWavTable[2] = "negative.wav";
  auto *timeline = chart->Measures.front()->TimeLines.front();
  timeline->BackgroundNotes.push_back(new bms_parser::Note(1));
  timeline->BackgroundNotes.push_back(new bms_parser::Note(2));
  const auto output = chart->Meta.Folder / "result.wav";
  expect(guardedRender(*chart, output).success, "direct stereo overlap renders");
  SF_INFO info{};
  auto sound = asobmashow::audio::openSoundFileHandle(output, SFM_READ, info);
  short samples[4]{};
  expect(sound && sf_readf_short(sound.get(), samples, 2) == 2 &&
             samples[0] == 19998 && samples[1] == -19998 &&
             samples[2] == 0 && samples[3] == 0,
         "direct mixing preserves stereo, float headroom, and the silent tail");
}

void benchmarkRendering(const std::filesystem::path &root) {
  for (const int rate : {44100, 48000}) {
    auto chart = chartFixture(root, "benchmark-" + std::to_string(rate));
    std::vector<short> samples(rate * 2);
    for (std::size_t i = 0; i < samples.size(); ++i)
      samples[i] = static_cast<short>(static_cast<int>((i * 17) % 65536) - 32768);
    replaceKeysound(*chart, rate, 2, samples);
    auto *timeline = chart->Measures.front()->TimeLines.front();
    for (int i = 1; i < 512; ++i)
      timeline->BackgroundNotes.push_back(new bms_parser::Note(1));
    for (int run = 0; run < 3; ++run) {
      const auto start = std::chrono::steady_clock::now();
      const auto result = chart_audio::RenderChartAudioToWav(
          *chart, chart->Meta.Folder / "result.wav");
      expect(result.success && result.eventCount == 512, "benchmark renders all voices");
      std::cout << "Render 512 overlapping 1 s stereo sounds at " << rate << " Hz: "
                << std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - start).count()
                << " ms\n";
    }
  }
}

void testUnicodeKeysoundReference(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "unicode-keysound");
  const std::u8string name = u8"\u97f3\u697d-\ud55c\uae00-\U0001f3b5.wav";
  std::filesystem::rename(chart->Meta.Folder / "click.wav",
                          chart->Meta.Folder / std::filesystem::path(name));
  const std::string reference(name.begin(), name.end());
  chart->WavTable[1] = reference;
  chart->ReferencedWavTable[1] = reference;
  const auto output = chart->Meta.Folder / "result.wav";
  const auto result = guardedRender(*chart, output);
  expect(result.success, "UTF-8 keysound reference renders successfully");
  SF_INFO info{};
  auto sound = asobmashow::audio::openSoundFileHandle(output, SFM_READ, info);
  expect(sound && info.frames == 88200 && info.channels == 2,
         "Unicode keysound render preserves the expected WAV format");
  if (sound) {
    short samples[2]{};
    expect(sf_readf_short(sound.get(), samples, 1) == 1 && samples[0] > 11000 &&
               samples[0] == samples[1],
           "Unicode keysound contributes decoded PCM instead of silence");
  }
}

void testRateAndTailSemantics(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "rate-and-tail");
  const auto output = chart->Meta.Folder / "result.wav";
  for (const int percent : {50, 200}) {
    chart_audio::RenderOptions options;
    options.playback.percent = percent;
    expect(guardedRender(*chart, output, options).success,
           "supported non-neutral rate renders normally");
    SF_INFO info{};
    auto sound = asobmashow::audio::openSoundFileHandle(output, SFM_READ, info);
    expect(sound && info.frames == 88200 * 100 / percent,
           "output duration preserves PitchShift rate scaling");
  }
  chart->Measures.front()->TimeLines.front()->Timing = 1999000;
  expect(guardedRender(*chart, output).success,
         "ordinary sound-tail growth is allowed");
  SF_INFO info{};
  auto sound = asobmashow::audio::openSoundFileHandle(output, SFM_READ, info);
  expect(sound && info.frames == 88597,
         "successful growth retains the entire decoded tail and interpolation frame");
  if (sound) {
    short samples[2]{};
    expect(sf_seek(sound.get(), 88595, SEEK_SET) == 88595 &&
               sf_readf_short(sound.get(), samples, 1) == 1 && samples[0] > 11000,
           "the actual final keysound frame is not truncated");
  }
  expect(chart_audio::replayEventRawTimeMicros(
             std::numeric_limits<long long>::max(), -1) ==
             std::numeric_limits<long long>::max(),
         "extreme replay offset subtraction saturates without signed overflow");
}

void testDurationAdmission(const std::filesystem::path &root) {
  for (const bool cancelledAtEntry : {false, true}) {
    auto chart = chartFixture(root, cancelledAtEntry ? "cancelled" : "low-bpm",
                              "0.001");
    expect(chart->Meta.TotalLength == 240000000000LL,
           "bounded real parser fixture retains low-positive-BPM duration");
    const auto output = chart->Meta.Folder / "result.wav";
    writeText(output, "preserved output");
    std::atomic_bool cancelled{cancelledAtEntry};
    const auto result = guardedRender(*chart, output, {.isCancelled = &cancelled});
    expect(!result.success, "pathological or cancelled render is not published");
    expect(audio_allocation_guard::rejected == 0,
           "renderer rejects duration before requesting a huge dense allocation");
    expect(cancelledAtEntry ? result.message.find("cancel") != std::string::npos
                            : (result.message.find("limit") != std::string::npos ||
                               result.message.find("budget") != std::string::npos),
           "renderer explains cancellation or resource admission failure");
    expect(readText(output) == "preserved output",
           "failed admission preserves caller-owned output");
  }
}

void testSelectedLargeRender(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "selected-large", "0.5");
  const auto output = chart->Meta.Folder / "result.wav";
  writeText(output, "preserved output");
  std::atomic_bool cancelled{false};
  bool reachedWriteBoundary = false;
  const auto result = chart_audio::RenderChartAudioToWav(
      *chart, output,
      {.isCancelled = &cancelled,
       .log = [&](const std::string &message) {
         if (message.starts_with("Chart audio duration:")) {
           reachedWriteBoundary = true;
           cancelled.store(true);
         }
       }});
  expect(reachedWriteBoundary && result.durationMicros == 480000000,
         "selected eight-minute chart mixes beyond the former 128 MiB quota");
  expect(!result.success && result.message.find("cancel") != std::string::npos &&
             readText(output) == "preserved output",
         "large selected render retains cancellation and atomic publication");

  const auto allocationFailure = guardedRender(*chart, output);
  expect(!allocationFailure.success && audio_allocation_guard::rejected == 1 &&
             allocationFailure.message.find("memory") != std::string::npos &&
             readText(output) == "preserved output",
         "selected allocation failure is reported and preserves existing output");
}

void testSelectedHighMixWork(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "selected-high-work", "120", 88200);
  auto *timeline = chart->Measures.front()->TimeLines.front();
  for (int index = 0; index < 1800; ++index) {
    timeline->BackgroundNotes.push_back(new bms_parser::Note(1));
  }
  const auto output = chart->Meta.Folder / "result.wav";
  // 1801 overlapping two-second sounds exceed the former one-hour work quota.
  const auto result = guardedRender(*chart, output);
  expect(result.success && result.eventCount == 1801,
         "selected work beyond the former one-hour quota renders every sound");
  SF_INFO info{};
  auto sound = asobmashow::audio::openSoundFileHandle(output, SFM_READ, info);
  expect(sound && info.frames == 88201,
         "high cumulative work retains the full two-second decoded tail");
  std::atomic_bool cancelled{false};
  AllocationGuard guard;
  const auto limitedOutput = chart->Meta.Folder / "limited.wav";
  const auto limited = chart_audio::RenderChartAudioToWav(
      *chart, limitedOutput, {.isCancelled = &cancelled,
                             .maxMixedFrames = 44100U * 60U * 60U});
  expect(!limited.success &&
             limited.message.find("work limit") != std::string::npos &&
             !std::filesystem::exists(limitedOutput),
         "explicit renderer work budgets reject excessive cumulative mixing");
}

void testSelectedLargeClubPlan(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "selected-large-club", "100000000");
  chart->Measures.front()->Scale = 25001;
  chart->Measures.front()->TimeLines.front()->Notes.front()->Wav =
      bms_parser::Parser::NoWav;
  std::atomic_bool cancelled{false};
  bool mixedClubSound = false;
  const auto result = guardedRender(
      *chart, chart->Meta.Folder / "result.wav",
      {.clubMode = true,
       .isCancelled = &cancelled,
       .log = [&](const std::string &message) {
         if (message.starts_with("Chart audio mix started:")) {
           mixedClubSound = true;
           cancelled.store(true);
         }
       }});
  expect(mixedClubSound && !result.success &&
             result.message.find("cancel") != std::string::npos,
         "selected club plan beyond 100000 beats reaches real generated-sound mixing");
  expect(club_beat::buildPlan(*chart, &cancelled).empty(),
         "cancelled club planning stops before producing events");
  cancelled.store(false);
  AllocationGuard guard;
  const auto limitedOutput = chart->Meta.Folder / "limited.wav";
  const auto limited = chart_audio::RenderChartAudioToWav(
      *chart, limitedOutput, {.clubMode = true, .isCancelled = &cancelled,
                             .maxClubBeats = 100000});
  expect(!limited.success &&
             limited.message.find("plan limit") != std::string::npos &&
             audio_allocation_guard::rejected == 0 &&
             !std::filesystem::exists(limitedOutput),
         "explicit renderer club-plan budgets reject excess beats before allocation");
}

void testOverflowScaleMetadata(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "overflow-duration");
  chart->Meta.TotalLength = std::numeric_limits<long long>::max();
  const auto output = chart->Meta.Folder / "result.wav";
  const auto result = guardedRender(*chart, output);
  expect(!result.success && !std::filesystem::exists(output),
         "overflow-scale duration fails without publishing audio");
  expect(audio_allocation_guard::rejected == 0,
         "overflow-scale metadata is rejected before allocating or narrowing");
}

void testInvalidClubTimingPreservesOutput(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "invalid-club-timing");
  auto *timeline = chart->Measures.front()->TimeLines.front();
  timeline->StopLength = -48;
  const auto output = chart->Meta.Folder / "result.wav";
  writeText(output, "preserved output");
  const auto result = guardedRender(*chart, output, {.clubMode = true});
  expect(!result.success && result.message.find("timing") != std::string::npos,
         "export reports invalid club timing rather than silently omitting beats");
  expect(readText(output) == "preserved output",
         "invalid club plan preserves the existing output file");
}

void testReferenceChartAdmission(const std::filesystem::path &root) {
  const std::string malformed = "#BPM 120\n#00002:5e-324\n#00151:01\n";
  const auto source = root / "unmatched-long-note.bms";
  writeText(source, malformed);
  const std::vector<unsigned char> bytes(malformed.begin(), malformed.end());
  std::atomic_bool cancelled{false};
  std::string diagnostic;
  const auto fromPath = play_options::parseChart(source, cancelled, "reference fixture", &diagnostic);
  expect(fromPath && fromPath->Meta.TotalNotes == 1 && fromPath->Meta.TotalLongNotes == 0 && diagnostic.empty(),
         "filesystem parser adapter accepts the demoted normal note");
  const auto fromBytes = play_options::parseChartBytes(source, bytes, std::nullopt,
              std::nullopt, std::nullopt, cancelled, "reference fixture", &diagnostic);
  expect(fromBytes && fromBytes->Meta.TotalNotes == 1 && fromBytes->Meta.TotalLongNotes == 0 && diagnostic.empty(),
         "buffered parser adapter accepts the demoted normal note");

  bms_parser::Parser parser;
  bms_parser::Chart *raw = nullptr;
  parser.Parse(bytes, &raw, false, false, cancelled);
  const std::unique_ptr<bms_parser::Chart> chart(raw);
  expect(chart && chart->Meta.TotalNotes == 1,
         "raw parser demotes the unmatched head and retains its metadata");
  if (!chart) return;
  for (const int mode : {1, 2, 3}) {
    try {
      bms_parser::Chart *modeRaw = nullptr;
      parser.Parse(bytes, &modeRaw, false, false, cancelled);
      const std::unique_ptr<bms_parser::Chart> modeChart(modeRaw);
      expect(modeChart != nullptr, "each LN mode starts with the reference graph");
      if (!modeChart) continue;
      applyEffectiveLongNoteModeToChart(*modeChart, mode);
      expect(modeChart->Meta.TotalNotes == 1 && modeChart->Meta.TotalLongNotes == 0,
             "demoted normal note counts are independent of selected LN mode");
    } catch (const std::exception &) {
      expect(false, "LN mode preparation must not add graph admission rules");
    }
  }
  for (const int mode : {1, 2, 3}) {
    const std::string late = "#BPM 120\n#00151:0101\n#00111:0002\n";
    auto selected = play_options::parseChartBytes(source, {late.begin(), late.end()},
        std::nullopt, std::nullopt, std::nullopt, cancelled, "selected LN mode");
    expect(selected && selected->Meta.TotalLongNotes == 1,
           "undefined mode initially retains healthy classic detached-tail head");
    if (!selected) continue;
    applyEffectiveLongNoteModeToChart(*selected, mode);
    expect(selected->Meta.TotalNotes == 2 && selected->Meta.TotalLongNotes == (mode == 1 ? 1 : 0),
           "selected CN/HCN mode finalizes unusable hold as normal in parser library");
    applyEffectiveLongNoteModeToChart(*selected, mode);
    expect(selected->Meta.TotalNotes == 2 && selected->Meta.TotalLongNotes == (mode == 1 ? 1 : 0),
           "repeated mode finalization is idempotent");
  }
  const auto output = root / "unsupported-chart.wav";
  writeText(output, "preserved output");
  const auto result = guardedRender(*chart, output);
  expect(result.success && readText(output).starts_with("RIFF"),
         "audio scheduling does not impose an unrelated LN pairing restriction");
  expect(chart->Meta.TotalNotes == 1, "audio export preserves the parsed graph's counts");

  const std::string extreme = "#BPM 120\n#STOP01 Infinity\n#00009:01\n#00111:01\n";
  writeText(source, extreme);
  const auto saturated = play_options::parseChart(source, cancelled, "saturated fixture");
  expect(saturated &&
             saturated->Meta.TotalLength == std::numeric_limits<long long>::max(),
         "the parser adapter retains saturated timing without chart rejection");

  writeText(source, "#BPM 120\n#00051:0101\n");
  auto paired = play_options::parseChart(source, cancelled, "supported fixture");
  expect(paired && paired->Meta.TotalNotes == 1,
         "an ordinary reciprocal LN pair remains playable");

  auto audioFixture = chartFixture(root, "detached-long-note");
  for (const auto replacement : {"02", "0002"}) {
    const std::string detached = std::string("#BPM 120\n#WAV01 click.wav\n"
        "#WAV02 click.wav\n#00051:0101\n#00011:") + replacement + "\n";
    writeText(audioFixture->Meta.BmsPath, detached);
    auto fromPath = play_options::parseChart(audioFixture->Meta.BmsPath,
        cancelled, "detached fixture", &diagnostic);
    auto fromBytes = play_options::parseChartBytes(audioFixture->Meta.BmsPath,
        {detached.begin(), detached.end()}, std::nullopt, std::nullopt,
        std::nullopt, cancelled, "detached fixture", &diagnostic);
    expect(fromPath && fromBytes && diagnostic.empty() &&
               fromPath->DetachedNotes.empty() &&
               fromBytes->DetachedNotes.empty(),
           "path and buffered preparation demote malformed LN endpoints");
    if (fromPath) {
      const auto notes = fromPath->Meta.TotalNotes;
      const auto rendered = guardedRender(*fromPath,
          audioFixture->Meta.Folder / "detached.wav");
      expect(rendered.success && fromPath->Meta.TotalNotes == notes &&
                 fromPath->DetachedNotes.empty(),
             "audio export preserves parser-demoted normal notes");
    }
  }
}

void testCancellationBeforePublication(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "cancel-before-write");
  const auto output = chart->Meta.Folder / "result.wav";
  writeText(output, "preserved output");
  std::atomic_bool cancelled{false};
  bool reachedWriteBoundary = false;
  const auto result = guardedRender(
      *chart, output,
      {.isCancelled = &cancelled,
       .log = [&](const std::string &message) {
         if (message.starts_with("Chart audio duration:")) {
           reachedWriteBoundary = true;
           cancelled.store(true);
         }
       }});
  expect(reachedWriteBoundary, "cancellation follows actual decoding and mixing");
  expect(!result.success && result.message.find("cancel") != std::string::npos,
         "cancellation before output cannot become successful incomplete audio");
  expect(readText(output) == "preserved output",
         "cancelled publication preserves the preexisting destination");
}

void testTailAndWorkAdmission(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "tail-budget");
  const auto output = chart->Meta.Folder / "result.wav";
  auto *timeline = chart->Measures.front()->TimeLines.front();
  timeline->Timing = 1999000;
  writeText(output, "preserved output");
  chart_audio::RenderOptions options;
  options.maxOutputFrames = 88200;
  const auto tailResult = guardedRender(*chart, output, options);
  expect(!tailResult.success && tailResult.message.find("limit") != std::string::npos,
         "actual decoded sound tail cannot grow beyond the output frame limit");
  expect(readText(output) == "preserved output",
         "rejected sound-tail growth preserves caller-owned output");
  timeline->Timing = 0;
  options.maxMixedFrames = 440;
  const auto workResult = guardedRender(*chart, output, options);
  expect(!workResult.success && workResult.message.find("limit") != std::string::npos,
         "real decoded sample mixing obeys a cumulative work limit");
  expect(audio_allocation_guard::rejected == 0,
         "tiny injected limits are checked without large allocations");
  expect(readText(output) == "preserved output",
         "rejected mixing work does not publish incomplete output");
  options.maxMixedFrames = 441;
  expect(guardedRender(*chart, output, options).success,
         "exact decoded frame work budget is admitted without truncation");
  timeline->Timing = std::numeric_limits<long long>::max();
  expect(!guardedRender(*chart, output, options).success,
         "extreme event time cannot overflow growth arithmetic");
  options.timelineStartMicros = std::numeric_limits<long long>::min();
  expect(!guardedRender(*chart, output, options).success,
         "extreme timeline subtraction cannot bypass duration admission");
}

void testCancellationDuringWrite(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "cancel-during-write");
  const auto output = chart->Meta.Folder / "result.wav";
  writeText(output, "preserved output");
  std::atomic_bool cancelled{false};
  bool observedPartialWrite = false;
  const auto result = guardedRender(
      *chart, output,
      {.isCancelled = &cancelled,
       .log = [&](const std::string &message) {
         if (!message.starts_with("Chart audio output started:")) return;
         for (const auto &entry : std::filesystem::directory_iterator(chart->Meta.Folder)) {
           if (entry.is_directory() && entry.path().filename().string().starts_with(".chart-audio-")) {
             const auto stagedOutput = entry.path() / "output.wav";
             observedPartialWrite = std::filesystem::file_size(stagedOutput) > 44;
           }
         }
         cancelled.store(true);
       }});
  expect(observedPartialWrite, "write cancellation follows actual partial staged WAV bytes");
  expect(!result.success && result.message.find("cancel") != std::string::npos,
         "cancelled partial writing fails instead of publishing success");
  expect(readText(output) == "preserved output",
         "cancelled partial writing leaves existing output intact");
  for (const auto &entry : std::filesystem::directory_iterator(chart->Meta.Folder)) {
    expect(!entry.path().filename().string().starts_with(".chart-audio-"),
           "owned staging directory is removed after write cancellation");
  }
}

void testPublicationFailureCleanup(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "publication-failure");
  const auto output = chart->Meta.Folder / "result.wav";
  std::filesystem::create_directory(output);
  writeText(output / "unrelated", "preserved directory");
  const auto result = guardedRender(*chart, output);
  expect(!result.success && readText(output / "unrelated") == "preserved directory",
         "failed atomic publication cannot replace or delete an unrelated directory");
  for (const auto &entry : std::filesystem::directory_iterator(chart->Meta.Folder)) {
    expect(!entry.path().filename().string().starts_with(".chart-audio-"),
           "failed publication cleans its exclusively owned staging directory");
  }
}

void testCancellationDuringMix(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "cancel-during-mix", "120", 22050);
  const auto output = chart->Meta.Folder / "result.wav";
  writeText(output, "preserved output");
  std::atomic_bool cancelled{false};
  bool observedMix = false;
  bool observedOutput = false;
  const auto result = guardedRender(
      *chart, output,
      {.isCancelled = &cancelled,
       .log = [&](const std::string &message) {
         if (message.starts_with("Chart audio mix started:")) {
           observedMix = true;
           cancelled.store(true);
         }
         if (message.starts_with("Chart audio output started:")) observedOutput = true;
       }});
  expect(observedMix && !observedOutput,
         "cancellation is injected after 4096 actually mixed frames, before output");
  expect(!result.success && result.message.find("cancel") != std::string::npos,
         "mid-sound cancellation is not reported as successful audio");
  expect(readText(output) == "preserved output",
         "mid-sound cancellation preserves existing destination");
}

void testGeneratedSoundBudget(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "club-budget");
  const auto output = chart->Meta.Folder / "result.wav";
  expect(guardedRender(*chart, output, {.clubMode = true}).success,
         "ordinary generated club sounds continue to render");
  chart_audio::RenderOptions options;
  options.clubMode = true;
  options.maxMixedFrames = 441;
  expect(!guardedRender(*chart, output, options).success,
         "generated sounds share the cumulative keysound work limit");
  chart->Measures.front()->Scale = 25001;
  options.maxMixedFrames = 44100U * 60U * 60U;
  options.maxClubBeats = 100000;
  const auto result = guardedRender(*chart, output, options);
  expect(!result.success && result.message.find("plan limit") != std::string::npos &&
             audio_allocation_guard::rejected == 0,
         "explicit background club budget rejects before allocating or iterating the plan");
  chart->Measures.front()->Scale = 1000000000;
  options.maxClubBeats = std::numeric_limits<std::size_t>::max();
  const auto unrepresentable = guardedRender(*chart, output, options);
  expect(!unrepresentable.success &&
             unrepresentable.message.find("plan limit") != std::string::npos &&
             audio_allocation_guard::rejected == 0,
         "unrepresentable club beat numbers fail before allocation or integer overflow");
}

void testLegacyMusicCacheCleanup(const std::filesystem::path &root) {
  const auto cache = root / "old-music-cache";
  const auto exports = root / "exports";
  std::filesystem::create_directories(cache / ".music-stream-old");
  std::filesystem::create_directories(cache / "unrecognized-directory");
  std::filesystem::create_directories(exports);
  writeText(cache / "song.wav", "old full-song cache");
  writeText(cache / "song.WAV", "old full-song cache");
  writeText(cache / "song.wav.tmp", "old incomplete render");
  writeText(cache / ".music-stream-old" / "audio.wav", "old stream spool");
  writeText(cache / "notes.txt", "preserve unknown files");
  writeText(cache / "unrecognized-directory" / "keep.wav", "preserve unknown subtree");
  writeText(exports / "song.wav", "explicit export");
  std::error_code error;
  std::filesystem::create_directory_symlink(exports, cache / ".music-stream-link", error);
  const bool linked = !error;
  music_preparation::RemoveLegacyMusicCacheFiles(cache);
  expect(!std::filesystem::exists(cache / "song.wav") &&
             !std::filesystem::exists(cache / "song.WAV") &&
             !std::filesystem::exists(cache / "song.wav.tmp") &&
             !std::filesystem::exists(cache / ".music-stream-old"),
         "legacy cleanup removes cached WAV, partial output, and old private stream directories");
  expect(readText(exports / "song.wav") == "explicit export" &&
             readText(cache / "notes.txt") == "preserve unknown files" &&
             readText(cache / "unrecognized-directory" / "keep.wav") == "preserve unknown subtree",
         "legacy cleanup preserves exports and unrecognized entries");
  if (linked) {
    expect(std::filesystem::is_symlink(cache / ".music-stream-link"),
           "legacy cleanup does not follow or remove symbolic links");
    music_preparation::RemoveLegacyMusicCacheFiles(cache / ".music-stream-link");
    expect(readText(exports / "song.wav") == "explicit export",
           "legacy cleanup refuses a symlink as its root");
  }
  music_preparation::RemoveLegacyMusicCacheFiles(root / "missing-cache");
}

#if !TARGET_OS_WINDOWS
void testShortMusicPreparationAlwaysStreams(const std::filesystem::path &root) {
  // HOME is redirected by main, so migration cleanup cannot touch a real user cache.
  const auto cache = Utils::GetDocumentsPath("music_cache");
  std::filesystem::create_directories(cache);
  writeText(cache / "old.wav", "retired cache");
  auto chart = chartFixture(root, "short-stream", "1200");
  std::atomic_bool cancelled{false};
  auto prepared = music_preparation::PrepareMusicForPlayback(chart->Meta, cancelled, false);
  expect(prepared.success && prepared.stream != nullptr,
         "a fully prepared short song remains a stream instead of a persistent WAV");
  expect(!std::filesystem::exists(cache),
         "music preparation removes the old cache instead of recreating it");
  if (prepared.stream) {
    expect(prepared.stream->GetState().complete && prepared.durationMicros == 200000,
           "short source completes before the startup buffer threshold");
    std::array<short, 2> samples{};
    const auto read = prepared.stream->ReadFrames(0, samples, std::chrono::milliseconds(100));
    expect(read.frames == 1 && samples[0] > 11000 && samples[0] == samples[1],
           "fully prepared short stream retains its playable PCM after preparation returns");
  }
}
#endif
}

int main(int argc, char **argv) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("chart-audio-tests-" + uuid::generateV4());
  std::filesystem::create_directories(root);
  ScopeExit cleanup([&] { std::filesystem::remove_all(root); });
#if !TARGET_OS_WINDOWS
  const char *home = std::getenv("HOME");
  const std::optional<std::string> previousHome = home ? std::make_optional(home) : std::nullopt;
  if (setenv("HOME", root.c_str(), 1) != 0) return 2;
  ScopeExit restoreHome([&] {
    if (previousHome) setenv("HOME", previousHome->c_str(), 1);
    else unsetenv("HOME");
  });
#endif
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--unicode-keysound-test") {
      testUnicodeKeysoundReference(root);
      return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
      benchmarkRendering(root);
      return failures == 0 ? 0 : 1;
    }
    testResampledWaveform(root);
    testDirectMixHeadroom(root);
    testFractionalPlaybackWaveform(root);
    testNormalDecodedRender(root);
    testUnicodeKeysoundReference(root);
    testRateAndTailSemantics(root);
    testDurationAdmission(root);
    testSelectedLargeRender(root);
    testSelectedHighMixWork(root);
    testSelectedLargeClubPlan(root);
    testOverflowScaleMetadata(root);
    testInvalidClubTimingPreservesOutput(root);
    testReferenceChartAdmission(root);
    testCancellationBeforePublication(root);
    testTailAndWorkAdmission(root);
    testCancellationDuringWrite(root);
    testPublicationFailureCleanup(root);
    testCancellationDuringMix(root);
    testGeneratedSoundBudget(root);
    testLegacyMusicCacheCleanup(root);
#if !TARGET_OS_WINDOWS
    testShortMusicPreparationAlwaysStreams(root);
#endif
  } catch (const std::exception &error) {
    std::cerr << "Fixture failure: " << error.what() << '\n';
    return 2;
  }
  if (failures != 0) return 1;
  std::cout << "chart audio renderer tests passed\n";
}
