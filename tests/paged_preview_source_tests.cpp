#include "audio/PagedPreviewSource.h"
#include "audio/AudioMix.h"
#include "audio/DecodedAssetCache.h"
#include "audio/SoundFileIO.h"
#include "bms_parser.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
thread_local bool forbidAllocation = false;
thread_local bool bypassPreparationGate = false;
std::atomic_bool preparationGate{false};
std::atomic_bool preparationBlocked{false};
}
void *operator new(std::size_t bytes) {
  if (forbidAllocation) throw std::bad_alloc();
  // Pause the real worker's first large unused-asset read. The short scheduled
  // sound fits below this threshold, so startup must publish before this gate.
  if (!bypassPreparationGate && bytes >= 16384 && preparationGate.load()) {
    preparationBlocked.store(true);
    while (preparationGate.load()) std::this_thread::yield();
  }
  if (auto *memory = std::malloc(bytes ? bytes : 1)) return memory;
  throw std::bad_alloc();
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace std::chrono_literals;
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void writeWav(const std::filesystem::path &path, int rate,
              const std::vector<short> &samples, int channels = 1) {
  SF_INFO info{};
  info.samplerate = rate;
  info.channels = channels;
  info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
  auto file = asobmashow::audio::openSoundFileHandle(path, SFM_WRITE, info);
  require(bool(file), "cannot write fixture");
  require(sf_write_short(file.get(), samples.data(), samples.size()) ==
              static_cast<sf_count_t>(samples.size()), "short fixture write");
}
std::unique_ptr<bms_parser::Chart> chart(const std::filesystem::path &root,
                                       const std::string &body) {
  const auto path = root / "fixture.bms";
  { std::ofstream out(path);
    out << "#PLAYER 1\n#TITLE Paged test\n#BPM 120\n"
           "#WAV01 first.wav\n#WAV02 second.wav\n" << body; }
  std::atomic_bool cancelled{false};
  bms_parser::Parser parser;
  bms_parser::Chart *result = nullptr;
  parser.Parse(path, &result, false, false, cancelled);
  require(result != nullptr, "could not parse fixture");
  return std::unique_ptr<bms_parser::Chart>(result);
}
void waitPrepared(const std::shared_ptr<audio::PagedPreviewSource> &source) {
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (!source->GetState().preparationComplete) {
    const auto state = source->GetState();
    require(state.error.empty(), state.error.c_str());
    require(std::chrono::steady_clock::now() < deadline, "preparation timed out");
    source->WaitForChange(state.revision, 20ms);
  }
}
std::vector<float> renderAll(const std::shared_ptr<audio::PagedPreviewSource> &source) {
  std::vector<float> samples;
  std::vector<float> block(997 * 2);
  for (;;) {
    forbidAllocation = true;
    const auto rendered = source->Render(block, 997);
    forbidAllocation = false;
    if (rendered.status == audio::PagedPreviewRenderStatus::End) return samples;
    require(rendered.status == audio::PagedPreviewRenderStatus::Data,
            "fully prepared source unexpectedly starved");
    samples.insert(samples.end(), block.begin(), block.begin() + rendered.frames * 2);
  }
}
void testHighlightAndReuse(const std::filesystem::path &root) {
  // At one second, the BGM tail (8192) overlaps a key sound (4096).
  writeWav(root / "first.wav", 22050, std::vector<short>(22050 * 3, 8192));
  writeWav(root / "second.wav", 48000, std::vector<short>(48000, 4096));
  auto parsed = chart(root, "#00001:01\n#00011:0002\n");
  parsed->ReferencedWavTable.emplace(9, "referenced.wav");
  writeWav(root / "referenced.wav", 44100, std::vector<short>(8193, 1200));
  auto cache = std::make_shared<audio::DecodedAssetCache>(root / "cache",
      audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  std::atomic_bool cancelled{false};
  std::vector<short> prefix;
  SF_INFO info{};
  require(cache->ReadRange(root / "first.wav", 0, 4096, prefix, info, cancelled),
          "cannot seed partial gameplay cache");
  auto source = audio::PagedPreviewSource::Create(*parsed, cache, 44100, {0.5f, 0.25f});
  parsed.reset(); // The source must own a synchronous snapshot.
  std::vector<float> pending(16, 1.0f);
  require(source->Render(pending, 8).status == audio::PagedPreviewRenderStatus::Pending,
          "unstarted source must be pending");
  require(source->GetState().positionFrames44100 == 44100,
          "pending callbacks must freeze the chart clock");
  require(source->Begin(48000), "cannot begin preview");
  waitPrepared(source);
  const auto samples = renderAll(source);
  require(samples.size() == 96000 * 2, "highlight must retain the earlier tail");
  const float overlap = 0.9f / 32768.0f * (8191.0f * 0.5f + 4095.0f * 0.25f);
  require(std::abs(samples[0] - overlap) < 0.00001f,
          "highlight tails and independent bus gains must reach the shared mixer");
  require(std::abs(samples[48000 * 2] - (8191.0f * 0.45f / 32768.0f)) < 0.00001f,
          "ended keysound must leave only the BGM tail");
  const auto referencedBefore = cache->GetStatistics();
  std::vector<short> referenced;
  require(cache->Materialize(root / "referenced.wav", referenced, info, cancelled),
          "cannot materialize referenced gameplay audio");
  require(referenced.size() == 8193 &&
              cache->GetStatistics().decodedFrames == referencedBefore.decodedFrames,
          "referenced WAVs must have every native page retained for gameplay");
  const auto before = cache->GetStatistics();
  auto next = audio::PagedPreviewSource::Create(*chart(root, "#00001:01\n#00011:0002\n"), cache, 0);
  require(next->Begin(44100), "cannot begin reused preview");
  waitPrepared(next);
  require(cache->GetStatistics().decodedFrames == before.decodedFrames,
          "fully warmed native PCM must survive preview EOF and reuse");
  require(cache->GetStatistics().cacheHitFrames > before.cacheHitFrames,
          "second preview must consume the native cache");
  require(!std::filesystem::exists(root / "cache") ||
          std::filesystem::is_empty(root / "cache"), "preview must not spool mixed PCM");
  next->Cancel();
  require(next->GetState().cancelled, "Cancel must be terminal");
}
void testResampling(const std::filesystem::path &root) {
  std::vector<short> native(13001 * 2);
  for (std::size_t i = 0; i < native.size(); ++i) native[i] = short(int((i * 137) % 30000) - 15000);
  writeWav(root / "first.wav", 32000, native, 2);
  for (int outputRate : {44100, 48000}) {
    auto source = audio::PagedPreviewSource::Create(*chart(root, "#00011:01\n"), {}, 0);
    require(source->Begin(outputRate), "cannot start resampling preview");
    waitPrepared(source);
    const auto actual = renderAll(source);
    auto decoded = native;
    for (auto &value : decoded) value = static_cast<short>(int(value) * 32767 / 32768);
    const auto expected = audio::ResamplePcm(decoded, 2, 32000, outputRate);
    require(actual.size() >= expected.size(), "duration must contain all resampled PCM");
    for (std::size_t i = 0; i < expected.size(); ++i)
      require(std::abs(actual[i] - expected[i] * (0.9f / 32768.0f)) < 0.000001f,
              "paged resampling differs at a seek/page/interpolation endpoint");
  }
}
void testDownsamplingWarmsEveryNativePage(const std::filesystem::path &root) {
  // 8193 native frames at 192 kHz produce 1882 output frames at 44.1 kHz.
  // The last interpolation endpoint is native frame 8190, so output preparation
  // alone never requests the final native page beginning at frame 8192.
  writeWav(root / "first.wav", 192000, std::vector<short>(8193, 8192));
  writeWav(root / "referenced.wav", 192000, std::vector<short>(8193, 4096));
  auto parsed = chart(root, "#00011:01\n");
  parsed->ReferencedWavTable.emplace(9, "referenced.wav");
  auto cache = std::make_shared<audio::DecodedAssetCache>(root / "cache",
      audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  auto source = audio::PagedPreviewSource::Create(*parsed, cache, 0);
  require(source->Begin(44100), "cannot begin downsampling preview");
  waitPrepared(source);
  std::atomic_bool cancelled{false};
  for (const auto *name : {"first.wav", "referenced.wav"}) {
    const auto before = cache->GetStatistics();
    std::vector<short> native;
    SF_INFO info{};
    require(cache->Materialize(root / name, native, info, cancelled),
            "cannot materialize native gameplay PCM after downsampled preview");
    require(native.size() == 8193 && info.samplerate == 192000,
            "gameplay materialization must contain the complete native asset");
    require(cache->GetStatistics().decodedFrames == before.decodedFrames,
            "preparationComplete must retain native pages skipped by downsampling for selected and referenced WAVs");
  }
}
void testFractionalTiming(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 44100, {16384, 0, -8192, 4096, 0, 0});
  auto parsed = chart(root, "#00011:01010101\n");
  const std::vector<long long> times{31, 207777, 999999, 1000001};
  std::size_t noteIndex = 0;
  for (auto *measure : parsed->Measures)
    for (auto *timeline : measure->TimeLines)
      for (const auto *note : timeline->Notes)
        if (note && note->Wav != bms_parser::Parser::NoWav)
          timeline->Timing = times.at(noteIndex++);
  require(noteIndex == times.size(), "fractional timing fixture must contain four notes");
  const std::vector<short> decoded{16383, 0, -8191, 4095, 0, 0};
  for (int rate : {44100, 48000, 768000}) {
    auto source = audio::PagedPreviewSource::Create(*parsed, {}, 0);
    require(source->Begin(rate), "cannot begin fractional-timing preview");
    waitPrepared(source);
    SoundData contiguous;
    contiguous.channels = 1;
    contiguous.outputData = audio::ResamplePcm(decoded, 1, 44100, rate);
    contiguous.outputFrameCount = contiguous.outputData.size();
    AudioCallbackState reference;
    for (std::size_t i = 0; i < times.size(); ++i)
      require(audio::playback::InsertScheduledSound(reference,
          {.soundData = &contiguous, .bus = audio::Bus::Keysound,
           .startMicros = times[i], .sequence = i}), "cannot schedule reference sound");
    const std::vector<std::uint32_t> partitions{1, 31, 999, 13, 4097};
    std::uint32_t position = 0;
    std::size_t partition = 0;
    std::vector<float> actual(4097 * 2), expected(4097 * 2);
    for (;;) {
      const auto count = partitions[partition++ % partitions.size()];
      const auto rendered = source->Render(actual, count);
      if (rendered.status == audio::PagedPreviewRenderStatus::End) break;
      require(rendered.status == audio::PagedPreviewRenderStatus::Data,
              "prepared fractional-timing source starved");
      std::fill(expected.begin(), expected.end(), 0.0f);
      audio::playback::ActivateScheduledSounds(reference, 0, rate, rendered.frames,
                                              100, position);
      audio::playback::MixActiveSounds(reference, expected, rendered.frames, 2, 1, 1, 100);
      for (std::size_t sample = 0; sample < rendered.frames * 2; ++sample)
        require(actual[sample] == expected[sample],
                "source onset shifted from shared mixer timing across callback partitions");
      position += rendered.frames;
    }
  }
}
void testProgressiveStartup(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 44100, std::vector<short>(100, 16384));
  writeWav(root / "second.wav", 44100, std::vector<short>(44100 * 4, 8192));
  auto source = audio::PagedPreviewSource::Create(*chart(root, "#00011:01\n"), {}, 0);
  preparationBlocked.store(false);
  preparationGate.store(true);
  require(source->Begin(44100), "cannot begin progressive preview");
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!preparationBlocked.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const auto state = source->GetState();
  preparationGate.store(false); // Always release before an assertion can unwind.
  require(preparationBlocked.load(), "unused asset warming did not reach the allocation gate");
  require(state.startupReady && !state.preparationComplete,
          "startup must precede full preparation of unused assets");
  std::vector<float> block(64 * 2);
  require(source->Render(block, 64).status == audio::PagedPreviewRenderStatus::Data,
          "startup PCM must be playable before full preparation");
  require(std::abs(block[0] - 16383.0f * (0.9f / 32768.0f)) < 0.000001f,
          "startup callback must play the prepared scheduled sound");
  // No further Render calls: completion must never require consumption.
  waitPrepared(source);
  require(source->GetState().positionFrames44100 == 64,
          "background warming must not move the callback chart clock");
  source->Cancel();
  require(source->GetState().cancelled, "Cancel must join the producer");
}
void testFullCallbackPreflightPreservesScheduledAndActiveVoices(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 44100, std::vector<short>(100, 16384));
  writeWav(root / "second.wav", 44100, std::vector<short>(44100 * 4, 8192));
  auto source = audio::PagedPreviewSource::Create(*chart(root, "#00011:01\n#00111:02\n"), {}, 0);
  constexpr std::uint32_t largeFrames = 44100 * 3;
  std::vector<float> large(largeFrames * 2, 1.0f), small(64 * 2);
  forbidAllocation = true;
  const auto unstarted = source->CheckRender(largeFrames);
  forbidAllocation = false;
  preparationBlocked.store(false);
  preparationGate.store(true);
  require(source->Begin(44100), "cannot begin full-callback readiness preview");
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!preparationBlocked.load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const auto startup = source->GetState();
  forbidAllocation = true;
  const auto scheduledPending = source->CheckRender(largeFrames);
  const auto initial = source->Render(small, 64);
  const auto activePending = source->CheckRender(largeFrames);
  const auto pendingRender = source->Render(large, largeFrames);
  forbidAllocation = false;
  const auto frozen = source->GetState();
  preparationGate.store(false); // Release before any assertion can unwind.
  require(unstarted.status == audio::PagedPreviewRenderStatus::Pending,
          "unstarted preflight must be allocation-free and pending");
  require(preparationBlocked.load() && startup.startupReady && !startup.preparationComplete,
          "future scheduled asset must remain unpublished after startup");
  require(scheduledPending.status == audio::PagedPreviewRenderStatus::Pending &&
              activePending.status == audio::PagedPreviewRenderStatus::Pending,
          "large preflight must include future scheduled sounds with active voices too");
  require(initial.status == audio::PagedPreviewRenderStatus::Data && initial.frames == 64 &&
              std::abs(small[0] - 16383.0f * (0.9f / 32768.0f)) < 0.000001f,
          "pending preflight must preserve the first scheduled voice");
  require(pendingRender.status == audio::PagedPreviewRenderStatus::Pending &&
              frozen.positionFrames44100 == 64 &&
              std::all_of(large.begin(), large.end(), [](float value) { return value == 0; }),
          "pending full render must clear output without changing active voice or clock");
  waitPrepared(source);
  const auto prepared = source->GetState();
  forbidAllocation = true;
  const auto beyondEnd = source->CheckRender(UINT32_MAX);
  const auto repeated = source->CheckRender(UINT32_MAX);
  const auto resumed = source->Render(small, 64);
  forbidAllocation = false;
  require(beyondEnd.status == audio::PagedPreviewRenderStatus::Data &&
              beyondEnd.frames == prepared.durationFrames44100 - 64 &&
              repeated.frames == beyondEnd.frames,
          "full-callback readiness must clamp to known EOF without advancing time");
  require(resumed.status == audio::PagedPreviewRenderStatus::Data &&
              std::abs(small[0] - 16383.0f * (0.9f / 32768.0f)) < 0.000001f,
          "failed preflight must preserve active voice phase for exact resume");
  (void)renderAll(source);
  forbidAllocation = true;
  const auto ended = source->CheckRender(UINT32_MAX);
  forbidAllocation = false;
  require(ended.status == audio::PagedPreviewRenderStatus::End && !ended.frames,
          "preflight at known EOF must return End without allocation");
}

void testMissingAndExpired(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 44100, std::vector<short>(100, 8192));
  { std::ofstream invalid(root / "second.wav"); invalid << "invalid audio"; }
  auto source = audio::PagedPreviewSource::Create(*chart(root, "#00001:01\n#00111:02\n#00211:03\n"), {}, 44100);
  require(source->Begin(44100), "cannot start invalid asset preview");
  waitPrepared(source);
  require(source->GetState().error.empty(), "invalid/missing assets must be known empty");
  const auto samples = renderAll(source);
  for (float value : samples) require(value == 0, "expired tail must not replay at highlight origin");
  auto badRate = audio::PagedPreviewSource::Create(*chart(root, ""), {}, 0);
  require(!badRate->Begin(0) && !badRate->GetState().error.empty(),
          "invalid backend rate must report a control-thread error");
}
}
int main() {
  bypassPreparationGate = true;
  const auto root = std::filesystem::temp_directory_path() /
      ("asobmashow-paged-source-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    for (const char *name : {"highlight", "resampling", "missing", "progressive", "timing", "downsampling", "preflight"}) std::filesystem::create_directories(root / name);
    testHighlightAndReuse(root / "highlight");
    testResampling(root / "resampling");
    testDownsamplingWarmsEveryNativePage(root / "downsampling");
    testMissingAndExpired(root / "missing");
    testProgressiveStartup(root / "progressive");
    testFullCallbackPreflightPreservesScheduledAndActiveVoices(root / "preflight");
    testFractionalTiming(root / "timing");
    std::filesystem::remove_all(root);
    std::cout << "Paged preview source tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    forbidAllocation = false;
    preparationGate.store(false);
    std::filesystem::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
