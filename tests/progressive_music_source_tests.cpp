#include "PlayOptionUtils.h"
#include "RAII.h"
#include "Uuid.h"
#include "audio/ChartAudioRenderer.h"
#include "audio/DecodedAssetCache.h"
#include "audio/ProgressiveMusicSource.h"
#include "audio/SoundFileIO.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
std::atomic_bool guardLargeAllocations{false};
}
void *operator new(std::size_t bytes) {
  if (guardLargeAllocations.load() && bytes > 8U * 1024U * 1024U)
    throw std::bad_alloc();
  if (void *memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
  throw std::bad_alloc();
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace std::chrono_literals;
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void writeWav(const std::filesystem::path &path, int rate, int channels,
              const std::vector<short> &samples, int repeats = 1) {
  SF_INFO info{};
  info.samplerate = rate;
  info.channels = channels;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  auto file = asobmashow::audio::openSoundFileHandle(path, SFM_WRITE, info);
  require(bool(file), "could not create WAV fixture");
  for (int repeat = 0; repeat < repeats; ++repeat)
    require(sf_write_short(file.get(), samples.data(), samples.size()) ==
                static_cast<sf_count_t>(samples.size()), "could not write WAV fixture");
}
std::unique_ptr<bms_parser::Chart> chartAt(const std::filesystem::path &folder,
                                         const std::string &body = "#00011:01\n") {
  std::filesystem::create_directories(folder);
  const auto path = folder / "chart.bms";
  { std::ofstream file(path);
    file << "#PLAYER 1\n#TITLE Stream fixture\n#BPM 120\n"
            "#WAV01 first.wav\n#WAV02 second.wav\n" << body; }
  std::atomic_bool cancelled{false};
  auto chart = play_options::parseChart(path, cancelled, "progressive music test");
  require(bool(chart), "could not parse chart fixture");
  return chart;
}
std::vector<short> readAll(const std::shared_ptr<music_stream::Source> &source,
                         std::uint64_t firstFrame = 0) {
  require(bool(source), "progressive music source must be created");
  std::vector<short> result;
  std::vector<short> block(997 * 2);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  for (;;) {
    require(std::chrono::steady_clock::now() < deadline, "stream read timed out");
    const auto read = source->ReadFrames(firstFrame + result.size() / 2, block, 100ms);
    if (read.status == music_stream::ReadStatus::End) return result;
    if (read.status == music_stream::ReadStatus::Error)
      throw std::runtime_error(read.error);
    require(read.status != music_stream::ReadStatus::Cancelled, "unexpected stream cancellation");
    if (read.status == music_stream::ReadStatus::Data)
      result.insert(result.end(), block.begin(), block.begin() + read.frames * 2);
  }
}
std::vector<short> readWav(const std::filesystem::path &path) {
  SF_INFO info{};
  auto file = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
  require(bool(file), "reference must be a readable WAV");
  require(info.channels == 2 && info.samplerate == 44100, "reference format changed");
  std::vector<short> result(static_cast<std::size_t>(info.frames) * 2);
  require(sf_readf_short(file.get(), result.data(), info.frames) == info.frames,
          "reference must contain all rendered frames");
  return result;
}
void waitForOpening(const std::shared_ptr<music_stream::Source> &source) {
  require(bool(source), "progressive music source must be created");
  std::vector<short> block(4096 * 2);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (source->GetState().availableFrames < music_stream::kInitialBufferFrames) {
    require(std::chrono::steady_clock::now() < deadline, "opening buffer timed out");
    const auto read = source->ReadFrames(source->GetState().availableFrames, block, 100ms);
    if (read.status == music_stream::ReadStatus::Error) throw std::runtime_error(read.error);
    require(read.status != music_stream::ReadStatus::Cancelled, "opening cancelled");
  }
}
void testEquivalence(const std::filesystem::path &root) {
  for (const bool club : {false, true}) {
    const auto dir = root / (club ? "club" : "plain");
    auto chart = chartAt(dir, "#00011:0102\n#00001:01000100\n");
    std::vector<short> first(48000 * 2 * 3);
    for (std::size_t i = 0; i < first.size(); ++i)
      first[i] = static_cast<short>(static_cast<int>((i * 17) % 16000) - 8000);
    writeWav(dir / "first.wav", 48000, 2, first);
    writeWav(dir / "second.wav", 22050, 1, std::vector<short>(4410, -12345));
    const auto reference = dir / "reference.wav";
    require(chart_audio::RenderChartAudioToWav(*chart, reference, {.clubMode = club}).success,
            "reference render failed");
    if (!club) {
      const std::uint64_t origin = 60123;
      const auto preview = music_stream::Source::CreatePreview(*chart, dir / "preview", {}, origin);
      const auto suffix = readAll(preview, origin);
      const auto full = readWav(reference);
      require(suffix == std::vector<short>(full.begin() + origin * 2, full.end()),
              "nonzero preview must preserve earlier tails and exact resampler phase");
      require(preview->GetState().firstFrame == origin,
              "preview must retain its absolute origin");
      std::array<short, 2> before{};
      require(preview->ReadFrames(origin - 1, before).status == music_stream::ReadStatus::Error,
              "preview must reject reads outside its prepared range");
      const auto pastEnd = music_stream::Source::CreatePreview(*chart, dir / "end", {}, 44100 * 60);
      require(readAll(pastEnd, 44100 * 60).empty(), "preview beyond all tails must end without underflow");
    }
    const auto staging = dir / "streams";
    const auto stream = music_stream::Source::Create(std::move(chart), staging, club);
    const auto actual = readAll(stream);
    require(actual == readWav(reference), "progressive PCM must match full rendering across boundaries/tails");
    std::vector<short> backward(100 * 2);
    const auto seek = stream->ReadFrames(4090, backward);
    require(seek.status == music_stream::ReadStatus::Data && seek.frames == 100 &&
                std::equal(backward.begin(), backward.end(), actual.begin() + 4090 * 2),
            "backward seek must preserve previously rendered audio");
  }
}
void testTemporaryLifetime(const std::filesystem::path &root) {
  const auto dir = root / "temporary";
  auto chart = chartAt(dir);
  writeWav(dir / "first.wav", 44100, 1, std::vector<short>(4096, 12000));
  const auto staging = dir / "streams";
  auto stream = music_stream::Source::Create(std::move(chart), staging);
  const auto pcm = readAll(stream);
  require(!pcm.empty() && stream->GetState().complete, "short stream must complete");
  require(std::filesystem::is_directory(staging),
          "completed music must retain only private temporary stream data");
  require(!std::filesystem::is_empty(staging), "active stream must own its seek data");
  stream.reset();
  require(std::filesystem::is_empty(staging),
          "releasing a completed stream must remove all of its temporary data");
}
void testLazyLongSource(const std::filesystem::path &root) {
  const auto dir = root / "long";
  auto chart = chartAt(dir, "#00011:01\n#01011:02\n");
  // 80 s stereo exceeds the allocation guard if decoded as a complete PCM vector.
  writeWav(dir / "first.wav", 44100, 2, std::vector<short>(44100 * 2, 1000), 80);
  const auto staging = dir / "streams";
  std::shared_ptr<music_stream::Source> stream;
  {
    guardLargeAllocations.store(true);
    ScopeExit restore([] { guardLargeAllocations.store(false); });
    stream = music_stream::Source::Create(std::move(chart), staging);
    waitForOpening(stream);
    const auto state = stream->GetState();
    require(!state.complete && state.availableFrames < 44100 * 20,
            "long song must become ready before full preparation");
    // This future asset did not exist during startup; eager all-asset loading
    // would permanently cache a miss and omit it from the sought block.
    writeWav(dir / "second.wav", 44100, 1, std::vector<short>(44100, 7000));
    std::vector<short> samples(20 * 2);
    music_stream::ReadResult read;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
      read = stream->ReadFrames(44100 * 20, samples, 100ms);
      require(std::chrono::steady_clock::now() < deadline, "forward seek timed out");
    } while (read.status == music_stream::ReadStatus::Pending);
    require(read.status == music_stream::ReadStatus::Data && samples[0] == 7998 &&
                samples[1] == 7998,
            "forward seek must include late assets and the earlier BGM tail");
  }
  stream->Cancel();
}
void testSeekPreparesOnlyRequestedRanges(const std::filesystem::path &root) {
  const auto dir = root / "seek-ranges";
  auto chart = chartAt(dir, "#00001:01\n#02511:02\n");
  std::vector<short> backing(48000 * 2);
  for (std::size_t i = 0; i < backing.size(); ++i)
    backing[i] = static_cast<short>(static_cast<int>((i * 17) % 16000) - 8000);
  writeWav(dir / "first.wav", 48000, 2, backing, 80);
  writeWav(dir / "second.wav", 22050, 1, std::vector<short>(22050, 7000));
  const auto referencePath = dir / "reference.wav";
  require(chart_audio::RenderChartAudioToWav(*chart, referencePath).success,
          "seek reference render failed");
  const auto reference = readWav(referencePath);
  const auto staging = dir / "streams";
  auto stream = music_stream::Source::Create(std::move(chart), staging);
  waitForOpening(stream);
  const auto readAt = [&](std::uint64_t frame) {
    std::vector<short> samples(997 * 2);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
      const auto result = stream->ReadFrames(frame, samples, 100ms);
      require(std::chrono::steady_clock::now() < deadline, "range seek timed out");
      if (result.status == music_stream::ReadStatus::Pending) continue;
      require(result.status == music_stream::ReadStatus::Data && result.frames > 0,
              "sought range must produce audio");
      require(std::equal(samples.begin(), samples.begin() + result.frames * 2,
                         reference.begin() + frame * 2),
              "sought PCM must include earlier tails with exact resampler phase");
      return;
    }
  };
  readAt(44100 * 50 + 123);
  require(stream->GetState().availableFrames < 44100 * 20,
          "forward seek must not render the skipped prefix before target audio");
  stream->SetRenderingEnabled(false);
  std::array<short, 2> paused{};
  require(stream->ReadFrames(44100 * 30 + 17, paused, 20ms).status ==
              music_stream::ReadStatus::Pending,
          "paused reads of skipped ranges must not expose sparse-file silence");
  stream->SetRenderingEnabled(true);
  readAt(44100 * 30 + 17); // Backward into a range skipped by the first seek.
  require(stream->ReadFrames(44100 * 200, paused, 100ms).status ==
              music_stream::ReadStatus::End,
          "seeking past the known end must finish without filling earlier gaps");
  readAt(41);             // Backward into the already prepared opening.
  const auto tail = readAll(stream, 44100 * 75 + 19);
  require(tail == std::vector<short>(reference.begin() + (44100 * 75 + 19) * 2,
                                    reference.end()),
          "seeking near the end must finish without filling earlier gaps");
  require(!stream->GetState().complete,
          "reaching EOF after a seek must not treat holes as prepared audio");
  require(readAll(stream) == reference,
          "reading skipped ranges later must recover the entire exact stream");
  require(stream->GetState().complete,
          "stream completion must wait until all skipped ranges are prepared");
}

void testCancelAndPause(const std::filesystem::path &root) {
  auto chart = chartAt(root / "cancel", "#10011:01\n");
  const auto staging = chart->Meta.Folder / "streams";
  std::filesystem::create_directories(staging);
  const auto sentinel = staging / "unrelated.txt";
  { std::ofstream old(sentinel); old << "unrelated file"; }
  auto stream = music_stream::Source::Create(std::move(chart), staging);
  waitForOpening(stream);
  stream->SetRenderingEnabled(false);
  std::vector<short> samples(2);
  auto waiting = std::async(std::launch::async, [&] {
    return stream->ReadFrames(44100 * 100, samples, 5s);
  });
  require(waiting.wait_for(20ms) == std::future_status::timeout,
          "a paused producer must not generate requested future audio");
  stream->Cancel();
  require(waiting.wait_for(1s) == std::future_status::ready &&
              waiting.get().status == music_stream::ReadStatus::Cancelled,
          "cancellation must wake a blocked reader");
  stream.reset();
  std::ifstream old(sentinel);
  std::string contents;
  std::getline(old, contents);
  require(contents == "unrelated file", "cancelled stream must preserve unrelated files");
  for (const auto &entry : std::filesystem::directory_iterator(staging))
    require(!entry.path().filename().string().starts_with(".music-stream-"),
            "cancelled source must clean its owned staging files");
}

void testPreviewPagesSurviveHandoff(const std::filesystem::path &root) {
  const auto dir = root / "preview-handoff";
  auto chart = chartAt(dir);
  writeWav(dir / "first.wav", 48000, 2, std::vector<short>(48000 * 2, 12000), 80);
  auto cache = std::make_shared<audio::DecodedAssetCache>(dir);
  constexpr std::uint64_t origin = 44100 * 50 + 123;
  auto preview = music_stream::Source::CreatePreview(*chart, dir, cache, origin);
  std::array<short, 32> block{};
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  music_stream::ReadResult read;
  do {
    read = preview->ReadFrames(origin, block, 100ms);
    require(std::chrono::steady_clock::now() < deadline, "highlight startup timed out");
  } while (read.status == music_stream::ReadStatus::Pending);
  require(read.status == music_stream::ReadStatus::Data && block[0] == 11999,
          "highlight must include the BGM started fifty seconds earlier");
  preview->Cancel();
  preview.reset();
  const auto prepared = cache->GetStatistics().decodedFrames;
  require(prepared > 0 && prepared < 48000 * 11,
          "highlight must decode nearby source pages, not its skipped prefix or complete BGM");
  std::atomic_bool cancelled{false}; SF_INFO info{}; std::vector<short> full;
  require(cache->Materialize(dir / "first.wav", full, info, cancelled),
          "gameplay must finish preparation after preview cancellation");
  require(cache->GetStatistics().decodedFrames == 48000 * 80 &&
              cache->GetStatistics().cacheHitFrames >= prepared,
          "gameplay must decode only the missing pages, retaining highlight preparation");
  require(full.size() == 48000 * 80 * 2,
          "gameplay handoff must expose full immutable PCM, not preview pages");
}

void testPreviewGainHeadroom(const std::filesystem::path &root) {
  const auto dir = root / "preview-gain";
  auto chart = chartAt(dir, "#00011:01\n#00001:01\n");
  writeWav(dir / "first.wav", 44100, 1, std::vector<short>(4096, 26000));
  auto preview = music_stream::Source::CreatePreview(*chart, dir, {}, 0,
      {.bgm = 0.5f, .keysound = 0.5f});
  const auto pcm = readAll(preview);
  require(!pcm.empty() && pcm[0] == 25998,
          "preview must apply master/bus gain before clipping overlapping voices");
}
}

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("music-stream-tests-" + uuid::generateV4());
  std::filesystem::create_directories(root);
  ScopeExit cleanup([&] { std::filesystem::remove_all(root); });
  try {
    testTemporaryLifetime(root);
    testEquivalence(root);
    testLazyLongSource(root);
    testSeekPreparesOnlyRequestedRanges(root);
    testCancelAndPause(root);
    testPreviewPagesSurviveHandoff(root);
    testPreviewGainHeadroom(root);
    std::cout << "progressive music source tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
