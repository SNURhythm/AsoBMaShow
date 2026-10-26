#include "PlayOptionUtils.h"
#include "BmsonAudioFixture.h"
#include "BmsonMultichannelFixture.h"
#include "BmsonPhaseFixture.h"
#include "BmsonReleaseFixture.h"
#include "RAII.h"
#include "Uuid.h"
#include "targets.h"
#include "audio/ChartAudioRenderer.h"
#include "audio/AudioMix.h"
#include "audio/decoder.h"
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
      // Sample-rate conversion rounds PCM16 first; playback interpolation
      // retains float fractions until the final WAV quantization, like AudioMix.
      const short half = percent == 50 ? 0 : 1;
      expect(samples == (channels == 1
                             ? std::vector<short>{-1, -1, short(-half), short(-half), 0, 0, 0, 0, 0, 0}
                             : std::vector<short>{-1, 1, short(-half), half, 0, 0, 0, 0, 0, 0}),
             "offline conversion and playback retain the mixer's separate quantization stages");
    }
  }
}

void testFractionalPlaybackWaveform(const std::filesystem::path &root) {
  auto chart = chartFixture(root, "fractional-playback-waveform");
  replaceKeysound(*chart, 48000, 1, {-101, 201, -301, 401, -501});
  const auto output = chart->Meta.Folder / "result.wav";
  for (const int percent : {105, 195}) {
    // Decoded PCM is {-100, 200, -300, 400, -500}. Normalizing the
    // source to 44.1 kHz gives {-100, 156, -176, 161, -500}; the
    // mixer's Q32 playback cursor then advances by 1.05 or 1.95.
    const std::vector<short> expected = percent == 105
        ? std::vector<short>{-100, 139, -142, 62, -500}
        : std::vector<short>{-100, -159, -434};
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
  expect(sound && info.frames == 88598,
         "successful growth retains the entire decoded tail and interpolation frame");
  if (sound) {
    short samples[2]{};
    expect(sf_seek(sound.get(), 88596, SEEK_SET) == 88596 &&
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

void testBmsonAudio(const std::filesystem::path &root) {
  auto chart = bmsonAudioFixture(root / "bmson");
  const auto output = root / "bmson.wav";
  expect(chart_audio::RenderChartAudioToWav(*chart, output).success, "bmson renders");
  SF_INFO info{};
  auto sound = asobmashow::audio::openSoundFileHandle(output, SFM_READ, info);
  for (const auto &[frame, expected] : std::vector<std::pair<int, int>>{{4410,1500},{13230,1000},{26460,-2000},{35280,-1000},{57330,-2000}}) {
    short sample[2]{};
    expect(sound && sf_seek(sound.get(), frame, SEEK_SET) == frame && sf_readf_short(sound.get(), sample, 1) == 1 &&
           std::abs(sample[0] - expected) <= 2 && sample[0] == sample[1], "bmson slice offset, boundary, restart and layers");
  }
}

void testBmsonReleaseAudioRendering(const std::filesystem::path &root) {
  auto chart = bmsonReleaseFixture();
  chart->Meta.Folder = root / "bmson-release";
  std::filesystem::create_directories(chart->Meta.Folder);
  replaceKeysound(*chart, 44100, 1, std::vector<short>(4410, 1000), "head.wav");
  replaceKeysound(*chart, 44100, 1, std::vector<short>(4410, -2000), "tail.wav");
  ReplayData replay{.provenance = {}};
  replay.chartMeta = chart->Meta;
  replay.events = {
      {.action = ReplayEventAction::Press, .lane = 0,
       .noteTimeMicros = 1'000'000, .songTimeMicros = 1'100'000, .judgement = PGreat},
      {.action = ReplayEventAction::Release, .lane = 0,
       .noteTimeMicros = 2'000'000, .songTimeMicros = 1'300'000, .judgement = None},
      {.action = ReplayEventAction::Release, .lane = 0,
       .noteTimeMicros = 2'000'000, .songTimeMicros = 1'700'000, .judgement = Poor}};
  for (bool replayTiming : {false, true}) {
    const auto output = chart->Meta.Folder / (replayTiming ? "replay.wav" : "auto.wav");
    const auto rendered = chart_audio::RenderChartAudioToWav(*chart, output, {
        .keySoundMode = replayTiming ? chart_audio::KeySoundMode::ReplayTiming
                                    : chart_audio::KeySoundMode::ChartTiming,
        .replay = &replay, .keySoundOffsetMicros = 100'000});
    expect(rendered.success && rendered.eventCount == (replayTiming ? 3 : 2),
           "chart/autokey and replay each emit head and tail, with replay failed-release mute");
    SF_INFO info{};
    auto sound = asobmashow::audio::openSoundFileHandle(output, SFM_READ, info);
    for (const auto &[frame, expected] : std::vector<std::pair<int, int>>{
             {44541, 1000}, {53361, 0}, {71001, replayTiming ? -2000 : 0},
             {88641, replayTiming ? 0 : -2000}}) {
      short sample[2]{};
      expect(sound && sf_seek(sound.get(), frame, SEEK_SET) == frame &&
                 sf_readf_short(sound.get(), sample, 1) == 1 &&
                 std::abs(sample[0] - expected) <= 2 && sample[0] == sample[1],
             "decoded release audio uses actual resolution time, skips None and never doubles chart-timed tail");
    }
  }
}

void testHeldClassicBoundaryRendering(const std::filesystem::path &root) {
  auto chart = bmsonReleaseWithSentinelFixture();
  chart->Meta.Folder = root / "classic-boundary";
  std::filesystem::create_directories(chart->Meta.Folder);
  std::vector<short> pcm(3 * 44100, 3000);
  std::fill(pcm.begin(), pcm.begin() + 44100, 1000);
  replaceKeysound(*chart, 44100, 1, pcm, "head.wav");
  replaceKeysound(*chart, 44100, 1, std::vector<short>(4410, -500), "tail.wav");
  ReplayData replay{.provenance = {}};
  replay.chartMeta = chart->Meta;
  replay.events = {
      {.action = ReplayEventAction::Press, .lane = 7,
       .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000, .judgement = PGreat},
      {.action = ReplayEventAction::Press, .lane = 7, .songTimeMicros = 2'000'000},
      {.action = ReplayEventAction::Release, .lane = 7,
       .noteTimeMicros = 2'000'000, .songTimeMicros = 2'000'001,
       .judgement = PGreat, .releaseByHolding = true},
      {.action = ReplayEventAction::Release, .lane = 7, .songTimeMicros = 2'400'000},
      {.action = ReplayEventAction::Press, .lane = 7, .songTimeMicros = 2'500'000}};
  for (long long origin : {0LL, 2'050'000LL}) {
    const auto path = chart->Meta.Folder / (std::to_string(origin) + ".wav");
    expect(chart_audio::RenderChartAudioToWav(*chart, path, {
        .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &replay,
        .timelineStartMicros = origin}).success, "held classic boundary replay exports");
    SF_INFO info{};
    auto sound = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
    for (const auto &[time, expected] : std::array<std::pair<long long, int>, 3>{{
             {2'050'000, 2500}, {2'200'000, 3000}, {2'550'000, 1000}}}) {
      const auto frame = (time - origin) * 44100 / 1000000;
      short sample[2]{};
      expect(sound && sf_seek(sound.get(), frame, SEEK_SET) == frame &&
                 sf_readf_short(sound.get(), sample, 1) == 1 && std::abs(sample[0] - expected) <= 2,
             "WAV preserves held head phase, layers authored release, and permits post-resolution retrigger");
    }
  }
}

void testAdjacentHcnRendering(const std::filesystem::path &root) {
  for (bool nextHcn : {false, true})
    for (const auto ruleset : {GameplayRuleset::LR2, GameplayRuleset::Beatoraja})
      for (bool rawBoundary : {false, true}) {
      auto chart = bmsonAdjacentHcnFixture(nextHcn);
      chart->Meta.Folder = root / (nextHcn ? "adjacent-hcn" : "adjacent-tap");
      std::filesystem::create_directories(chart->Meta.Folder);
      replaceKeysound(*chart, 44100, 1, std::vector<short>(3 * 44100, 1000), "head.wav");
      replaceKeysound(*chart, 44100, 1, std::vector<short>(3 * 44100, 200), "next.wav");
      if (nextHcn) replaceKeysound(*chart, 44100, 1, std::vector<short>(4410, 50), "tail.wav");
      ReplayData replay{.provenance = {}};
      replay.chartMeta = chart->Meta;
      replay.playbackRuleset = RulesetDescriptor::For(ruleset);
      replay.events = {
          {.action = ReplayEventAction::Press, .lane = 0, .noteTimeMicros = 1'000'000,
           .songTimeMicros = 1'000'000, .judgement = PGreat, .audioLanePressed = true},
          {.action = ReplayEventAction::Release, .lane = 0, .noteTimeMicros = 2'000'000,
           .songTimeMicros = 1'200'000, .judgement = Poor, .audioLanePressed = false},
          {.action = ReplayEventAction::Press, .lane = 0, .noteTimeMicros = 2'100'000,
           .songTimeMicros = 1'990'000, .judgement = Bad, .audioLanePressed = true},
          {.action = ReplayEventAction::Release, .lane = 0,
           .noteTimeMicros = nextHcn ? 2'600'000 : -1,
           .songTimeMicros = 1'995'000, .judgement = nextHcn ? Poor : None,
           .audioLanePressed = false}};
      if (rawBoundary) {
        replay.events[0].songTimeMicros = 900'000;
        replay.events[1].songTimeMicros = 920'000;
        replay.events.insert(replay.events.begin() + 2, {
            ReplayEvent{.action = ReplayEventAction::Press, .lane = 0, .noteTimeMicros = 1'000'000,
                        .songTimeMicros = 950'000, .judgement = Kpoor, .audioLanePressed = true},
            ReplayEvent{.action = ReplayEventAction::Release, .lane = 0,
                        .songTimeMicros = 980'000, .audioLanePressed = false}});
        replay.audioUpdates = {{.beforeEventIndex = 4, .songTimeMicros = 1'010'000,
                                .lane = 0, .headTimeMicros = 1'000'000, .gain = 0}};
      }
      const long long offset = rawBoundary ? 100'000 : 0;
      for (long long gameplayOrigin : {0LL, 1'050'000LL, 1'992'000LL}) {
        const auto origin = gameplayOrigin == 0 ? 0 : gameplayOrigin - offset;
        const auto path = chart->Meta.Folder / (std::to_string(origin) + ".wav");
        expect(chart_audio::RenderChartAudioToWav(*chart, path, {
            .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &replay,
            .keySoundOffsetMicros = offset, .timelineStartMicros = origin}).success, "adjacent HCN replay exports");
        SF_INFO info{};
        auto sound = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
        for (const auto [time, expected] : std::array<std::pair<long long, int>, 4>{{
                 {1'100'000, rawBoundary ? 0 : 1000}, {1'500'000, 0},
                 {1'992'000, 1200},
                 {1'997'000, nextHcn ? 50 : 200}}}) {
          if (time - offset < origin) continue;
          const auto frame = (time - offset - origin) * 44100 / 1000000;
          short sample[2]{};
          expect(sound && sf_seek(sound.get(), frame, SEEK_SET) == frame &&
                     sf_readf_short(sound.get(), sample, 1) == 1 &&
                     std::abs(sample[0] - expected) <= 2 && sample[0] == sample[1],
                 "WAV origins retain live ruleset HCN gain, upcoming sound, and independent tail release");
        }
      }
    }
}

void testBackspinCompanionRendering(const std::filesystem::path &root) {
  auto chart = bmsonReleaseWithSentinelFixture(2);
  chart->Meta.Folder = root / "backspin-companion";
  std::filesystem::create_directories(chart->Meta.Folder);
  std::vector<short> pcm(3 * 44100, 3000);
  std::fill_n(pcm.begin(), 22050, 1000);
  replaceKeysound(*chart, 44100, 1, pcm, "head.wav");
  replaceKeysound(*chart, 44100, 1, std::vector<short>(4410, -500), "tail.wav");
  for (const auto ruleset : {GameplayRuleset::LR2, GameplayRuleset::Beatoraja}) {
    ReplayData replay{.provenance = {}};
    replay.chartMeta = chart->Meta;
    // Runtime playback identity must win over stale stored provenance.
    replay.provenance.ruleset = RulesetDescriptor::For(
        ruleset == GameplayRuleset::LR2 ? GameplayRuleset::Beatoraja : GameplayRuleset::LR2);
    replay.playbackRuleset = RulesetDescriptor::For(ruleset);
    replay.events = {
        {.action = ReplayEventAction::Press, .lane = 7,
         .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000, .judgement = PGreat},
        {.action = ReplayEventAction::Release, .lane = 7,
         .noteTimeMicros = 2'000'000, .songTimeMicros = 2'000'000,
         .judgement = PGreat, .releaseByBackspin = true},
        {.action = ReplayEventAction::Press, .lane = 7, .songTimeMicros = 2'000'000},
        {.action = ReplayEventAction::Release, .lane = 7, .songTimeMicros = 2'400'000},
        {.action = ReplayEventAction::Press, .lane = 7, .songTimeMicros = 2'500'000}};
    for (const auto origin : {0LL, 2'025'000LL}) {
      const auto path = chart->Meta.Folder / (std::to_string(static_cast<int>(ruleset)) + "-" + std::to_string(origin) + ".wav");
      expect(chart_audio::RenderChartAudioToWav(*chart, path, {
          .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &replay,
          .timelineStartMicros = origin}).success, "backspin companion replay exports");
      SF_INFO info{};
      auto sound = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
      for (const auto &[time, expected] : std::array<std::pair<long long, int>, 2>{{
               {2'050'000, ruleset == GameplayRuleset::LR2 ? 2500 : 500}, {2'550'000, 1000}}}) {
        const auto frame = (time - origin) * 44100 / 1000000;
        short sample[2]{};
        expect(sound && sf_seek(sound.get(), frame, SEEK_SET) == frame &&
                   sf_readf_short(sound.get(), sample, 1) == 1 && std::abs(sample[0] - expected) <= 2,
               "WAV honors playback ruleset for companion suppression and preserves tail/later presses across seek");
      }
    }
  }
}

void testScratchHcnAggregateHeldRendering(const std::filesystem::path &root) {
  auto chart = bmsonReleaseFixture(3, 8, false, 960);
  chart->Meta.Folder = root / "scratch-hcn-held";
  std::filesystem::create_directories(chart->Meta.Folder);
  replaceKeysound(*chart, 44100, 1, std::vector<short>(3 * 44100, 1000), "head.wav");
  ReplayData replay{.provenance = {}};
  replay.chartMeta = chart->Meta;
  replay.events = {
      {.action = ReplayEventAction::Press, .lane = 7,
       .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000, .judgement = PGreat,
       .audioLanePressed = true},
      {.action = ReplayEventAction::Release, .lane = 7,
       .noteTimeMicros = 3'000'000, .songTimeMicros = 1'300'000, .judgement = Poor,
       .releaseByBackspin = true, .audioLanePressed = true},
      {.action = ReplayEventAction::Release, .lane = 7,
       .songTimeMicros = 1'400'000, .audioLanePressed = true},
      {.action = ReplayEventAction::Release, .lane = 7,
       .songTimeMicros = 1'500'000, .audioLanePressed = false}};
  for (long long origin : {0LL, 1'400'000LL}) {
    const auto path = chart->Meta.Folder / (std::to_string(origin) + ".wav");
    expect(chart_audio::RenderChartAudioToWav(*chart, path, {
        .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &replay,
        .timelineStartMicros = origin}).success, "aggregate scratch replay exports");
    SF_INFO info{};
    auto sound = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
    for (const auto &[time, expected] : std::array<std::pair<long long, int>, 2>{{
             {1'450'000, 1000}, {1'550'000, 0}}}) {
      const auto frame = (time - origin) * 44100 / 1000000;
      short sample[2]{};
      expect(sound && sf_seek(sound.get(), frame, SEEK_SET) == frame &&
                 sf_readf_short(sound.get(), sample, 1) == 1 && std::abs(sample[0] - expected) <= 2,
             "WAV retains scratch HCN audio until both keys lift, including a seek after the first lift");
    }
  }
}

void testMissedHcnPreviewRendering(const std::filesystem::path &root) {
  auto chart = bmsonReleaseFixture(3, 1, false, 960);
  chart->Meta.Folder = root / "missed-hcn-preview";
  std::filesystem::create_directories(chart->Meta.Folder);
  std::vector<short> pcm(3 * 44100, 1000);
  std::fill(pcm.begin() + 44100, pcm.end(), 3000);
  replaceKeysound(*chart, 44100, 1, pcm, "head.wav");
  ReplayData replay{.provenance = {}};
  replay.chartMeta = chart->Meta;
  replay.events = {
      {.action = ReplayEventAction::Press, .lane = 0,
       .noteTimeMicros = 1'000'000, .songTimeMicros = 500'000, .judgement = Kpoor},
      {.action = ReplayEventAction::Release, .lane = 0,
       .songTimeMicros = 600'000, .judgement = None},
      {.action = ReplayEventAction::Miss, .lane = 0,
       .noteTimeMicros = 1'000'000, .songTimeMicros = 1'300'000, .judgement = Poor},
      {.action = ReplayEventAction::Miss, .lane = 0,
       .noteTimeMicros = 3'000'000, .songTimeMicros = 1'300'000, .judgement = Poor},
      {.action = ReplayEventAction::Press, .lane = 0,
       .songTimeMicros = 1'600'000, .judgement = None},
      {.action = ReplayEventAction::Release, .lane = 0,
       .songTimeMicros = 1'700'000, .judgement = None}};
  // A nonzero timeline origin covers seeking into both muted and restored
  // portions while retaining the original source cursor in exported audio.
  for (long long origin : {0LL, 1'400'000LL, 1'650'000LL}) {
    const auto path = chart->Meta.Folder / (std::to_string(origin) + ".wav");
    expect(chart_audio::RenderChartAudioToWav(*chart, path, {
        .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &replay,
        .timelineStartMicros = origin}).success, "missed HCN preview exports from requested origin");
    SF_INFO info{};
    auto sound = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
    for (const auto &[time, expected] : std::array<std::pair<long long, int>, 4>{{
             {1'200'000, 1000}, {1'400'000, 0}, {1'650'000, 3000}, {1'750'000, 0}}}) {
      if (time < origin) continue;
      const auto frame = (time - origin) * 44100 / 1000000;
      short sample[2]{};
      expect(sound && sf_seek(sound.get(), frame, SEEK_SET) == frame &&
                 sf_readf_short(sound.get(), sample, 1) == 1 && std::abs(sample[0] - expected) <= 2,
             "missed preview WAV mutes and resumes the original source even after seeking");
    }
  }
}

void testLongHeadVoiceMuteAndResume(const std::filesystem::path &root) {
  for (int type : {1, 2, 3}) {
    auto chart = bmsonReleaseFixture(type, 1, false, 960);
    chart->Meta.Folder = root / ("head-control-" + std::to_string(type));
    std::filesystem::create_directories(chart->Meta.Folder);
    std::vector<short> pcm(3 * 44100, 1000);
    std::fill(pcm.begin() + 44100, pcm.end(), 3000);
    replaceKeysound(*chart, 44100, 1, pcm, "head.wav");
    ReplayData replay{.provenance = {}};
    replay.chartMeta = chart->Meta;
    replay.events = {
        {.action = ReplayEventAction::Press, .lane = 0,
         .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000, .judgement = PGreat},
        {.action = ReplayEventAction::Release, .lane = 0,
         .songTimeMicros = 1'500'000, .judgement = None},
        {.action = ReplayEventAction::Release, .lane = 0,
         .noteTimeMicros = 3'000'000, .songTimeMicros = 1'600'000,
         .judgement = Bad, .diffMicros = -1'500'000},
        {.action = ReplayEventAction::Press, .lane = 0,
         .songTimeMicros = 2'200'000, .judgement = None},
        {.action = ReplayEventAction::Release, .lane = 0,
         .songTimeMicros = 2'600'000, .judgement = None}};
    const auto output = chart->Meta.Folder / "replay.wav";
    expect(chart_audio::RenderChartAudioToWav(*chart, output, {
        .keySoundMode = chart_audio::KeySoundMode::ReplayTiming,
        .replay = &replay}).success, "LN voice control replay renders");
    SF_INFO info{};
    auto sound = asobmashow::audio::openSoundFileHandle(output, SFM_READ, info);
    for (const auto &[micros, expected] : std::vector<std::pair<int, int>>{
             {1'550'000, 1000}, {1'650'000, 0},
             {2'250'000, type == 3 ? 3000 : 1000},
             {2'650'000, type == 3 ? 0 : 1000}}) {
      short sample[2]{};
      const auto frame = static_cast<sf_count_t>(micros * 44100LL / 1000000);
      expect(sound && sf_seek(sound.get(), frame, SEEK_SET) == frame &&
                 sf_readf_short(sound.get(), sample, 1) == 1 &&
                 std::abs(sample[0] - expected) <= 2,
             "failed release mutes at resolution; HCN rehold restores the ongoing playhead without retrigger");
    }
    if (type == 1) {
      replay.events = {
          {.action = ReplayEventAction::Press, .lane = 0,
           .noteTimeMicros = 1'000'000, .songTimeMicros = 850'000, .judgement = Bad},
          {.action = ReplayEventAction::Release, .lane = 0,
           .noteTimeMicros = 3'000'000, .songTimeMicros = 3'000'001,
           .judgement = Bad, .diffMicros = -150'000, .releaseByHolding = true}};
      const auto held = chart->Meta.Folder / "held-bad.wav";
      expect(chart_audio::RenderChartAudioToWav(*chart, held, {
          .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &replay}).success,
          "held classic BAD replay renders");
      SF_INFO heldInfo{};
      auto completed = asobmashow::audio::openSoundFileHandle(held, SFM_READ, heldInfo);
      short sample[2]{};
      expect(completed && sf_seek(completed.get(), 3'100'000LL * 44100 / 1000000, SEEK_SET) >= 0 &&
                 sf_readf_short(completed.get(), sample, 1) == 1 && std::abs(sample[0] - 3000) <= 2,
             "WAV retains the head beyond an automatically completed BAD classic tail");
    }
    if (type == 2) {
      replay.events.resize(3);
      replay.events.push_back({.action = ReplayEventAction::Press, .lane = 0,
          .noteTimeMicros = 1'000'000, .songTimeMicros = 1'600'000, .judgement = Kpoor});
      const auto retrigger = chart->Meta.Folder / "retrigger.wav";
      expect(chart_audio::RenderChartAudioToWav(*chart, retrigger, {
          .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &replay}).success,
          "same-time retrigger renders");
      SF_INFO retriggerInfo{};
      auto restarted = asobmashow::audio::openSoundFileHandle(retrigger, SFM_READ, retriggerInfo);
      short sample[2]{};
      const auto frame = 1'650'000LL * 44100 / 1000000;
      expect(restarted && sf_seek(restarted.get(), frame, SEEK_SET) == frame &&
                 sf_readf_short(restarted.get(), sample, 1) == 1 && std::abs(sample[0] - 1000) <= 2,
             "old head mute must not capture a new trigger of the same note at the same timestamp");
    }
  }
}

// A gain transition must change only amplitude, never the continuous source
// position. Constant samples cannot detect a restarted fractional cursor.
void testGainSegmentFractionalPhase(const std::filesystem::path &root) {
  constexpr std::array<long long, 6> controls{
      1'200'000, 1'500'090, 1'700'110, 1'800'130, 2'600'000, 2'700'000};
  struct Case { int sourceRate, speed; long long origin; };
  for (const auto [sourceRate, speed, origin] : std::array<Case, 6>{{
           {44100, 125, 0}, {48000, 100, 0}, {44100, 150, -250'000},
           {48000, 195, 1'150'000}, {44100, 50, 1'650'000},
           {44100, 200, 1'830'000}}}) {
    auto chart = bmsonReleaseFixture(3, 1, false, 960);
    const int sourcePrefix = speed == 195 ? sourceRate : 0;
    if (sourcePrefix != 0) {
      const std::string json = R"({"version":"1.0.0","info":{"init_bpm":120,"ln_type":3},
        "sound_channels":[{"name":"head.wav","notes":[{"x":0,"y":0},
        {"x":1,"y":480,"l":960,"c":true}]}]})";
      bms_parser::Parser parser;
      bms_parser::Chart *raw = nullptr;
      std::atomic_bool cancelled{false};
      parser.Parse(std::vector<unsigned char>(json.begin(), json.end()), &raw, false, false,
                   cancelled, "continued-gain.bmson");
      if (!raw) throw std::runtime_error("Continued gain fixture did not parse");
      chart.reset(raw);
    }
    chart->Meta.Folder = root / ("gain-phase-" + std::to_string(sourceRate) +
                                "-" + std::to_string(speed));
    std::filesystem::create_directories(chart->Meta.Folder);
    // The sound ends before the final controls; these must not extend its tail.
    std::vector<short> pcm(sourcePrefix + sourceRate * 135 / 100);
    for (std::size_t frame = 0; frame < pcm.size(); ++frame)
      pcm[frame] = static_cast<short>(12000 * std::sin(frame * 0.7));
    replaceKeysound(*chart, sourceRate, 1, pcm, "head.wav");
    ReplayData replay{.provenance = {}};
    replay.chartMeta = chart->Meta;
    replay.events = {{.action = ReplayEventAction::Press, .lane = 0,
                     .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000,
                     .judgement = PGreat}};
    const auto readRender = [&](const char *name) {
      const auto path = chart->Meta.Folder / name;
      expect(chart_audio::RenderChartAudioToWav(*chart, path, {
          .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &replay,
          .playback = {.percent = speed}, .timelineStartMicros = origin}).success,
          "fractional HCN gain fixture exports decoded WAV");
      SF_INFO info{};
      auto file = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
      if (!file) throw std::runtime_error("Could not read gain phase WAV");
      std::vector<short> samples(info.frames * info.channels);
      expect(sf_readf_short(file.get(), samples.data(), info.frames) == info.frames,
             "complete gain phase output is readable");
      return samples;
    };
    const auto continuous = readRender("continuous.wav");
    for (std::size_t i = 0; i < controls.size(); ++i)
      replay.events.push_back({.action = i % 2 ? ReplayEventAction::Press : ReplayEventAction::Release,
          .lane = 0, .noteTimeMicros = i == 0 ? 3'000'000 : -1,
          .songTimeMicros = controls[i], .judgement = i == 0 ? Poor : None});
    const auto controlled = readRender("controlled.wav");
    expect(controlled.size() == continuous.size(), "gain intervals preserve the bounded source tail");
    std::size_t nextControl = 0;
    bool audible = true;
    int maxError = 0;
    for (std::size_t frame = 0; frame < std::min(continuous.size(), controlled.size()) / 2; ++frame) {
      while (nextControl < controls.size() &&
             BmsonPhaseFixture::frameAt(controls[nextControl], speed) -
                 BmsonPhaseFixture::frameAt(origin, speed) <= static_cast<long long>(frame)) {
        audible = nextControl % 2 != 0;
        ++nextControl;
      }
      const int expected = audible ? continuous[frame * 2] : 0;
      maxError = std::max(maxError, std::abs(expected - controlled[frame * 2]));
    }
    if (maxError > 2) std::cerr << "gain phase source=" << sourceRate << " speed=" << speed
                               << " origin=" << origin << " max PCM error=" << maxError << '\n';
    expect(maxError <= 2, "gain intervals preserve continuous decoded source phase through origin clipping");

    if (sourceRate == 44100 && speed == 125) {
      chart_audio::RenderOptions budgetOptions{
          .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &replay,
          .playback = {.percent = speed}, .maxMixedFrames = 47628};
      const auto budgetPath = chart->Meta.Folder / "budget.wav";
      expect(guardedRender(*chart, budgetPath, budgetOptions).success,
             "gain intervals share the voice's exact output-frame work budget");
      --budgetOptions.maxMixedFrames;
      expect(!guardedRender(*chart, budgetPath, budgetOptions).success,
             "gain intervals still reject a one-frame-short work budget");
      // Independent production mixer oracle: one voice, six gain commands,
      // a shared callback rounding origin and deliberately small mix chunks.
      SoundData sound;
      sound.channels = 1;
      sound.monophonic = true;
      SF_INFO info{};
      std::atomic_bool cancelled{false};
      std::vector<short> decoded;
      expect(decodeAudioToPCM(fspath_to_path_t(chart->Meta.Folder / "head.wav"),
                             decoded, info, cancelled), "phase mixer source decodes");
      sound.outputFrameCount = decoded.size();
      sound.outputData = std::move(decoded);
      AudioCallbackState state;
      audio::playback::InsertScheduledSound(state, {.soundData = &sound, .bus = audio::Bus::Keysound,
          .startMicros = 1'000'000, .voiceId = 1});
      for (std::size_t i = 0; i < controls.size(); ++i)
        audio::playback::InsertScheduledSound(state, {.soundData = &sound, .bus = audio::Bus::Keysound,
            .startMicros = controls[i], .sequence = i + 1, .gain = i % 2 ? 1.0F : 0.0F,
            .voiceId = 1, .setVoiceGain = true});
      std::vector<float> mixed(controlled.size() / 2);
      for (std::uint32_t offset = 0; offset < mixed.size(); offset += 257) {
        const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(257, mixed.size() - offset));
        audio::playback::ActivateScheduledSounds(state, 0, 44100, count, speed, offset);
        audio::playback::MixActiveSounds(state, std::span(mixed).subspan(offset, count),
                                         count, 1, 1, 1, speed);
      }
      int mixerError = 0;
      for (std::size_t frame = 0; frame < mixed.size(); ++frame)
        mixerError = std::max(mixerError, std::abs(
            static_cast<int>(std::lrint(mixed[frame] / 0.9F * 32767)) - controlled[frame * 2]));
      expect(mixerError <= 2, "decoded WAV gain boundaries and fractional playhead match production AudioMix");
    }
  }
}

void testGainSegmentLayerOwnership(const std::filesystem::path &root) {
  const std::string json = R"({"version":"1.0.0","info":{"init_bpm":120,"ln_type":3},
    "sound_channels":[{"name":"shared.wav","notes":[{"x":1,"y":480,"l":960},{"x":2,"y":480}]},
    {"name":"layer.wav","notes":[{"x":1,"y":480,"l":960}]}]})";
  bms_parser::Parser parser;
  bms_parser::Chart *raw = nullptr;
  std::atomic_bool cancelled{false};
  parser.Parse(std::vector<unsigned char>(json.begin(), json.end()), &raw, false, false,
               cancelled, "layer-phase.bmson");
  std::unique_ptr<bms_parser::Chart> chart(raw);
  if (!chart) throw std::runtime_error("Layer phase bmson did not parse");
  chart->Meta.Folder = root / "layer-phase";
  std::filesystem::create_directories(chart->Meta.Folder);
  const auto makeSound = [&](const char *name, int frames, int amplitude) {
    std::vector<short> pcm(frames);
    for (std::size_t frame = 0; frame < pcm.size(); ++frame)
      pcm[frame] = static_cast<short>(amplitude * std::sin(frame * 0.7));
    replaceKeysound(*chart, 44100, 1, pcm, name);
    auto sound = std::make_shared<SoundData>();
    sound->channels = 1;
    sound->monophonic = true;
    SF_INFO info{};
    std::vector<short> decoded;
    if (!decodeAudioToPCM(fspath_to_path_t(chart->Meta.Folder / name), decoded, info, cancelled))
      throw std::runtime_error("Layer phase source did not decode");
    sound->outputFrameCount = decoded.size();
    sound->outputData = std::move(decoded);
    return sound;
  };
  const auto shared = makeSound("shared.wav", 46305, 6000);
  const auto layer = makeSound("layer.wav", 37485, 3000);
  SoundData group;
  group.layers = {shared, layer};
  ReplayData replay{.provenance = {}};
  replay.chartMeta = chart->Meta;
  replay.events = {
      {.action = ReplayEventAction::Press, .lane = 0, .noteTimeMicros = 1'000'000,
       .songTimeMicros = 1'000'000, .judgement = PGreat},
      {.action = ReplayEventAction::Release, .lane = 0, .noteTimeMicros = 3'000'000,
       .songTimeMicros = 1'200'000, .judgement = Poor},
      {.action = ReplayEventAction::Press, .lane = 0, .songTimeMicros = 1'500'090},
      {.action = ReplayEventAction::Press, .lane = 1, .songTimeMicros = 1'600'120},
      {.action = ReplayEventAction::Release, .lane = 0, .songTimeMicros = 1'700'110},
      {.action = ReplayEventAction::Press, .lane = 0, .songTimeMicros = 1'800'130}};
  const auto path = chart->Meta.Folder / "render.wav";
  expect(chart_audio::RenderChartAudioToWav(*chart, path, {
      .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &replay,
      .playback = {.percent = 125}}).success, "layered gain phase replay exports");
  SF_INFO info{};
  auto file = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
  if (!file) throw std::runtime_error("Layer phase WAV missing");
  std::vector<short> samples(info.frames * 2);
  sf_readf_short(file.get(), samples.data(), info.frames);
  AudioCallbackState state;
  audio::playback::InsertScheduledSound(state, {.soundData = &group, .bus = audio::Bus::Keysound,
      .startMicros = 1'000'000, .voiceId = 1});
  audio::playback::InsertScheduledSound(state, {.soundData = shared.get(), .bus = audio::Bus::Keysound,
      .startMicros = 1'600'120, .sequence = 3, .voiceId = 2});
  constexpr std::array<long long, 4> controls{1'200'000, 1'500'090, 1'700'110, 1'800'130};
  for (std::size_t i = 0; i < controls.size(); ++i)
    audio::playback::InsertScheduledSound(state, {.soundData = &group, .bus = audio::Bus::Keysound,
        .startMicros = controls[i], .sequence = i < 2 ? i + 1 : i + 2,
        .gain = i % 2 ? 1.0F : 0.0F, .voiceId = 1, .setVoiceGain = true});
  std::vector<float> mixed(info.frames);
  audio::playback::ActivateScheduledSounds(state, 0, 44100, mixed.size(), 125);
  audio::playback::MixActiveSounds(state, mixed, mixed.size(), 1, 1, 1, 125);
  int maxError = 0;
  for (std::size_t frame = 0; frame < mixed.size(); ++frame)
    maxError = std::max(maxError, std::abs(
        static_cast<int>(std::lrint(mixed[frame] / 0.9F * 32767)) - samples[frame * 2]));
  expect(maxError <= 3, "WAV layers keep fractional phase, finite tails, and gain ownership after shared-slice retrigger");
}

void testFiniteSliceEndControl(const std::filesystem::path &root) {
  for (const int speed : {125, 195}) {
    BmsonPhaseFixture fixture(root / ("finite-end-control-" + std::to_string(speed)),
                              44100, 1'000'020, 0, true, false, false, 0, true);
    const auto definition = gameplay::buildGameplayDefinition(*fixture.chart, 0);
    const auto duration = fixture.chart->WavSlices.at(fixture.schedule.front().wav).front().DurationMicros;
    expect(duration > 0, "finite end control uses an authored bounded slice");
    const auto time = 1'000'020 + duration;
    const auto frame = BmsonPhaseFixture::frameAt(time, speed);
    fixture.replay.events.resize(1);
    fixture.schedule = buildReplayKeysoundSchedule(definition, fixture.replay.events, 0, std::nullopt);
    const auto baseline = fixture.continuous(speed, frame + 2);
    expect(std::abs(baseline[frame]) > 0.01F,
           "nominal finite slice end still has an audible production mixer frame");
    fixture.replay.events.push_back({.action = ReplayEventAction::Release, .lane = 0,
        .noteTimeMicros = 3'000'000, .songTimeMicros = time, .judgement = Poor});
    fixture.schedule = buildReplayKeysoundSchedule(definition, fixture.replay.events, 0, std::nullopt);
    const auto controlled = fixture.continuous(speed, frame + 2);
    expect(controlled[frame] == 0 && controlled[frame - 1] == baseline[frame - 1],
           "production gain command mutes exactly the last active output frame");
    const auto path = fixture.chart->Meta.Folder / "controlled.wav";
    expect(chart_audio::RenderChartAudioToWav(*fixture.chart, path, {
        .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &fixture.replay,
        .playback = {.percent = speed}}).success, "finite end control exports");
    SF_INFO info{};
    auto file = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
    std::array<short, 4> pcm{};
    expect(file && sf_seek(file.get(), frame - 1, SEEK_SET) == frame - 1 &&
           sf_readf_short(file.get(), pcm.data(), 2) == 2,
           "finite end control output is readable");
    expect(pcm[2] == 0 && pcm[3] == 0 && std::abs(pcm[0]) > 100,
           "finite slice controls use output clipping instead of raw-duration rejection");
    std::cout << "finite slice end gain: rate=" << speed << " duration=" << duration
              << " frame=" << frame << " unmutedPCM=" << std::lrint(baseline[frame] / 0.9F * 32767)
              << " controlledPCM=" << pcm[2] << '\n';
  }
}

void testFrameGridPhase(const std::filesystem::path &root) {
  struct Case { int sourceRate, speed; long long trigger, retrigger, origin; bool layers; bool continued = false; bool bounded = false; };
  const std::array cases{
      Case{44100, 125, 1'000'020, 0, 0, false},
      Case{48000, 125, 1'000'000, 0, 0, false},
      Case{44100, 125, 1'000'000, 1'600'043, 0, false},
      Case{44100, 125, 1'000'000, 1'600'058, 0, false},
      Case{44100, 125, 1'000'000, 0, -20, false},
      Case{44100, 125, 1'000'000, 0, 999'980, false},
      Case{44100, 125, 1'000'020, 0, 1'500'020, false},
      Case{48000, 125, 1'000'020, 1'600'043, 1'500'020, true},
      Case{48000, 195, 1'000'020, 1'600'058, -20, true, true},
      Case{32000, 150, 1'000'020, 1'600'058, 1'500'020, true, true, true},
      Case{44100, 50, 1'002'500, 0, 999'980, false},
      Case{44100, 100, 1'005'000, 0, -20, false},
      Case{44100, 200, 1'010'000, 0, 1'500'020, false}};
  int index = 0;
  for (const auto &c : cases) {
    BmsonPhaseFixture fixture(root / ("frame-grid-" + std::to_string(index++)),
                              c.sourceRate, c.trigger, c.retrigger, c.layers, c.layers, c.continued, 0, c.bounded);
    const auto path = fixture.chart->Meta.Folder / "output.wav";
    expect(chart_audio::RenderChartAudioToWav(*fixture.chart, path, {
        .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &fixture.replay,
        .playback = {.percent = c.speed}, .timelineStartMicros = c.origin}).success,
        "frame grid fixture exports");
    SF_INFO info{};
    auto file = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
    if (!file) throw std::runtime_error("frame grid WAV missing");
    std::vector<short> wav(info.frames * 2);
    sf_readf_short(file.get(), wav.data(), info.frames);
    const auto origin = BmsonPhaseFixture::frameAt(c.origin, c.speed);
    const auto continuous = fixture.continuous(c.speed, std::max(0LL, origin) + info.frames);
    int maximum = 0;
    for (long long frame = 0; frame < info.frames; ++frame) {
      const auto source = frame + origin;
      const int expected = source < 0 ? 0 : std::lrint(continuous[source] / 0.9F * 32767);
      maximum = std::max(maximum, std::abs(expected - wav[frame * 2]));
    }
    if (maximum > 2) std::cerr << "frame grid source=" << c.sourceRate << " rate=" << c.speed
        << " trigger=" << c.trigger << " retrigger=" << c.retrigger << " origin=" << c.origin
        << " maxPCM=" << maximum << '\n';
    expect(maximum <= 2, "WAV starts, cutoffs, gain intervals and origins preserve continuous mixer phase");
  }
}

void testBmsonMultichannelRendering(const std::filesystem::path &root) {
  struct Case { int channels, sourceRate, speed, isolatedChannel; long long origin; bool layers; };
  const std::array cases{
      Case{1, 44100, 100, -1, 0, false},
      Case{2, 48000, 125, -1, -20, false},
      Case{3, 44100, 100, 2, 0, false},
      Case{4, 44100, 100, 3, 0, false},
      Case{3, 48000, 125, 2, 230'100, true},
      Case{4, 48000, 195, 3, 160'030, true},
      Case{5, 32000, 155, -1, 230'100, true},
      Case{6, 48000, 65, -1, -20, true},
      Case{6, 44100, 125, -1, 280'044, true}};
  int index = 0;
  int maximumError = 0;
  std::size_t comparedSamples = 0;
  for (const auto &c : cases) {
    BmsonMultichannelFixture fixture(root / ("multichannel-" + std::to_string(index++)),
                                      c.channels, c.sourceRate, c.isolatedChannel, c.layers);
    const auto path = fixture.chart->Meta.Folder / "output.wav";
    chart_audio::RenderOptions options{
        .keySoundMode = chart_audio::KeySoundMode::ReplayTiming, .replay = &fixture.replay,
        .playback = {.percent = c.speed}, .timelineStartMicros = c.origin};
    expect(guardedRender(*fixture.chart, path, options).success, "multichannel bmson exports");
    SF_INFO info{};
    auto file = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
    if (!file) throw std::runtime_error("multichannel export missing");
    std::vector<short> wav(info.frames * 2);
    if (sf_readf_short(file.get(), wav.data(), info.frames) != info.frames)
      throw std::runtime_error("multichannel export truncated");
    const auto origin = BmsonPhaseFixture::frameAt(c.origin, c.speed);
    const auto mixed = fixture.continuous(fixture.replaySchedule(), c.speed,
                                          std::max(0LL, origin) + info.frames);
    int maximum = 0;
    bool audible = false;
    for (long long frame = 0; frame < info.frames; ++frame)
      for (int channel = 0; channel < 2; ++channel) {
        const auto source = frame + origin;
        const int expected = source < 0 ? 0 :
            BmsonMultichannelFixture::pcm16(mixed[source * 2 + channel]);
        audible = audible || expected != 0;
        maximum = std::max(maximum, std::abs(expected - wav[frame * 2 + channel]));
      }
    if (maximum > 1) std::cerr << "multichannel export channels=" << c.channels
        << " source=" << c.sourceRate << " rate=" << c.speed << " origin=" << c.origin
        << " maxPCM=" << maximum << '\n';
    expect(audible && maximum <= 1,
           "export folds every channel after normalization and playback interpolation like AudioMix");
    maximumError = std::max(maximumError, maximum);
    comparedSamples += wav.size();
    if (c.channels == 3 && c.speed == 100) {
      // The single continuation slice spans [100000, 550000) at 44100 Hz.
      // Channel-pair processing must neither copy PCM nor charge this voice's
      // output-frame work budget once per pair.
      fixture.replay.events.resize(1);
      options.maxMixedFrames = 19845;
      expect(guardedRender(*fixture.chart, path, options).success,
             "multichannel continuation uses the exact voice-frame budget");
      --options.maxMixedFrames;
      expect(!guardedRender(*fixture.chart, path, options).success,
             "multichannel rendering rejects a one-frame-short work budget");
    }
  }
  std::cout << "multichannel export: cases=" << cases.size() << " samples=" << comparedSamples
            << " maxPCM=" << maximumError << '\n';
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
    if (argc == 2 && std::string_view(argv[1]) == "--multichannel") {
      testBmsonMultichannelRendering(root);
      return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--unicode-keysound-test") {
      testUnicodeKeysoundReference(root);
      return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--benchmark") {
      benchmarkRendering(root);
      return failures == 0 ? 0 : 1;
    }
    testBmsonMultichannelRendering(root);
    testFiniteSliceEndControl(root);
    testFrameGridPhase(root);
    if (argc == 2 && std::string_view(argv[1]) == "--phase") return failures == 0 ? 0 : 1;
    testGainSegmentFractionalPhase(root);
    testGainSegmentLayerOwnership(root);
    testAdjacentHcnRendering(root);
    testBackspinCompanionRendering(root);
    testHeldClassicBoundaryRendering(root);
    testScratchHcnAggregateHeldRendering(root);
    testMissedHcnPreviewRendering(root);
    testLongHeadVoiceMuteAndResume(root);
    testBmsonReleaseAudioRendering(root);
    testBmsonAudio(root);
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
