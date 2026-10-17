#include "ArchiveFile.h"
#include "ArchiveRAII.h"
#include <archive_entry.h>
#include "PlayOptionUtils.h"
#include "RAII.h"
#include "Uuid.h"
#include "audio/ChartAudioRenderer.h"
#include "audio/ProgressiveMusicSource.h"
#include "audio/SoundFileIO.h"
#include "audio/VorbisDurationBound.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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
void writeOgg(const std::filesystem::path &path, int frames, bool largeHeader = false) {
  SF_INFO info{};
  info.samplerate = 44100;
  info.channels = 1;
  info.format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;
  auto file = asobmashow::audio::openSoundFileHandle(path, SFM_WRITE, info);
  require(bool(file), "could not create Ogg fixture");
  if (largeHeader) {
    const std::string comment(70000, 'x');
    require(sf_set_string(file.get(), SF_STR_COMMENT, comment.c_str()) == 0,
            "could not write large Ogg metadata fixture");
  }
  const std::array<short, 4096> samples = [] {
    std::array<short, 4096> result{};
    result.fill(12000);
    return result;
  }();
  for (int offset = 0; offset < frames;) {
    const auto count = std::min<int>(samples.size(), frames - offset);
    require(sf_write_short(file.get(), samples.data(), count) == count,
            "could not write Ogg fixture");
    offset += count;
  }
}
void testVorbisDurationBounds(const std::filesystem::path &root) {
  const auto path = root / "duration.ogg";
  writeOgg(path, 8820);
  std::ifstream input(path, std::ios::binary);
  const std::vector<unsigned char> encoded(
      (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  const auto duration = audio::duration_bound::InspectVorbis(encoded);
  require(duration && duration->sampleRate == 44100 && duration->frames >= 8820 &&
              duration->frames < 10000,
          "small Vorbis keysounds need a conservative duration without opening a decoder");
  require(!audio::duration_bound::InspectVorbis(std::span(encoded).first(encoded.size() - 1)),
          "truncated Vorbis must fall back to the decoder");
  auto corrupt = encoded;
  corrupt.back() ^= 1;
  require(!audio::duration_bound::InspectVorbis(corrupt),
          "corrupt Ogg pages must not supply duration bounds");
  auto chained = encoded;
  chained.insert(chained.end(), encoded.begin(), encoded.end());
  require(!audio::duration_bound::InspectVorbis(chained),
          "chained streams must fall back to the decoder");
  const std::array<unsigned char, 4> other{'R', 'I', 'F', 'F'};
  require(!audio::duration_bound::InspectVorbis(other),
          "non-Vorbis assets must fall back to their normal decoder");
}
std::unique_ptr<bms_parser::Chart> chartAt(const std::filesystem::path &folder,
                                         const std::string &body = "#00011:01\n",
                                         int bpm = 120) {
  std::filesystem::create_directories(folder);
  const auto path = folder / "chart.bms";
  { std::ofstream file(path);
    file << "#PLAYER 1\n#TITLE Stream fixture\n#BPM " << bpm << "\n"
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
void testCompressedSourceDecoderLifetime(const std::filesystem::path &root) {
  struct Case { int count; bool repeat; };
  // Cover the idle-cache boundary, voice retirement/reuse, and the FD ceiling.
  for (const auto [count, repeat] : {Case{32, false}, Case{33, false},
                                    Case{64, true}, Case{192, false}}) {
    const auto dir = root / ("overlapping-ogg-" + std::to_string(count) +
                             (repeat ? "-repeat" : ""));
    std::filesystem::create_directories(dir);
    const auto fixture = dir / "layer.ogg";
    {
      SF_INFO info{};
      info.samplerate = count > 128 ? 48000 : 44100;
      info.channels = count > 128 ? 2 : 1;
      info.format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;
      auto file = asobmashow::audio::openSoundFileHandle(fixture, SFM_WRITE, info);
      require(bool(file), "could not create overlapping Ogg fixture");
      // Three decode blocks still exceed the two-opens-per-source guard if
      // active decoders are incorrectly reopened on every block.
      std::vector<short> samples(count > 128 ? 32000 : 8193);
      for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<short>(static_cast<int>((i * 17) % 801) - 400);
      require(sf_write_short(file.get(), samples.data(), samples.size()) ==
                  static_cast<sf_count_t>(samples.size()),
              "could not write overlapping Ogg fixture");
    }
    const std::string digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string body;
    for (int index = 0; index < count; ++index) {
      const int wav = index + 10;
      const std::string code{digits[wav / 36], digits[wav % 36]};
      const auto name = "layer-" + std::to_string(index) + ".ogg";
      std::filesystem::copy_file(fixture, dir / name);
      body += "#WAV" + code + " " + name + "\n#00001:" + code + "\n";
      // All first-burst voices expire before the second burst at 0.8s.
      if (repeat) body += "#00201:" + code + "\n";
    }
    auto chart = chartAt(dir, body, 600);
    const auto referencePath = dir / "reference.wav";
    require(chart_audio::RenderChartAudioToWav(*chart, referencePath).success,
            "overlapping Ogg reference render failed");
#if !defined(_WIN32)
    struct rlimit previous{};
    require(getrlimit(RLIMIT_NOFILE, &previous) == 0, "could not query FD limit");
    auto constrained = previous;
    if (count > 128) constrained.rlim_cur = std::min<rlim_t>(160, previous.rlim_cur);
    require(setrlimit(RLIMIT_NOFILE, &constrained) == 0, "could not constrain FD limit");
    ScopeExit restoreLimit([&] { setrlimit(RLIMIT_NOFILE, &previous); });
#endif
    const auto stream = music_stream::Source::Create(std::move(chart), dir / "streams");
    const auto actual = readAll(stream);
    const auto opens = stream->GetStatistics().decoderOpens;
    if (count <= 128 && opens > static_cast<std::uint64_t>(count) * 2)
      throw std::runtime_error("active compressed sources must not reopen every block: " +
          std::to_string(count) + " sources opened " + std::to_string(opens) + " decoders");
    if (repeat)
      require(opens > static_cast<std::uint64_t>(count),
              "completed voices must release excess idle decoders before the next burst");
    const auto reference = readWav(referencePath);
    if (actual != reference) {
      const auto mismatch = std::mismatch(actual.begin(), actual.end(),
                                          reference.begin(), reference.end());
      throw std::runtime_error("overlapping compressed voices differ: " +
          std::to_string(count) + " sources, actual samples " + std::to_string(actual.size()) +
          ", reference samples " + std::to_string(reference.size()) + ", mismatch " +
          std::to_string(mismatch.first - actual.begin()) + ", actual " +
          (mismatch.first == actual.end() ? "end" : std::to_string(*mismatch.first)) +
          ", reference " +
          (mismatch.second == reference.end() ? "end" : std::to_string(*mismatch.second)));
    }
  }
}
void testAbandonedLeaseCleanup(const std::filesystem::path &root) {
  const auto dir = root / "abandoned-lease";
  auto chart = chartAt(dir);
  const auto staging = dir / "music-streams";
  const auto abandoned = staging / (".music-stream-" + uuid::generateV4());
  std::filesystem::create_directories(abandoned);
  { std::ofstream lease(abandoned / ".lease"); }
  std::filesystem::permissions(abandoned / ".lease",
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  { std::ofstream audio(abandoned / "audio.wav"); audio << "abandoned"; }
  const auto stream = music_stream::Source::Create(std::move(chart), staging);
  readAll(stream);
  require(!std::filesystem::exists(abandoned),
          "an unlocked abandoned lease and its directory must be fully removed");
}
void testAbandonedStagingCleanup(const std::filesystem::path &root, const char *executable) {
#if !defined(_WIN32)
  const auto dir = root / "abandoned";
  const auto staging = dir / "music-streams";
  const auto child = fork();
  require(child >= 0, "could not fork crashed-source fixture");
  if (child == 0) {
    execl(executable, executable, "--abandon-staging", dir.c_str(), nullptr);
    _exit(1);
  }
  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "crashed-source fixture failed");
  std::filesystem::path abandoned;
  for (const auto &entry : std::filesystem::directory_iterator(staging))
    if (entry.path().filename().string().starts_with(".music-stream-")) abandoned = entry.path();
  require(!abandoned.empty(), "crash must leave an owned staging directory");
  const auto unrelated = staging / ".music-stream-unrelated";
  std::filesystem::create_directory(unrelated);
  const auto outside = dir / "outside";
  std::filesystem::create_directory(outside);
  { std::ofstream sentinel(outside / "keep"); sentinel << "preserve"; }
  const auto link = staging / (".music-stream-" + uuid::generateV4());
  std::filesystem::create_directory_symlink(outside, link);
  const auto oldLegacy = staging / (".music-stream-" + uuid::generateV4());
  const auto freshLegacy = staging / (".music-stream-" + uuid::generateV4());
  std::filesystem::create_directory(oldLegacy);
  std::filesystem::create_directory(freshLegacy);
  { std::ofstream stale(oldLegacy / "audio.wav"); stale << "abandoned old stream"; }
  std::filesystem::last_write_time(oldLegacy,
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8));
  const auto unsafeLease = staging / (".music-stream-" + uuid::generateV4());
  std::filesystem::create_directory(unsafeLease);
  std::filesystem::create_symlink(outside / "keep", unsafeLease / ".lease");
  auto live = music_stream::Source::Create(chartAt(dir), staging);
  const auto expected = readAll(live);
  require(!std::filesystem::exists(abandoned), "new source must sweep crashed staging at the actual stream root");
  require(!std::filesystem::exists(oldLegacy) && std::filesystem::exists(freshLegacy),
          "legacy staging cleanup must remove old orphans but preserve fresh unleased directories");
  int ready[2], stop[2];
  require(pipe(ready) == 0 && pipe(stop) == 0, "could not create child lifetime pipes");
  const auto holding = fork();
  require(holding >= 0, "could not fork active-source fixture");
  if (holding == 0) {
    close(ready[0]);
    close(stop[1]);
    const auto readyFd = std::to_string(ready[1]);
    const auto stopFd = std::to_string(stop[0]);
    execl(executable, executable, "--hold-staging", dir.c_str(),
          readyFd.c_str(), stopFd.c_str(), nullptr);
    _exit(1);
  }
  close(ready[1]);
  close(stop[0]);
  bool childReaped = false;
  ScopeExit stopChild([&] {
    close(ready[0]);
    if (stop[1] >= 0) close(stop[1]);
    if (!childReaped) waitpid(holding, &status, 0);
  });
  char signal = 0;
  require(read(ready[0], &signal, 1) == 1, "active child source failed to start");
  std::vector<std::filesystem::path> heldDirectories;
  for (const auto &entry : std::filesystem::directory_iterator(staging)) {
    if (std::filesystem::is_regular_file(entry.path() / "audio.wav")) {
      heldDirectories.push_back(entry.path());
      // Age alone must never override a live modern lease.
      std::filesystem::last_write_time(entry.path(),
          std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8));
    }
  }
  require(heldDirectories.size() == 2, "expected parent and child source staging");
  auto concurrent = music_stream::Source::Create(chartAt(dir), staging);
  require(readAll(concurrent) == expected && readAll(live) == expected,
          "sweeping must preserve another live source's seek data");
  for (const auto &held : heldDirectories)
    require(std::filesystem::exists(held / "audio.wav"),
            "sweeping must preserve another process's active lease regardless of age");
  require(std::filesystem::exists(unsafeLease) &&
              std::filesystem::exists(unrelated) && std::filesystem::is_symlink(link) &&
              std::filesystem::exists(outside / "keep"),
          "staging sweep must preserve unrelated directories and symlink targets");
  close(stop[1]);
  stop[1] = -1;
  const auto reaped = waitpid(holding, &status, 0);
  childReaped = reaped == holding;
  require(childReaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "another process must still read its source after cleanup");
#endif
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
  for (const auto &entry : std::filesystem::directory_iterator(staging))
    require(entry.path().filename() == ".music-stream.lock",
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
  auto chart = chartAt(dir, "#00001:01\n#01211:02\n");
  std::vector<short> backing(48000 * 2);
  for (std::size_t i = 0; i < backing.size(); ++i)
    backing[i] = static_cast<short>(static_cast<int>((i * 17) % 16000) - 8000);
  // Retain seeks beyond the 10 s read-ahead and 20 s eager-work guard.
  writeWav(dir / "first.wav", 48000, 2, backing, 32);
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
  readAt(44100 * 24 + 123);
  require(stream->GetState().availableFrames < 44100 * 20,
          "forward seek must not render the skipped prefix before target audio");
  stream->SetRenderingEnabled(false);
  std::array<short, 2> paused{};
  require(stream->ReadFrames(44100 * 16 + 17, paused, 20ms).status ==
              music_stream::ReadStatus::Pending,
          "paused reads of skipped ranges must not expose sparse-file silence");
  stream->SetRenderingEnabled(true);
  readAt(44100 * 16 + 17); // Backward into a range skipped by the first seek.
  require(stream->ReadFrames(44100 * 200, paused, 100ms).status ==
              music_stream::ReadStatus::End,
          "seeking past the known end must finish without filling earlier gaps");
  const auto oversized = std::numeric_limits<std::uint64_t>::max();
  stream->RequestSeek(oversized);
  require(stream->ReadFrames(oversized, paused, 100ms).status ==
              music_stream::ReadStatus::End,
          "oversized seek must acknowledge its clamped target and report EOF");
  readAt(41);             // Backward into the already prepared opening.
  const auto tail = readAll(stream, 44100 * 28 + 19);
  require(tail == std::vector<short>(reference.begin() + (44100 * 28 + 19) * 2,
                                    reference.end()),
          "seeking near the end must finish without filling earlier gaps");
  require(!stream->GetState().complete,
          "reaching EOF after a seek must not treat holes as prepared audio");
  require(readAll(stream) == reference,
          "reading skipped ranges later must recover the entire exact stream");
  require(stream->GetState().complete,
          "stream completion must wait until all skipped ranges are prepared");
}

void testColdSeekSkipsExpiredVorbisDecoders(const std::filesystem::path &root,
                                            std::string_view archiveExtension = {},
                                            bool largeOgg = false) {
  const bool archived = !archiveExtension.empty();
  const auto dir = root / ((archived ? "cold-archive-seek" + std::string(archiveExtension)
                                    : "cold-vorbis-seek") + (largeOgg ? "-large" : ""));
  std::filesystem::create_directories(dir);
  writeOgg(dir / "tap.ogg", 8820, largeOgg);
  if (largeOgg) require(std::filesystem::file_size(dir / "tap.ogg") > 64U * 1024U,
                        "large Ogg fixture must exceed the single-file probe limit");
  writeWav(dir / "first.wav", 44100, 1, std::vector<short>(44100, 3000), 65);
  const std::string digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const auto code = [&](int value) {
    return std::string{digits[value / 36], digits[value % 36]};
  };
  std::string body = "#00001:01\n";
  for (int index = 0; index < 64; ++index) {
    const auto filename = "tap-" + std::to_string(index) + ".ogg";
    std::filesystem::copy_file(dir / "tap.ogg", dir / filename);
    body += "#WAV" + code(index + 10) + " " + filename + "\n";
  }
  for (int measure = 0; measure < 32; ++measure) {
    auto number = std::to_string(measure);
    number.insert(0, 3 - number.size(), '0');
    body += "#" + number + "11:" + code(measure * 2 + 10) +
            code(measure * 2 + 11) + "\n";
  }
  // Repeated sounds and aliases must retain the occurrence overlapping the
  // target, even when another occurrence/alias in the same batch has expired.
  writeWav(dir / "second.wav", 44100, 1, std::vector<short>(4410, 1000));
  body += "#01212:" + code(60) + "\n#WAV04 tap-50.ogg\n#02513:04\n"
          "#02012:02\n#WAV03 second.wav\n#02512:03\n";
  auto chart = chartAt(dir, body);
  const auto referencePath = dir / "reference.wav";
  require(chart_audio::RenderChartAudioToWav(*chart, referencePath).success,
          "Vorbis seek reference failed");
  const auto reference = readWav(referencePath);
  if (archived) {
    const auto archivePath = root / ((largeOgg ? "cold-seek-large" : "cold-seek") +
                                     std::string(archiveExtension));
    auto writer = makeArchiveWriteHandle();
    const auto format = archiveExtension == ".7z" ? archive_write_set_format_7zip(writer.get())
                                                  : archive_write_set_format_zip(writer.get());
    require(format == ARCHIVE_OK &&
                archive_write_open_filename(writer.get(), archivePath.string().c_str()) == ARCHIVE_OK,
            "could not create archive fixture");
    for (const auto &file : std::filesystem::directory_iterator(dir)) {
      if (file.path() == referencePath) continue;
      std::ifstream input(file.path(), std::ios::binary);
      const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
      std::unique_ptr<archive_entry, decltype(&archive_entry_free)>
          entry(archive_entry_new(), archive_entry_free);
      archive_entry_set_pathname(entry.get(), file.path().filename().string().c_str());
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_perm(entry.get(), 0644);
      archive_entry_set_size(entry.get(), bytes.size());
      require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK &&
                  archive_write_data(writer.get(), bytes.data(), bytes.size()) ==
                      static_cast<la_ssize_t>(bytes.size()) &&
                  archive_write_finish_entry(writer.get()) == ARCHIVE_OK,
              "could not write archived keysound fixture");
    }
    require(archive_write_close(writer.get()) == ARCHIVE_OK, "could not finish archive fixture");
    std::atomic_bool cancelled{false};
    chart = play_options::parseChart(archivePath / "chart.bms", cancelled, "archive seek test");
    require(bool(chart), "could not parse archived chart fixture");
  }
  auto stream = music_stream::Source::Create(std::move(chart), dir / "streams");
  waitForOpening(stream);
  stream->SetRenderingEnabled(false);
  const auto before = stream->GetStatistics().decoderOpens;
  const auto archiveReadsBefore = stream->GetStatistics().archiveReads;
  constexpr std::uint64_t target = 50 * 44100 + 123;
  std::array<short, 1994> samples{};
  stream->ReadFrames(target, samples, 0ms);
  stream->SetRenderingEnabled(true);
  music_stream::ReadResult result;
  do { result = stream->ReadFrames(target, samples, 100ms); }
  while (result.status == music_stream::ReadStatus::Pending);
  stream->SetRenderingEnabled(false);
  require(result.status == music_stream::ReadStatus::Data && result.frames > 0 &&
              std::equal(samples.begin(), samples.begin() + result.frames * 2,
                         reference.begin() + target * 2),
          "cold Vorbis seek must preserve overlapping tails and target PCM");
  require(stream->GetStatistics().decoderOpens - before < 8,
          "cold seek must not initialize every expired Vorbis decoder in the skipped prefix");
  if (archived) {
    const auto reads = stream->GetStatistics().archiveReads - archiveReadsBefore;
    require(reads > 0 && reads <= 2,
            "cold seek must batch archived keysounds instead of repeatedly extracting a solid prefix");
  }
  stream->SetRenderingEnabled(true);
  require(readAll(stream) == reference,
          "skipped Vorbis assets must still play correctly when seeking backward");
}

void testSpacedArchivedSoundsUseStagingWindows(const std::filesystem::path &root) {
  const auto dir = root / "spaced-archive";
  std::filesystem::create_directories(dir);
  const std::string digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string body;
  std::vector<std::filesystem::path> paths;
  for (int measure = 0; measure < 2; ++measure) {
    std::string notes;
    for (int note = 0; note < 16; ++note) {
      const int index = measure * 16 + note;
      const int wav = index + 10;
      const std::string code{digits[wav / 36], digits[wav % 36]};
      const auto name = "tap-" + std::to_string(index) + ".wav";
      paths.push_back(dir / name);
      std::vector<short> samples(8820);
      for (std::size_t frame = 0; frame < samples.size(); ++frame)
        samples[frame] = static_cast<short>(index * 100 + static_cast<int>(frame % 301) - 1500);
      writeWav(paths.back(), 44100, 1, samples);
      body += "#WAV" + code + " " + name + "\n";
      notes += code;
    }
    body += "#00" + std::to_string(measure) + "11:" + notes + "\n";
  }
  auto chart = chartAt(dir, body);
  const auto referencePath = dir / "reference.wav";
  require(chart_audio::RenderChartAudioToWav(*chart, referencePath).success,
          "spaced archive reference render failed");
  const auto reference = readWav(referencePath);
  paths.push_back(dir / "chart.bms");
  const auto archivePath = root / "spaced-sounds.7z";
  auto writer = makeArchiveWriteHandle();
  require(archive_write_set_format_7zip(writer.get()) == ARCHIVE_OK &&
              archive_write_open_filename(writer.get(), archivePath.string().c_str()) == ARCHIVE_OK,
          "could not create spaced archive fixture");
  for (const auto &path : paths) {
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    std::unique_ptr<archive_entry, decltype(&archive_entry_free)>
        entry(archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), path.filename().string().c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), bytes.size());
    require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK &&
                archive_write_data(writer.get(), bytes.data(), bytes.size()) ==
                    static_cast<la_ssize_t>(bytes.size()) &&
                archive_write_finish_entry(writer.get()) == ARCHIVE_OK,
            "could not write spaced archive fixture");
  }
  require(archive_write_close(writer.get()) == ARCHIVE_OK,
          "could not finish spaced archive fixture");
  std::atomic_bool cancelled{false};
  chart = play_options::parseChart(archivePath / "chart.bms", cancelled, "spaced archive test");
  require(bool(chart), "could not parse spaced archive fixture");
  const auto stream = music_stream::Source::Create(std::move(chart), dir / "streams");
  require(readAll(stream) == reference, "spaced archived keysounds must preserve exact mixed PCM");
  const auto reads = stream->GetStatistics().archiveReads;
  if (reads > 3)
    throw std::runtime_error("32 spaced archived keysounds must use staging windows, read " +
                            std::to_string(reads) + " archive sessions");
}

void testSeekInterruptsObsoletePreparation(const std::filesystem::path &root) {
  const auto dir = root / "seek-preemption";
  std::filesystem::create_directories(dir);
  writeOgg(dir / "long.ogg", 65 * 44100);
  writeWav(dir / "first.wav", 44100, 1, std::vector<short>(44100, 3000), 65);
  const std::string digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string body = "#00001:01\n";
  for (int index = 0; index < 64; ++index) {
    const auto filename = "long-" + std::to_string(index) + ".ogg";
    std::filesystem::copy_file(dir / "long.ogg", dir / filename);
    const auto value = index + 10;
    const std::string code{digits[value / 36], digits[value % 36]};
    body += "#WAV" + code + " " + filename + "\n#02001:" + code + "\n";
  }
  auto stream = music_stream::Source::Create(chartAt(dir, body), dir / "streams");
  waitForOpening(stream);
  const auto before = stream->GetStatistics().decoderOpens;
  std::array<short, 32> samples{};
  stream->ReadFrames(50 * 44100, samples, 0ms);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (stream->GetStatistics().decoderOpens < before + 3) {
    require(std::chrono::steady_clock::now() < deadline, "obsolete work did not start");
    std::this_thread::yield();
  }
  // Supersede a block while it is opening dozens of overlapping compressed
  // sounds. The new target only needs the backing sound already opened at start.
  constexpr std::uint64_t target = 20 * 44100 + 123;
  stream->RequestSeek(target);
  require(stream->ReadFrames(50 * 44100, samples, 0ms).status ==
              music_stream::ReadStatus::Pending,
          "a stale feeder read must not replace the newly requested seek");
  music_stream::ReadResult result;
  do { result = stream->ReadFrames(target, samples, 100ms); }
  while (result.status == music_stream::ReadStatus::Pending &&
         std::chrono::steady_clock::now() < deadline);
  require(result.status == music_stream::ReadStatus::Data && samples[0] == 2999,
          "superseding seek must render the new target");
  stream->SetRenderingEnabled(false);
  require(stream->GetStatistics().decoderOpens - before < 32,
          "seek must abandon obsolete preparation before opening the entire old block");
}

void testSeekWakesWaitingRead(const std::filesystem::path &root) {
  auto stream = music_stream::Source::Create(
      chartAt(root / "waiting-seek", "#10011:01\n"), root / "waiting-streams");
  waitForOpening(stream);
  stream->SetRenderingEnabled(false);
  std::array<short, 2> oldSamples{};
  auto waiting = std::async(std::launch::async, [&] {
    return stream->ReadFrames(100 * 44100, oldSamples, 5s);
  });
  require(waiting.wait_for(20ms) == std::future_status::timeout,
          "old feeder read should wait for unavailable audio");
  stream->RequestSeek(50 * 44100);
  require(waiting.wait_for(500ms) == std::future_status::ready &&
              waiting.get().status == music_stream::ReadStatus::Pending,
          "seeking must wake an obsolete read instead of waiting for its timeout");
  stream->Cancel();
  require(stream->ReadFrames(100 * 44100, oldSamples).status == music_stream::ReadStatus::Cancelled,
          "cancellation must take precedence over a pending seek");
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


}

int main(int argc, char **argv) {
#if !defined(_WIN32)
  if (argc > 2 && (std::string_view(argv[1]) == "--abandon-staging" ||
                   std::string_view(argv[1]) == "--hold-staging")) {
    try {
      const std::filesystem::path dir = argv[2];
      const auto stream = music_stream::Source::Create(chartAt(dir), dir / "music-streams");
      const auto expected = readAll(stream);
      if (std::string_view(argv[1]) == "--abandon-staging") _exit(0);
      char signal = 1;
      if (argc != 5 || write(std::stoi(argv[3]), &signal, 1) != 1) return 1;
      read(std::stoi(argv[4]), &signal, 1);
      return readAll(stream) == expected ? 0 : 1;
    } catch (...) { return 1; }
  }
#endif
  const auto root = std::filesystem::temp_directory_path() /
                    ("music-stream-tests-" + uuid::generateV4());
  std::filesystem::create_directories(root);
  ScopeExit cleanup([&] { std::filesystem::remove_all(root); });
  try {
    if (argc > 1 && std::string_view(argv[1]) == "--decoder-limit") {
      testCompressedSourceDecoderLifetime(root);
      return 0;
    }
    testAbandonedLeaseCleanup(root);
    testAbandonedStagingCleanup(root, argv[0]);
    testVorbisDurationBounds(root);
    testTemporaryLifetime(root);
    testEquivalence(root);
    testCompressedSourceDecoderLifetime(root);
    testLazyLongSource(root);
    testSeekPreparesOnlyRequestedRanges(root);
    testColdSeekSkipsExpiredVorbisDecoders(root);
    testColdSeekSkipsExpiredVorbisDecoders(root, ".7z");
    testColdSeekSkipsExpiredVorbisDecoders(root, ".zip");
    testColdSeekSkipsExpiredVorbisDecoders(root, ".zip", true);
    testSpacedArchivedSoundsUseStagingWindows(root);
    testSeekInterruptsObsoletePreparation(root);
    testSeekWakesWaitingRead(root);
    testCancelAndPause(root);
    std::cout << "progressive music source tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
