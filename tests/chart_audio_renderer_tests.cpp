#include "PlayOptionUtils.h"
#include "RAII.h"
#include "Uuid.h"
#include "audio/ChartAudioRenderer.h"
#include "audio/ChartMusicCache.h"
#include "audio/MusicPlaylist.h"
#include "audio/SoundFileIO.h"

#include <atomic>
#include <cmath>
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

class MusicPlayerService {
public:
  std::atomic<std::uint64_t> preloadRequestRevision{1};
  std::atomic_bool preloadCancelled{false};
  void AdjacentPreloadWorker(std::vector<music_playlist::MusicTrack> tracks,
                             std::uint64_t preloadRevision,
                             bool requestedClubMode,
                             const std::stop_token &stopToken);
};

ASOBMS_ADJACENT_PRELOAD_METHOD

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
         "ordinary sound-tail growth is allowed below the hard limit");
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
  chart->Measures.front()->Scale = 1000000000;
  options.maxMixedFrames = chart_audio::kMaxMixedFrames;
  const auto result = guardedRender(*chart, output, options);
  expect(!result.success && result.message.find("plan limit") != std::string::npos &&
             audio_allocation_guard::rejected == 0,
         "huge club scale is rejected before allocating or iterating the plan");
}

#if !TARGET_OS_WINDOWS
void testAdjacentPreloadRejectsAndContinues(const std::filesystem::path &root) {
  auto oversized = chartFixture(root, "preload-oversized", "0.001");
  auto normal = chartFixture(root, "preload-normal");
  const auto oversizedPath = chart_music_cache::CachedAudioPathForChart(oversized->Meta);
  const auto normalPath = chart_music_cache::CachedAudioPathForChart(normal->Meta);
  music_playlist::MusicTrack first;
  first.representativeChart = oversized->Meta;
  music_playlist::MusicTrack second;
  second.representativeChart = normal->Meta;
  MusicPlayerService service;
  {
    AllocationGuard guard;
    service.AdjacentPreloadWorker({first, second}, 1, false, {});
  }
  expect(audio_allocation_guard::rejected == 0,
         "actual adjacent preload rejects before attempting huge allocations");
  expect(!std::filesystem::exists(oversizedPath) &&
             !std::filesystem::exists(oversizedPath.string() + ".tmp"),
         "failed actual cache render leaves no output or owned partial file");
  expect(std::filesystem::is_regular_file(normalPath),
         "adjacent worker continues to the next valid real chart");
  auto cancelled = chartFixture(root, "preload-cancelled", "0.001");
  first.representativeChart = cancelled->Meta;
  const auto cancelledPath = chart_music_cache::CachedAudioPathForChart(cancelled->Meta);
  service.preloadCancelled.store(true);
  service.AdjacentPreloadWorker({first}, 1, false, {});
  expect(!std::filesystem::exists(cancelledPath) &&
             !std::filesystem::exists(cancelledPath.string() + ".tmp"),
         "cancelled adjacent parsing does not publish cache artifacts");
}
#endif
}

int main() {
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
    testNormalDecodedRender(root);
    testRateAndTailSemantics(root);
    testDurationAdmission(root);
    testOverflowScaleMetadata(root);
    testCancellationBeforePublication(root);
    testTailAndWorkAdmission(root);
    testCancellationDuringWrite(root);
    testPublicationFailureCleanup(root);
    testCancellationDuringMix(root);
    testGeneratedSoundBudget(root);
#if !TARGET_OS_WINDOWS
    testAdjacentPreloadRejectsAndContinues(root);
#endif
  } catch (const std::exception &error) {
    std::cerr << "Fixture failure: " << error.what() << '\n';
    return 2;
  }
  if (failures != 0) return 1;
  std::cout << "chart audio renderer tests passed\n";
}
