#include "audio/PagedPreviewSource.h"
#include "audio/ChartPreviewHighlight.h"
#include "audio/AudioDurationBound.h"
#include "audio/AudioMix.h"
#include "audio/DecodedAssetCache.h"
#include "audio/SoundFileIO.h"
#include "audio/decoder.h"
#include "ArchiveFile.h"
#include "ArchiveRAII.h"
#include "bms_parser.hpp"
#include "utils/ParallelWork.h"

#include <archive_entry.h>
#include <array>
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
#include <tuple>
#include <vector>

namespace {
thread_local bool forbidAllocation = false;
thread_local bool bypassPreparationGate = false;
thread_local std::atomic_bool *cancelResamplingOnAllocation = nullptr;
thread_local audio::DecodedAssetCache *clearCacheOnPcmAllocation = nullptr;
thread_local std::size_t clearCacheAllocationBytes = 0;
std::atomic_bool preparationGate{false};
std::atomic_size_t preparationAllocationBytes{0};
std::atomic_bool preparationBlocked{false};
std::atomic_uint blockedPreparations{0};
std::atomic_bool rejectLargeAllocations{false};
std::atomic_uint largeAllocationAttempts{0};
}
void *operator new(std::size_t bytes) {
  if (forbidAllocation) throw std::bad_alloc();
  if (clearCacheOnPcmAllocation && bytes == clearCacheAllocationBytes) {
    auto *cache = clearCacheOnPcmAllocation;
    clearCacheOnPcmAllocation = nullptr;
    cache->Clear();
  }
  if (cancelResamplingOnAllocation) {
    cancelResamplingOnAllocation->store(true);
    cancelResamplingOnAllocation = nullptr;
  }
  if (bytes > 256U * 1024U * 1024U && rejectLargeAllocations.load()) {
    ++largeAllocationAttempts;
    throw std::bad_alloc();
  }
  // Pause the real worker's first large hidden-keysound read. The short scheduled
  // sound fits below this threshold, so startup must publish before this gate.
  if (!bypassPreparationGate && bytes >= 16384 && preparationGate.load() &&
      (!preparationAllocationBytes.load() || bytes == preparationAllocationBytes.load())) {
    preparationBlocked.store(true);
    ++blockedPreparations;
    while (preparationGate.load()) std::this_thread::yield();
    --blockedPreparations;
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
void testCacheClearDuringDecode(const std::filesystem::path &root) {
  constexpr std::size_t frames = 10000;
  const auto path = root / "cleared.wav";
  writeWav(path, 44100, std::vector<short>(frames, 1000));
  audio::DecodedAssetCache cache;
  std::atomic_bool cancelled{false};
  // Clear after admission, when the real decoder allocates its PCM. Completion
  // must return valid audio without republishing an entry from before Clear.
  clearCacheAllocationBytes = frames * sizeof(short);
  clearCacheOnPcmAllocation = &cache;
  const auto sound = cache.Load(path, cancelled);
  const bool cleared = clearCacheOnPcmAllocation == nullptr;
  clearCacheOnPcmAllocation = nullptr;
  require(cleared && sound && sound->pcm.size() == frames, "clear during real PCM decoding");
  require(!cache.Contains(path) && cache.GetStatistics().retainedBytes == 0,
          "completion after Clear must not publish a stale or null cache entry");
  require(bool(cache.Load(path, cancelled)) && cache.Contains(path),
          "a fresh load after Clear may populate the cache normally");
}

void testUnusedDefinitionsAreNotPrepared(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 44100, std::vector<short>(100, 1000));
  writeWav(root / "second.wav", 44100, std::vector<short>(44100, 2000));
  writeWav(root / "hidden.wav", 44100, std::vector<short>(200, 3000));
  auto parsed = chart(root, "#WAV03 hidden.wav\n#00011:01\n#00031:03\n");
  auto cache = std::make_shared<audio::DecodedAssetCache>();
  auto source = audio::PagedPreviewSource::Create(*parsed, cache, 0);
  require(source->Begin(44100), "begin required-assets preview");
  waitPrepared(source);
  require(cache->Contains(root / "first.wav") && cache->Contains(root / "hidden.wav"),
          "scheduled and hidden gameplay keysounds must be prepared");
  require(!cache->Contains(root / "second.wav") && cache->GetStatistics().decoderOpens == 2,
          "unused WAV definitions must not be decoded or retained");
}

void testHighlightAndReuse(const std::filesystem::path &root) {
  // At one second, the BGM tail (8192) overlaps a key sound (4096).
  writeWav(root / "first.wav", 22050, std::vector<short>(22050 * 3, 8192));
  writeWav(root / "second.wav", 48000, std::vector<short>(48000, 4096));
  auto parsed = chart(root, "#00001:01\n#00011:0002\n");
  parsed->ReferencedWavTable.emplace(9, "referenced.wav");
  writeWav(root / "referenced.wav", 44100, std::vector<short>(8193, 1200));
  auto cache = std::make_shared<audio::DecodedAssetCache>(
      audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  std::atomic_bool cancelled{false};
  require(bool(cache->Load(root / "first.wav", cancelled)),
          "cannot seed whole gameplay cache");
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
  const auto referenced = cache->Load(root / "referenced.wav", cancelled);
  require(referenced && referenced->pcm.size() == 8193 &&
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
  next->Cancel();
  require(next->GetState().cancelled, "Cancel must be terminal");
}
void testHighlightSelection(const std::filesystem::path &root) {
  std::string body;
  std::string unkeyedBody = "#00001:01\n";
  for (int measure = 0; measure < 60; ++measure) {
    char prefix[16];
    std::snprintf(prefix, sizeof(prefix), "#%03d11:", measure);
    body += std::string(prefix) +
        (measure >= 20 && measure < 28 ? "01010101010101010101010101010101" : "01") + "\n";
    // 03 has no WAV definition: these are placed notes over a single BGM.
    unkeyedBody += std::string(prefix) +
        (measure >= 20 && measure < 28 ? "03030303030303030303030303030303" : "03") + "\n";
  }
  // An isolated pile of BGM triggers must not beat sustained musical activity.
  for (int i = 0; i < 200; ++i) body += "#01001:01\n";
  auto parsed = chart(root, body);
  require(audio::FindChartPreviewHighlightMicros(*parsed) == 40'000'000,
          "highlight must begin on the sustained active section's measure boundary");
  require(audio::FindChartPreviewHighlightMicros(*parsed) ==
              audio::FindChartPreviewHighlightMicros(*parsed),
          "highlight must be deterministic");
  require(audio::FindChartPreviewHighlightMicros(*chart(root, unkeyedBody)) == 40'000'000,
          "placed notes without keysounds must locate the active section over a single BGM");
  auto shortChart = chart(root, "#00011:01\n#00411:01010101\n");
  require(audio::FindChartPreviewHighlightMicros(*shortChart) == 0,
          "short charts preserve the beginning");
  auto sparse = chart(root, "#00001:01\n#05901:01\n");
  require(audio::FindChartPreviewHighlightMicros(*sparse) == 0,
          "long BGM without useful onset structure falls back to beginning");
  sparse->Measures.back()->TimeLines.back()->Timing = 8'000'000'000'000'000LL;
  require(audio::FindChartPreviewHighlightMicros(*sparse) == 0,
          "sparse long charts must not allocate in proportion to duration");
  require(audio::FindChartPreviewHighlightMicros(bms_parser::Chart{}) == 0,
          "empty charts preserve the beginning");
  // Background-only charts still expose musical structure.
  std::string bgmBody = body;
  for (std::size_t pos = 0; (pos = bgmBody.find("11:", pos)) != std::string::npos; pos += 3)
    bgmBody.replace(pos, 3, "01:");
  require(audio::FindChartPreviewHighlightMicros(*chart(root, bgmBody)) == 40'000'000,
          "background sound activity must support highlight detection");

  struct Activity { int notes = 1, bgm = 0; };
  const auto selectActivity = [&](const std::vector<Activity> &measures) {
    std::string body;
    for (std::size_t measure = 0; measure < measures.size(); ++measure) {
      for (bool bgm : {false, true}) {
        const int perSecond = bgm ? measures[measure].bgm : measures[measure].notes;
        if (!perSecond) continue;
        char prefix[16];
        std::snprintf(prefix, sizeof(prefix), "#%03d%s:", int(measure), bgm ? "01" : "11");
        body += prefix;
        for (int i = 0; i < 2 * perSecond; ++i) body += "01";
        body += '\n';
      }
    }
    const auto parsed = chart(root, body);
    const auto selected = audio::FindChartPreviewHighlightMicros(*parsed);
    for (std::size_t i = 0; i < parsed->Measures.size(); ++i)
      if (parsed->Measures[i]->Timing == selected) return i;
    return parsed->Measures.size();
  };
  std::vector<Activity> activity(60);
  for (int i = 20; i < 28; ++i) activity[i] = {.notes = 4};
  for (int i = 40; i < 48; ++i) activity[i] = {.notes = 0, .bgm = 6};
  require(selectActivity(activity) == 40,
          "a stronger BGM section must beat placed notes with the same event weight");
  for (auto &measure : activity) std::swap(measure.notes, measure.bgm);
  require(selectActivity(activity) == 40,
          "moving the same activity between notes and BGM must not move the highlight");

  activity.assign(60, Activity{});
  for (int i = 0; i < 27; ++i) activity[i].notes = 9;
  for (int i = 32; i < 40; ++i) activity[i].notes = 8;
  require(selectActivity(activity) == 32,
          "a sustained entrance beats a denser plateau even just before an integral second");
  for (int i = 0; i < 27; ++i) activity[i].notes = 3;
  for (int i = 32; i < 40; ++i) activity[i].notes = i == 32 ? 32 : 1;
  require(selectActivity(activity) == 9,
          "an isolated two-second burst must not earn the sustained entrance bonus");
}
void testResampling(const std::filesystem::path &root) {
  std::vector<short> native(13001 * 2);
  for (std::size_t i = 0; i < native.size(); ++i) native[i] = short(int((i * 137) % 30000) - 15000);
  writeWav(root / "first.wav", 32000, native, 2);
  for (int outputRate : {44100, 48000}) {
    auto cache = std::make_shared<audio::DecodedAssetCache>();
    auto source = audio::PagedPreviewSource::Create(*chart(root, "#00011:01\n"), cache, 0);
    require(source->Begin(outputRate), "cannot start resampling preview");
    waitPrepared(source);
    const auto actual = renderAll(source);
    auto decoded = native;
    for (auto &value : decoded) value = static_cast<short>(int(value) * 32767 / 32768);
    const auto expected = audio::ResamplePcm(decoded, 2, 32000, outputRate);
    std::atomic_bool cancelled{false};
    const auto cached = cache->Load(root / "first.wav", cancelled);
    auto prepared = cached->FindPreparedOutput(outputRate);
    require(prepared == expected,
            "preview must expose its exact output buffer for gameplay reuse");
    std::weak_ptr<const std::vector<short>> outputOwner = prepared.storage();
    prepared = {};
    require(actual.size() >= expected.size(), "duration must contain all resampled PCM");
    for (std::size_t i = 0; i < expected.size(); ++i)
      require(std::abs(actual[i] - expected[i] * (0.9f / 32768.0f)) < 0.000001f,
              "paged resampling differs at a seek/page/interpolation endpoint");
    require(!outputOwner.expired(), "preview retains the published output buffer");
    source.reset();
    require(outputOwner.expired() && cached->FindPreparedOutput(outputRate).empty(),
            "the native cache must not retain abandoned converted buffers");
  }
}
void testResamplingRejectsOversizedOutputBeforeAllocation(const std::filesystem::path &root) {
  // Small, valid native files can expand far beyond the per-asset PCM budget.
  // The stereo case fits if channels are accidentally omitted from the check.
  for (const auto [frames, channels] : {std::pair{60000, 1}, std::pair{1500, 2}}) {
    writeWav(root / "first.wav", 1, std::vector<short>(frames * channels, 8192), channels);
    auto parsed = chart(root, "#00011:01\n");
    auto cache = std::make_shared<audio::DecodedAssetCache>(
        audio::DecodedAssetCache::kUnlimitedRetainedBytes);
    auto source = audio::PagedPreviewSource::Create(*parsed, cache, 0);
    largeAllocationAttempts.store(0);
    rejectLargeAllocations.store(true);
    require(source->Begin(48000), "cannot begin oversized resampling preview");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    auto state = source->GetState();
    while (state.error.empty() && !state.preparationComplete &&
           std::chrono::steady_clock::now() < deadline) {
      source->WaitForChange(state.revision, 20ms);
      state = source->GetState();
    }
    source->Cancel();
    rejectLargeAllocations.store(false);
    require(cache->GetStatistics().decodedFrames == static_cast<std::uint64_t>(frames),
            "low-rate fixture must decode successfully at its native rate");
    require(largeAllocationAttempts.load() == 0,
            "oversized resampled PCM must be rejected before requesting its allocation");
    require(!state.error.empty() && !state.preparationComplete,
            "oversized resampling must report a preparation error");
  }
}
void testCancellationInterruptsResampling() {
  std::vector<short> native(16384 * 2);
  for (std::size_t i = 0; i < native.size(); ++i) native[i] = static_cast<short>(int(i) - 16384);
  for (const int rate : {32000, 48000, 16000}) {
    std::atomic_bool cancelled{false};
    require(audio::ResamplePcm(native, 2, 32000, rate, &cancelled) ==
                audio::ResamplePcm(native, 2, 32000, rate),
            "cancellable resampling must preserve uncancelled PCM exactly");
    // Cancel during the output allocation, after the entry checkpoint. This
    // deterministically exercises cancellation during conversion without a
    // large fixture or a CPU-speed-dependent deadline.
    cancelResamplingOnAllocation = &cancelled;
    const auto clearHook = makeScopeExit([&] { cancelResamplingOnAllocation = nullptr; });
    const auto output = audio::ResamplePcm(native, 2, 32000, rate, &cancelled);
    require(cancelled && output.empty(),
            "resampling must discard work cancelled during its output allocation");
    forbidAllocation = true;
    const auto alreadyCancelled = audio::ResamplePcm(native, 2, 32000, rate, &cancelled);
    forbidAllocation = false;
    require(alreadyCancelled.empty(), "pre-cancelled resampling must not allocate output");
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
  auto cache = std::make_shared<audio::DecodedAssetCache>(
      audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  auto source = audio::PagedPreviewSource::Create(*parsed, cache, 0);
  require(source->Begin(44100), "cannot begin downsampling preview");
  waitPrepared(source);
  std::atomic_bool cancelled{false};
  for (const auto *name : {"first.wav", "referenced.wav"}) {
    const auto before = cache->GetStatistics();
    const auto native = cache->Load(root / name, cancelled);
    require(native && native->pcm.size() == 8193 && native->info.samplerate == 192000,
            "gameplay cache lookup must contain the complete native asset");
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
void testBoundedAudioDurationMetadata() {
  // A 128 MiB sparse WAV is represented only by its 44-byte header. Reading any
  // payload fails the reader, so this proves byte cost independently of timing.
  constexpr std::uint32_t payload = 128U * 1024U * 1024U;
  std::array<unsigned char, 44> wav{};
  const auto put32 = [&](std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) wav[offset + i] = static_cast<unsigned char>(value >> (8 * i));
  };
  std::memcpy(wav.data(), "RIFF", 4);
  put32(4, payload + 36);
  std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
  put32(16, 16);
  wav[20] = 1; wav[22] = 1;
  put32(24, 44100); put32(28, 88200);
  wav[32] = 2; wav[34] = 16;
  std::memcpy(wav.data() + 36, "data", 4);
  put32(40, payload);
  std::size_t bytesRead = 0;
  const auto reader = [&](std::uint64_t offset, std::span<unsigned char> output) {
    bytesRead += output.size();
    if (offset > wav.size() || output.size() > wav.size() - offset) return false;
    std::copy_n(wav.data() + offset, output.size(), output.data());
    return true;
  };
  const auto bound = audio::duration_bound::InspectAudio(payload + 44ULL, reader);
  require(bound && bound->sampleRate == 44100 && bound->frames == payload / 2 && bytesRead <= 128,
          "large PCM WAV duration must need at most 128 header bytes and no payload reads");
  require(!audio::duration_bound::InspectAudio(44, reader), "truncated WAV cannot claim a proven duration");
  require(!audio::duration_bound::InspectAudio(payload + 100ULL, reader),
          "WAV trailing content beyond RIFF length must not produce an underestimated bound");
  // A deflated ZIP can read the PCM header but cannot seek over a large
  // payload to trailing LIST metadata. Include those bytes in a safe bound.
  put32(4, payload + 36 + 34);
  wav[22] = 2; wav[32] = 4;
  put32(28, 176400);
  bytesRead = 0;
  const auto taggedBound = audio::duration_bound::InspectAudio(payload + 44ULL + 34, reader);
  require(taggedBound && taggedBound->sampleRate == 44100 &&
              taggedBound->frames == (payload + 34 + 3) / 4 &&
              bytesRead <= 128,
          "PCM WAV with unreadable trailing metadata needs a bounded duration, not the 15-second fallback");
  put32(4, payload + 36);
  wav[22] = 1; wav[32] = 3;
  put32(28, 88200);
  require(!audio::duration_bound::InspectAudio(payload + 44ULL, reader),
          "inconsistent WAV block alignment must remain unknown");
  std::array<unsigned char, 42> flac{};
  std::memcpy(flac.data(), "fLaC", 4);
  flac[4] = 0x80; flac[7] = 34;
  const auto packed = (std::uint64_t{44100} << 44) | (std::uint64_t{15} << 36) | 88200;
  for (unsigned i = 0; i < 8; ++i) flac[18 + i] = static_cast<unsigned char>(packed >> (56 - 8 * i));
  const auto flacBound = audio::duration_bound::InspectAudio(std::span<const unsigned char>(flac));
  require(flacBound && flacBound->frames == 88200 && flacBound->sampleRate == 44100,
          "FLAC duration comes directly from STREAMINFO");
  std::fill(flac.begin() + 22, flac.begin() + 26, 0);
  require(!audio::duration_bound::InspectAudio(std::span<const unsigned char>(flac)),
          "zero FLAC total samples means unknown, not silent");
}

void testBoundedLargeVorbisDuration(const std::filesystem::path &root) {
  const auto path = root / "long-probe.ogg";
  SF_INFO info{};
  info.samplerate = 44100; info.channels = 1; info.format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;
  {
    auto file = asobmashow::audio::openSoundFileHandle(path, SFM_WRITE, info);
    require(bool(file), "cannot open long Ogg duration fixture");
    std::array<short, 44100> pcm{};
    std::uint32_t random = 1;
    for (auto &sample : pcm) {
      random = random * 1664525U + 1013904223U;
      sample = static_cast<short>(random >> 16);
    }
    for (unsigned second = 0; second < 16; ++second)
      require(sf_writef_short(file.get(), pcm.data(), pcm.size()) == static_cast<sf_count_t>(pcm.size()),
              "cannot write long Ogg duration fixture");
  }
  std::ifstream input(path, std::ios::binary);
  const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
  require(bytes.size() > 65536, "large Ogg fixture must exercise bounded head/tail probing");
  std::size_t bytesRead = 0;
  auto duration = audio::duration_bound::InspectAudio(bytes.size(),
      [&](std::uint64_t offset, std::span<unsigned char> output) {
        bytesRead += output.size();
        if (offset > bytes.size() || output.size() > bytes.size() - offset) return false;
        std::copy_n(bytes.data() + offset, output.size(), output.data());
        return true;
      });
  require(duration && duration->sampleRate == 44100 && duration->frames >= 16 * 44100 &&
              duration->frames < 17 * 44100 && bytesRead <= 65636,
          "large Ogg duration needs only a fixed head/tail read, without codebooks or intervening pages");
  auto corrupt = bytes;
  corrupt.back() ^= 1;
  require(!audio::duration_bound::InspectAudio(std::span<const unsigned char>(corrupt)),
          "invalid terminal Ogg checksum must remain unknown");
  // The existing whole-file bound still rejects corruption in intermediate
  // pages; its music-player contract must survive sharing the page validator.
  require(audio::duration_bound::InspectVorbis(bytes).has_value(), "complete Ogg validation must still pass");
}

void testZipDurationProbes(const std::filesystem::path &root) {
  writeWav(root / "long-probe.wav", 8000, std::vector<short>(22 * 8000, 8192));
  {
    // Match Giselle's long WAV: an ordinary PCM data chunk followed by LIST.
    const auto path = root / "long-probe.wav";
    const auto riffSize = std::filesystem::file_size(path) + 12 - 8;
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(4);
    for (unsigned i = 0; i < 4; ++i) file.put(static_cast<char>(riffSize >> (8 * i)));
    file.seekp(0, std::ios::end);
    file.write("LIST\x04\0\0\0INFO", 12);
  }
  for (const auto *compression : {"store", "deflate"}) {
    const auto path = root / (std::string("duration-") + compression + ".zip");
    auto writer = makeArchiveWriteHandle();
    require(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK &&
                archive_write_set_format_option(writer.get(), "zip", "compression", compression) == ARCHIVE_OK &&
                archive_write_open_filename(writer.get(), path.string().c_str()) == ARCHIVE_OK,
            "cannot create duration probe ZIP");
    for (const auto *name : {"long-probe.wav", "long-probe.ogg"}) {
      std::ifstream input(root / name, std::ios::binary);
      const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
      UniqueResource<archive_entry, archive_entry_free> entry(archive_entry_new());
      archive_entry_set_pathname(entry.get(), name);
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_perm(entry.get(), 0644);
      archive_entry_set_size(entry.get(), bytes.size());
      require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK &&
                  archive_write_data(writer.get(), bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()),
              "cannot write duration probe ZIP entry");
    }
    require(archive_write_close(writer.get()) == ARCHIVE_OK, "cannot finish duration probe ZIP");
    std::size_t visited = 0;
    std::string error;
    require(archive_file::probeZipEntries(path, {"long-probe.wav", "long-probe.ogg"},
        [&](const auto &entry, auto size, const auto &readAt) {
          const auto bound = audio::duration_bound::InspectAudio(size, readAt);
          if (entry.extension() == ".wav") {
            require(bound && bound->sampleRate == 8000 && bound->frames >= 22 * 8000 && bound->frames <= 22 * 8000 + 6,
                    "both ZIP methods must preserve a known long WAV tail without full inflation");
          } else if (std::string_view(compression) == "store") {
            require(bound && bound->frames >= 16 * 44100,
                    "stored ZIP Ogg must support bounded random tail reads");
          } else {
            require(!bound, "large deflated Ogg must remain unknown rather than inflate an unbounded tail");
          }
          ++visited;
        }, &error), error.c_str());
    require(visited == 2, "ZIP duration probe must visit each requested entry");
    auto parsed = chart(root, "#WAV01 long-probe.wav\n#00011:01\n");
    parsed->Meta.Folder = path;
    auto source = audio::PagedPreviewSource::Create(*parsed, {}, 20 * 44100);
    require(source->Begin(8000, true), "cannot begin long tagged WAV highlight");
    waitPrepared(source);
    std::array<float, 2> output{};
    require(source->Render(output, 1).frames == 1 &&
                std::abs(output[0] - 8191.0f * 0.9f / 32768.0f) < 0.000001f,
            "highlight must retain a long placed WAV with trailing metadata in either ZIP method");
    require(archive_file::probeZipEntries(path, {"long-probe.wav"},
        [&](const auto &, auto, const auto &readAt) {
          std::array<unsigned char, 4096> bytes{};
          if (std::string_view(compression) == "deflate") {
            require(!readAt(65536, std::span(bytes).first(1)), "inflated prefix must stay within 64 KiB");
            require(readAt(0, bytes) && std::memcmp(bytes.data(), "RIFF", 4) == 0,
                    "out-of-budget probe must not invalidate an earlier readable prefix");
          } else {
            for (unsigned i = 0; i < 32; ++i)
              require(readAt(i * bytes.size(), bytes), "stored ZIP budget should allow 128 KiB");
            require(!readAt(0, std::span(bytes).first(1)), "stored ZIP payload read budget must be enforced");
          }
        }, &error), error.c_str());
    bool keepGoing = true;
    require(!archive_file::probeZipEntries(path, {"long-probe.wav", "long-probe.ogg"},
        [&](const auto &, auto, const auto &) { keepGoing = false; }, &error,
        [&] { return keepGoing; }), "ZIP duration batch must observe cancellation");
    const auto replacement = root / "duration-replacement.zip";
    std::filesystem::copy_file(path, replacement, std::filesystem::copy_options::overwrite_existing);
    require(!archive_file::probeZipEntries(path, {"long-probe.wav"},
        [&](const auto &, auto, const auto &) {
          std::filesystem::remove(path);
          std::filesystem::rename(replacement, path);
        }, &error),
        "ZIP duration batch must reject source replacement before publishing its results");
  }
}

void testExpiredSoundDoesNotBlockHighlight(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 8000, std::vector<short>(16000, 8192));
  writeWav(root / "second.wav", 8000, std::vector<short>(64, 16384));
  for (const auto &[container, repeated] : {
           std::pair{std::string("loose"), false}, std::pair{std::string("zip"), false},
           std::pair{std::string("7z"), false}, std::pair{std::string("zip"), true}}) {
    auto parsed = chart(root, std::string("#00001:01\n#01011:02\n") + (repeated ? "#01511:01\n" : ""));
    if (container != "loose") {
      const auto archivePath = root / ("expired." + container);
      auto writer = makeArchiveWriteHandle();
      require((container == "zip" ? archive_write_set_format_zip(writer.get())
                                  : archive_write_set_format_7zip(writer.get())) == ARCHIVE_OK &&
                  archive_write_open_filename(writer.get(), archivePath.string().c_str()) == ARCHIVE_OK,
              "cannot create expired audio archive");
      for (const char *name : {"first.wav", "second.wav"}) {
        std::ifstream input(root / name, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        UniqueResource<archive_entry, archive_entry_free> entry(archive_entry_new());
        archive_entry_set_pathname(entry.get(), name);
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0644);
        archive_entry_set_size(entry.get(), bytes.size());
        require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK &&
                    archive_write_data(writer.get(), bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()),
                "cannot write expired audio archive entry");
      }
      require(archive_write_close(writer.get()) == ARCHIVE_OK, "cannot finish expired audio archive");
      parsed->Meta.Folder = archivePath;
    }
    auto cache = std::make_shared<audio::DecodedAssetCache>(
        audio::DecodedAssetCache::kUnlimitedRetainedBytes);
    auto source = audio::PagedPreviewSource::Create(*parsed, cache, 20 * 44100);
    // ZIP startup must precede even the full encoded read of an expired entry.
    preparationAllocationBytes = container == "zip"
        ? std::filesystem::file_size(root / "first.wav") : 16000 * sizeof(short);
    preparationBlocked = false;
    preparationGate = true;
    const auto release = makeScopeExit([] { preparationGate = false; preparationAllocationBytes = 0; });
    require(source->Begin(8000), "cannot begin bounded-duration highlight");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    auto state = source->GetState();
    while ((!preparationBlocked || !state.startupReady) && std::chrono::steady_clock::now() < deadline) {
      source->WaitForChange(state.revision, 20ms);
      state = source->GetState();
    }
    preparationGate = false;
    require(preparationBlocked && state.startupReady && !state.preparationComplete,
            "expired audio must not block highlight startup on its full PCM decode");
    std::array<float, 2> output{};
    require(source->Render(output, 1).status == audio::PagedPreviewRenderStatus::Data &&
                std::abs(output[0] - 16383.0f * 0.9f / 32768.0f) < 0.000001f,
            "highlight begins with only the currently audible sound");
    waitPrepared(source);
    require(cache->GetStatistics().decodedFrames == 16064 && cache->GetStatistics().decoderOpens == 2 &&
                cache->GetStatistics().archiveReads == 0,
            "expired sounds still warm once for gameplay with no individual archive re-extraction");
    if (repeated) {
      const auto samples = renderAll(source);
      require(samples.size() > 79999 * 2 &&
                  std::abs(samples[79999 * 2] - 8191.0F * 0.9F / 32768.0F) < 0.000001F,
              "excluding expired past instances must preserve the same asset's future onset");
      continue;
    }
    // The same native cache at another device rate must also skip expired
    // resampling, rather than blocking on a redundant 88,200-frame output.
    auto reused = audio::PagedPreviewSource::Create(*parsed, cache, 20 * 44100);
    preparationAllocationBytes = 88200 * sizeof(short);
    preparationBlocked = false;
    preparationGate = true;
    require(reused->Begin(44100), "cannot begin cached highlight at a different rate");
    const auto reuseDeadline = std::chrono::steady_clock::now() + 2s;
    state = reused->GetState();
    while (!preparationBlocked && !state.startupReady && std::chrono::steady_clock::now() < reuseDeadline) {
      reused->WaitForChange(state.revision, 20ms);
      state = reused->GetState();
    }
    preparationGate = false;
    require(!preparationBlocked && state.startupReady,
            "cached expired sounds must not delay startup by resampling discarded audio");
    waitPrepared(reused);
    require(cache->GetStatistics().decoderOpens == 2 && cache->GetStatistics().archiveReads == 0,
            "cached highlight reuses native PCM without decoding or extracting again");
  }
}

void testUnknownDurationLookbackAndKnownLongTails(const std::filesystem::path &root) {
  // AIFF is decodable but intentionally outside the cheap metadata probe.
  for (const bool placed : {false, true})
  for (const auto [format, onset, cached, overlap, repeated] : {
           std::tuple{SF_FORMAT_WAV, 0, false, true, false},
           std::tuple{SF_FORMAT_AIFF, 5, false, false, false},
           std::tuple{SF_FORMAT_AIFF, 6, false, true, false},
           std::tuple{SF_FORMAT_AIFF, 0, true, true, false},
           std::tuple{SF_FORMAT_AIFF, 0, false, true, true}}) {
    SF_INFO info{};
    info.samplerate = 8000; info.channels = 1; info.format = format | SF_FORMAT_PCM_16;
    {
      auto file = asobmashow::audio::openSoundFileHandle(root / "first.wav", SFM_WRITE, info);
      const std::vector<short> pcm(8000 * 22, 8192);
      require(file && sf_write_short(file.get(), pcm.data(), pcm.size()) == static_cast<sf_count_t>(pcm.size()),
              "cannot create lookback audio fixture");
    }
    writeWav(root / "second.wav", 8000, std::vector<short>(64, 16384));
    auto parsed = chart(root, std::string(placed ? "#00011:01\n" : "#00001:01\n") + "#01012:02\n" +
        (repeated ? (placed ? "#00911:0001\n" : "#00901:0001\n") : ""));
    for (auto *measure : parsed->Measures)
      for (auto *timeline : measure->TimeLines)
        if (timeline->Timing == 0) timeline->Timing = onset * 1'000'000LL;
    auto cache = std::make_shared<audio::DecodedAssetCache>();
    std::atomic_bool cancelled = false;
    if (cached) require(bool(cache->Load(root / "first.wav", cancelled)), "cannot warm known duration");
    auto source = audio::PagedPreviewSource::Create(*parsed, cache, 20 * 44100);
    require(source->Begin(8000, true), "cannot begin unknown-duration highlight");
    waitPrepared(source);
    std::array<float, 2> output{};
    require(source->Render(output, 1).status == audio::PagedPreviewRenderStatus::Data,
            "lookback preview must be playable");
    const float expected = (overlap ? 24574.0F : 16383.0F) * 0.9F / 32768.0F;
    require(std::abs(output[0] - expected) < 0.000001F,
            "15-second cap applies only to unknown past sounds; known long tails remain audible");
    require(cache->GetStatistics().decoderOpens == 2,
            "probing or cached duration lookup must not add a PCM decoder pass");
  }
}

void testUnknownOggMinuteLookback(const std::filesystem::path &root) {
  // Use a misleading .wav name, as chart extension fallback often loads Ogg.
  SF_INFO info{};
  info.samplerate = 8000; info.channels = 1; info.format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;
  {
    auto file = asobmashow::audio::openSoundFileHandle(root / "first.wav", SFM_WRITE, info);
    std::vector<short> pcm(62 * 8000);
    std::uint32_t random = 1;
    for (auto &sample : pcm) {
      random = random * 1664525U + 1013904223U;
      sample = static_cast<short>(int(random >> 18) - 8192);
    }
    require(file && sf_write_short(file.get(), pcm.data(), pcm.size()) == static_cast<sf_count_t>(pcm.size()),
            "cannot create unknown Ogg fixture");
  }
  require(std::filesystem::file_size(root / "first.wav") > 65536,
          "Ogg fixture must exceed the ZIP inflated-prefix budget");
  writeWav(root / "second.wav", 8000, std::vector<short>(64, 16384));
  for (const auto *compression : {"store", "deflate"}) {
    const auto archive = root / (std::string("ogg-lookback-") + compression + ".zip");
    auto writer = makeArchiveWriteHandle();
    require(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK &&
                archive_write_set_format_option(writer.get(), "zip", "compression", compression) == ARCHIVE_OK &&
                archive_write_open_filename(writer.get(), archive.string().c_str()) == ARCHIVE_OK,
            "cannot create Ogg lookback ZIP");
    for (const auto *name : {"first.wav", "second.wav"}) {
      std::ifstream input(root / name, std::ios::binary);
      const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
      UniqueResource<archive_entry, archive_entry_free> entry(archive_entry_new());
      archive_entry_set_pathname(entry.get(), name);
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_perm(entry.get(), 0644);
      archive_entry_set_size(entry.get(), bytes.size());
      require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK &&
                  archive_write_data(writer.get(), bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()),
              "cannot write Ogg lookback ZIP entry");
    }
    require(archive_write_close(writer.get()) == ARCHIVE_OK, "cannot finish Ogg lookback ZIP");
    for (const bool placed : {false, true})
    for (const auto [age, cached] : {std::pair{59, false}, std::pair{60, false}, std::pair{61, true}}) {
      const bool repeatsLater = age == 60 && !cached;
      auto parsed = chart(root, std::string(placed ? "#00011:01\n" : "#00001:01\n") + "#03012:02\n" +
          (repeatsLater ? (placed ? "#03101:01\n" : "#03111:01\n") : ""));
      parsed->Meta.Folder = archive;
      for (auto *measure : parsed->Measures)
        for (auto *timeline : measure->TimeLines)
          if (timeline->Timing == 60 * 1'000'000LL) timeline->Timing = age * 1'000'000LL;
          else if (timeline->Timing == 62 * 1'000'000LL) timeline->Timing = (age + 2) * 1'000'000LL;
      auto cache = std::make_shared<audio::DecodedAssetCache>();
      std::atomic_bool cancelled = false;
      if (cached) require(bool(cache->Load(archive / "first.wav", cancelled)), "cannot warm known Ogg duration");
      auto source = audio::PagedPreviewSource::Create(*parsed, cache, age * 44100);
      require(source->Begin(8000, true), "cannot begin unknown Ogg preview");
      waitPrepared(source);
      const auto native = cache->Find(archive / "first.wav");
      const auto current = cache->Find(archive / "second.wav");
      require(native && current, "Ogg fallback must still warm both native assets");
      std::array<float, 16> output{};
      require(source->Render(output, 8).status == audio::PagedPreviewRenderStatus::Data,
              "Ogg lookback preview must be playable");
      const bool overlap = age < 60 || cached || std::string_view(compression) == "store";
      for (std::size_t frame = 0; frame < 8; ++frame) {
        const auto past = overlap ? native->pcm[age * 8000 + frame] : 0;
        const auto expected = (past + current->pcm[frame]) * (0.9f / 32768.0f);
        if (std::abs(output[frame * 2] - expected) >= 0.000001f)
          std::cerr << "Ogg lookback: " << compression << " placed=" << placed << " age=" << age << " cached=" << cached
                    << " expected=" << expected << " actual=" << output[frame * 2] << '\n';
        require(std::abs(output[frame * 2] - expected) < 0.000001f,
                "unknown Ogg needs a 60-second lookback; known longer Ogg must remain audible");
      }
      if (repeatsLater) {
        std::array<float, 1024> discard{};
        for (unsigned skipped = 8; skipped < 16000;) {
          const auto count = std::min(512U, 16000 - skipped);
          require(source->Render(discard, count).frames == count, "cannot reach repeated future Ogg");
          skipped += count;
        }
        require(source->Render(output, 8).frames == 8, "cannot render repeated future Ogg");
        for (std::size_t frame = 0; frame < 8; ++frame)
          require(std::abs(output[frame * 2] - native->pcm[frame] * (0.9f / 32768.0f)) < 0.000001f,
                  "an excluded past Ogg instance must not suppress its future use on the other bus");
      }
      require(cache->GetStatistics().decoderOpens == 2, "Ogg probing must not introduce another decoder pass");
    }
  }
}

void testProgressiveStartup(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 44100, std::vector<short>(100, 16384));
  writeWav(root / "second.wav", 44100, std::vector<short>(44100 * 4, 8192));
  auto source = audio::PagedPreviewSource::Create(*chart(root, "#00011:01\n#00031:02\n"), {}, 0);
  preparationBlocked.store(false);
  preparationGate.store(true);
  require(source->Begin(44100, true), "cannot begin progressive preview");
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while ((!preparationBlocked.load() || !source->GetState().startupReady) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const auto state = source->GetState();
  preparationGate.store(false); // Always release before an assertion can unwind.
  require(preparationBlocked.load(), "hidden keysound warming did not reach the allocation gate");
  require(state.startupReady && !state.preparationComplete,
          "startup must precede full preparation of hidden keysounds");
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
void testContinuousQueueAndEarlyEnd(const std::filesystem::path &root) {
  if (parallel_worker_count(2) < 2) return;
  for (const bool blockOpening : {true, false}) {
    writeWav(root / "first.wav", 44100,
             std::vector<short>(blockOpening ? 44100 * 4 : 100, 16384));
    writeWav(root / "second.wav", 44100,
             std::vector<short>(blockOpening ? 100 : 44100 * 4, 8192));
    auto parsed = chart(root, "#00011:01\n#00031:02\n");
    parsed->Meta.TotalLength = 0;
    parsed->Meta.PlayLength = 0;
    auto cache = std::make_shared<audio::DecodedAssetCache>();
    auto source = audio::PagedPreviewSource::Create(*parsed, cache, 0);
    preparationBlocked = false;
    preparationGate = true;
    const auto releaseGate = makeScopeExit([&] { preparationGate = false; });
    require(source->Begin(44100), "begin continuous queue test");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while ((!preparationBlocked || (blockOpening ? !cache->Contains(root / "second.wav") :
           !source->GetState().startupReady)) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    const auto state = source->GetState();
    bool correct = false;
    if (blockOpening) {
      correct = preparationBlocked && cache->Contains(root / "second.wav") && !state.startupReady;
    } else if (state.startupReady && state.durationKnown) {
      const auto samples = renderAll(source);
      correct = samples.size() == 200 && source->GetState().positionFrames44100 == 100 &&
                !source->GetState().preparationComplete;
    }
    preparationGate = false;
    waitPrepared(source);
    require(correct, blockOpening ? "hidden keysounds keep loading while opening sound is blocked" :
                                  "preview ends at true duration while hidden keysound loading remains blocked");
  }
}

void testAppearanceOrderPrecedesWavTableOrder(const std::filesystem::path &root) {
  writeWav(root / "opening.wav", 44100, std::vector<short>(100, 16384));
  writeWav(root / "large.wav", 44100, std::vector<short>(44100, 8192));
  auto parsed = chart(root, "#WAVZZ opening.wav\n#00011:ZZ\n");
  const auto count = parallel_worker_count(1024) + 2;
  for (unsigned int index = 1; index <= count; ++index) {
    const auto name = "unused-" + std::to_string(index) + ".wav";
    std::filesystem::copy_file(root / "large.wav", root / name);
    parsed->WavTable[index] = name;
    parsed->ReferencedWavTable[index] = name;
  }
  auto source = audio::PagedPreviewSource::Create(*parsed, {}, 0);
  preparationBlocked = false;
  preparationGate = true;
  require(source->Begin(44100), "begin appearance-order test");
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while ((!preparationBlocked || !source->GetState().startupReady) &&
         std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  const auto state = source->GetState();
  preparationGate = false;
  waitPrepared(source);
  require(state.startupReady && !state.preparationComplete,
          "first event must load before earlier WAV IDs can occupy every worker");
}

void testIndependentAssetsPrepareConcurrently(const std::filesystem::path &root) {
  if (parallel_worker_count(2) < 2) return;
  writeWav(root / "first.wav", 44100, std::vector<short>(44100, 1200));
  writeWav(root / "second.wav", 44100, std::vector<short>(44100, 2400));
  for (const auto &body : {"#00101:01\n#00101:02\n", "#00031:0102\n"}) {
    auto parsed = chart(root, body);
    auto source = audio::PagedPreviewSource::Create(*parsed, {}, 0);
    preparationGate = true;
    const bool begun = source->Begin(44100);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (blockedPreparations.load() < 2 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    const bool concurrent = blockedPreparations.load() >= 2;
    preparationGate = false;
    require(begun, "cannot begin concurrent preparation fixture");
    waitPrepared(source);
    source->Cancel();
    require(concurrent,
            "scheduled and hidden keysounds must decode concurrently, even when one decode stalls");
  }
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
  while ((!preparationBlocked.load() || !source->GetState().startupReady) &&
         std::chrono::steady_clock::now() < deadline)
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

void testLeadingSilenceIsSkippedWithoutChangingLaterAudio(const std::filesystem::path &root) {
  std::vector<short> samples(44100 * 3 * 2, 0);
  // Only the right channel sounds; keep a deliberate pause after the opening.
  for (std::size_t frame = 7000; frame < 17000; ++frame) samples[frame * 2 + 1] = 8192;
  for (std::size_t frame = 27000; frame < 44100 * 3; ++frame) samples[frame * 2 + 1] = 4096;
  writeWav(root / "first.wav", 44100, samples, 2);
  writeWav(root / "second.wav", 44100, std::vector<short>(44100, 0));
  auto parsed = chart(root, "#00001:02\n#00201:01\n");
  for (const int rate : {44100, 48000}) {
    for (const std::uint64_t start : {0ULL, 44100ULL * 4 + 3000}) {
      auto reference = audio::PagedPreviewSource::Create(*parsed, {}, start);
      require(reference->Begin(rate), "cannot start untrimmed reference");
      waitPrepared(reference);
      const auto original = renderAll(reference);
      const auto first = std::find_if(original.begin(), original.end(),
                                      [](float sample) { return sample != 0.0f; });
      require(first != original.end(), "silence fixture must contain audible audio");
      const auto skipped = static_cast<std::uint64_t>((first - original.begin()) / 2);
      auto cache = std::make_shared<audio::DecodedAssetCache>(
          audio::DecodedAssetCache::kUnlimitedRetainedBytes);
      auto trimmed = audio::PagedPreviewSource::Create(*parsed, cache, start);
      require(trimmed->Begin(rate, true), "cannot start silence-skipping preview");
      waitPrepared(trimmed);
      const auto ready = trimmed->GetState();
      const auto expectedStart = start + skipped * 44100 / rate;
      require(ready.firstFrame44100 == expectedStart &&
                  ready.positionFrames44100 == expectedStart,
              "preview clock must jump over timeline gaps and silent PCM at startup");
      const auto audible = renderAll(trimmed);
      require(audible.size() == original.size() - skipped * 2,
              "skip only the leading frames, keeping later pauses and the ending");
      require(std::equal(audible.begin(), audible.end(), original.begin() + skipped * 2),
              "trimmed preview must preserve every sample from its first audible frame");
      const auto before = cache->GetStatistics();
      std::atomic_bool cancelled{false};
      const auto native = cache->Load(root / "first.wav", cancelled);
      require(native && native->pcm.size() == samples.size() &&
                  cache->GetStatistics().decodedFrames == before.decodedFrames,
              "skipping preview silence must retain the full native audio for gameplay");
    }
  }
}

void testSilenceDetectionUsesTheMixedAudio(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 44100, std::vector<short>(44100 * 2, 8192));
  writeWav(root / "second.wav", 44100, std::vector<short>(44100, -8192));
  auto source = audio::PagedPreviewSource::Create(
      *chart(root, "#00001:01\n#00011:02\n"), {}, 0);
  require(source->Begin(44100, true), "cannot start cancelling-voices preview");
  waitPrepared(source);
  require(source->GetState().firstFrame44100 == 44100,
          "overlapping voices that cancel to silence must not end the leading scan");
  const auto audible = renderAll(source);
  require(audible.size() == 44100 * 2 && audible.front() != 0.0f,
          "skip the cancelled mix and retain the earlier sound's audible tail");
  auto muted = audio::PagedPreviewSource::Create(
      *chart(root, "#00001:01\n#00111:02\n"), {}, 0, {0.0f, 1.0f});
  require(muted->Begin(44100, true), "cannot start muted-opening preview");
  waitPrepared(muted);
  require(muted->GetState().firstFrame44100 == 44100 * 2,
          "a muted BGM must not delay the first audible keysound");
}

void testSilenceScanNeverDropsAnAudibleVoice(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 44100, std::vector<short>(256, 0));
  writeWav(root / "second.wav", 44100, std::vector<short>(44100, 8192));
  std::string body;
  for (std::size_t i = 0; i < kMaxActiveSounds; ++i) body += "#00001:01\n";
  body += "#00111:02\n";
  auto parsed = chart(root, body);
  for (auto *measure : parsed->Measures)
    for (auto *timeline : measure->TimeLines)
      for (const auto *note : timeline->Notes)
        if (note && note->Wav == 2) timeline->Timing = 11610; // output frame 512
  auto source = audio::PagedPreviewSource::Create(*parsed, {}, 0);
  require(source->Begin(44100, true), "cannot begin dense silence scan");
  waitPrepared(source);
  require(source->GetState().firstFrame44100 == 0,
          "a potentially saturated scan must preserve the original origin");
  std::vector<float> block(512 * 2);
  require(source->Render(block, 512).frames == 512,
          "dense preview must preserve its opening instead of incorrectly ending");
  require(source->Render(block, 512).frames == 512 && block.front() != 0.0f,
          "audible voice after short silent voices must survive preview preparation");
}

void testSparseLeadingSilenceSkipsEmptyTime(const std::filesystem::path &root) {
  constexpr long long distantMicros = 10LL * 365 * 24 * 60 * 60 * 1'000'000;
  constexpr std::uint64_t distantFrames = 10ULL * 365 * 24 * 60 * 60 * 44100;
  writeWav(root / "first.wav", 44100, std::vector<short>(64, 0));
  writeWav(root / "second.wav", 44100, std::vector<short>(64, 8192));
  for (int scenario = 0; scenario < 3; ++scenario) {
    auto parsed = chart(root, scenario == 0 ? "#00011:02\n" :
                              scenario == 1 ? "#00011:01\n#00111:02\n" : "");
    parsed->Meta.TotalLength = distantMicros;
    for (auto *measure : parsed->Measures)
      for (auto *timeline : measure->TimeLines)
        for (const auto *note : timeline->Notes)
          if (note && note->Wav == 2) timeline->Timing = distantMicros;
    auto source = audio::PagedPreviewSource::Create(*parsed, {}, 0);
    require(source->Begin(44100, true), "cannot begin sparse preview");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    auto state = source->GetState();
    while (!state.preparationComplete && state.error.empty() &&
           std::chrono::steady_clock::now() < deadline) {
      source->WaitForChange(state.revision, 20ms);
      state = source->GetState();
    }
    if (!state.preparationComplete) source->Cancel();
    require(state.preparationComplete && state.error.empty(),
            "leading-silence preparation must skip event-free years promptly");
    require(state.firstFrame44100 == distantFrames,
            "sparse preview must seek exactly to the first audible event or silent EOF");
    const auto samples = renderAll(source);
    require(scenario == 2 ? samples.empty() :
                samples.size() == 128 && samples.front() != 0.0f,
            "skipping empty time must preserve the complete distant sound");
  }
}

void testEntirelySilentPreviewReachesEnd(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 44100, std::vector<short>(44100, 0));
  auto source = audio::PagedPreviewSource::Create(*chart(root, "#00101:01\n"), {}, 0);
  require(source->Begin(44100, true), "cannot start silent preview");
  waitPrepared(source);
  require(source->GetState().startupReady, "silent preview must finish startup");
  require(renderAll(source).empty(), "an entirely silent preview must reach EOF without playing silence");
}

void testMissingAndExpired(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 44100, std::vector<short>(100, 8192));
  { std::ofstream invalid(root / "second.wav"); invalid << "invalid audio"; }
  auto parsed = chart(root, "#00001:01\n#00111:02\n#00211:03\n");
  parsed->ReferencedWavTable.emplace(20, "missing.zip/tone.wav");
  auto source = audio::PagedPreviewSource::Create(*parsed, {}, 44100);
  require(source->Begin(44100), "cannot start invalid asset preview");
  waitPrepared(source);
  require(source->GetState().error.empty(), "invalid/missing assets must be known empty");
  const auto samples = renderAll(source);
  for (float value : samples) require(value == 0, "expired tail must not replay at highlight origin");
  auto badRate = audio::PagedPreviewSource::Create(*chart(root, ""), {}, 0);
  require(!badRate->Begin(0) && !badRate->GetState().error.empty(),
          "invalid backend rate must report a control-thread error");
}

void testArchiveEntryLimitsBeforeExtraction(const std::filesystem::path &root) {
  writeWav(root / "tone.wav", 44100, std::vector<short>(64, 8192));
  std::ifstream input(root / "tone.wav", std::ios::binary);
  const std::vector<char> tone((std::istreambuf_iterator<char>(input)), {});
  // Stream zero padding into a compressed ZIP without retaining the expanded
  // payload. One entry exercises streaming; four enable concurrent extraction.
  // A valid 33 MiB entry must still load through the bounded fallback when it
  // exceeds the concurrent stage's 32 MiB scheduling reservation.
  for (const auto [count, megabytes] : {std::pair{1, 257}, std::pair{4, 257},
                                       std::pair{4, 33}}) {
    const auto archivePath = root / (std::to_string(count) + "-" +
        std::to_string(megabytes) + ".zip");
    auto writer = makeArchiveWriteHandle();
    require(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK &&
                archive_write_set_options(writer.get(), "zip:compression=deflate") == ARCHIVE_OK &&
                archive_write_open_filename(writer.get(), archivePath.string().c_str()) == ARCHIVE_OK,
            "cannot create bounded archive fixture");
    auto parsed = chart(root, "#00011:01\n");
    parsed->Meta.Folder = archivePath;
    parsed->WavTable.clear();
    parsed->ReferencedWavTable.clear();
    const std::array<char, 64 * 1024> zeros{};
    for (int index = 1; index <= count; ++index) {
      const auto name = "sound-" + std::to_string(index) + ".wav";
      parsed->WavTable.emplace(index, name);
      parsed->ReferencedWavTable.emplace(index, name);
      const auto bytes = index == 1 ? std::size_t(megabytes) * 1024 * 1024 : tone.size();
      UniqueResource<archive_entry, archive_entry_free> entry(archive_entry_new());
      archive_entry_set_pathname(entry.get(), name.c_str());
      archive_entry_set_filetype(entry.get(), AE_IFREG);
      archive_entry_set_perm(entry.get(), 0644);
      archive_entry_set_size(entry.get(), bytes);
      require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK &&
                  archive_write_data(writer.get(), tone.data(), tone.size()) ==
                      static_cast<la_ssize_t>(tone.size()), "cannot write bounded archive entry");
      for (auto remaining = bytes - tone.size(); remaining > 0;) {
        const auto chunk = std::min(remaining, zeros.size());
        require(archive_write_data(writer.get(), zeros.data(), chunk) ==
                    static_cast<la_ssize_t>(chunk), "cannot pad archive entry");
        remaining -= chunk;
      }
    }
    require(archive_write_close(writer.get()) == ARCHIVE_OK, "cannot finish bounded archive fixture");
    auto cache = std::make_shared<audio::DecodedAssetCache>();
    auto source = audio::PagedPreviewSource::Create(*parsed, cache, 0);
    largeAllocationAttempts = 0;
    rejectLargeAllocations = true;
    require(source->Begin(44100), "cannot begin bounded archive preview");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    auto state = source->GetState();
    while (state.error.empty() && !state.preparationComplete &&
           std::chrono::steady_clock::now() < deadline) {
      source->WaitForChange(state.revision, 20ms);
      state = source->GetState();
    }
    source->Cancel();
    rejectLargeAllocations = false;
    require(largeAllocationAttempts == 0,
            "preview archive entries must be bounded before extraction allocates their payload");
    if (megabytes > 256) {
      require(!state.error.empty() && !state.preparationComplete,
              "oversized encoded archive entries must fail preparation");
    } else {
      require(state.error.empty() && state.preparationComplete &&
                  cache->GetStatistics().decodedFrames == 64U * count,
              "valid entries above the scheduling budget must still decode through bounded streaming");
    }
  }
}

void testArchivedPreviewBatchesOpeningFutureAndReferencedAssets(const std::filesystem::path &root) {
  writeWav(root / "tone.wav", 44100, std::vector<short>(22050, 8192));
  std::ifstream input(root / "tone.wav", std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
  const auto archivePath = root / "sounds.7z";
  auto writer = makeArchiveWriteHandle();
  require(archive_write_set_format_7zip(writer.get()) == ARCHIVE_OK &&
              archive_write_open_filename(writer.get(), archivePath.string().c_str()) == ARCHIVE_OK,
          "cannot create archived preview fixture");
  std::string body;
  constexpr char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for (int index = 1; index <= 16; ++index) {
    const std::string name = "sound-" + std::to_string(index) + ".wav";
    UniqueResource<archive_entry, archive_entry_free> entry(archive_entry_new());
    archive_entry_set_pathname(entry.get(), name.c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), bytes.size());
    require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK &&
                archive_write_data(writer.get(), bytes.data(), bytes.size()) ==
                    static_cast<la_ssize_t>(bytes.size()), "cannot write archived preview asset");
    const std::string wav{'0', digits[index]};
    if (index <= 14) {
      body += "#WAV" + wav + " " + name + "\n";
      body += std::string(index <= 12 ? "#00001:" : "#00201:") + wav + "\n";
    }
  }
  require(archive_write_close(writer.get()) == ARCHIVE_OK, "cannot finish archived preview fixture");
  auto parsed = chart(root, body);
  parsed->Meta.Folder = archivePath;
  parsed->ReferencedWavTable.emplace(15, "sound-15.wav");
  parsed->ReferencedWavTable.emplace(16, "sound-16.wav");
  auto cache = std::make_shared<audio::DecodedAssetCache>(
      audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  std::atomic_uint archiveChecks{0};
  archive_file::setCachePathNormalizer([&](const std::filesystem::path &path) {
    ++archiveChecks;
    return fspath_to_utf8(path);
  });
  const auto resetNormalizer = makeScopeExit([] { archive_file::setCachePathNormalizer({}); });
  auto source = audio::PagedPreviewSource::Create(*parsed, cache, 0);
  require(source->Begin(44100), "cannot start archived preview");
  waitPrepared(source);
  require(cache->GetStatistics().decodedFrames == 16 * 22050,
          "archive batching must warm opening, future and referenced native PCM");
  require(cache->GetStatistics().archiveReads == 0,
          "preview preparation must batch archive entries instead of replaying one extraction per asset");
  std::cerr << "Archive preview metadata checks: " << archiveChecks.load() << '\n';
  require(archiveChecks.load() <= 64,
          "preview must validate archive metadata per batch, not repeatedly per keysound");
  const auto samples = renderAll(source);
  require(!samples.empty() && std::abs(samples.front() - 12 * 8191.0f * 0.9f / 32768) < 0.00001f,
          "archive loading must preserve overlapping preview PCM");
}

void testOverlappingCompressedAssetsDecodeOnce(const std::filesystem::path &root) {
  const auto encoded = root / "tone.ogg";
  SF_INFO written{};
  written.samplerate = 44100;
  written.channels = 1;
  written.format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;
  {
    auto file = asobmashow::audio::openSoundFileHandle(encoded, SFM_WRITE, written);
    require(bool(file), "cannot create overlapping compressed fixture");
    std::vector<short> samples(44100 * 4);
    for (std::size_t index = 0; index < samples.size(); ++index)
      samples[index] = static_cast<short>(int(index % 200) * 10 - 1000);
    require(sf_writef_short(file.get(), samples.data(), samples.size()) ==
                static_cast<sf_count_t>(samples.size()), "cannot write overlapping compressed fixture");
  }
  std::atomic_bool cancelled{false};
  SF_INFO info{};
  std::vector<short> expected;
  require(decodeAudioToPCM(fspath_to_path_t(encoded), expected, info, cancelled),
          "cannot decode overlapping compressed reference");
  constexpr char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for (const int count : {16, 17, 32}) {
    const auto directory = root / std::to_string(count);
    std::filesystem::create_directories(directory);
    std::string body;
    std::vector<std::filesystem::path> paths;
    for (int index = 1; index <= count; ++index) {
      const auto name = "sound-" + std::to_string(index) + ".ogg";
      paths.push_back(directory / name);
      std::filesystem::copy_file(encoded, paths.back());
      const std::string wav{'0', digits[index]};
      body += "#WAV" + wav + " " + name + "\n#00001:" + wav + "\n";
    }
    auto cache = std::make_shared<audio::DecodedAssetCache>(
        audio::DecodedAssetCache::kUnlimitedRetainedBytes);
    auto source = audio::PagedPreviewSource::Create(*chart(directory, body), cache, 0);
    require(source->Begin(44100), "cannot start overlapping compressed preview");
    waitPrepared(source);
    const auto state = cache->GetStatistics();
    require(state.decoderOpens == static_cast<std::uint64_t>(count),
            "each actively warmed compressed asset must open once across every preview window");
    require(state.decodedFrames == expected.size() * count,
            "overlapping preview must retain every native frame exactly once");
    require(state.liveDecoders == 0,
            "completed preview preparation must retain no live decoders");
    for (const auto &path : paths) {
      const auto actual = cache->Load(path, cancelled);
      require(actual && actual->pcm == expected,
              "retained compressed PCM must match the contiguous decoder exactly");
    }
    const auto actual = renderAll(source);
    require(actual.size() >= expected.size() * 2, "overlap render must contain every source frame");
    for (std::size_t frame = 0; frame < expected.size(); ++frame) {
      float mixed = 0;
      for (int voice = 0; voice < count; ++voice) mixed += expected[frame] * (0.9f / 32768.0f);
      require(std::abs(actual[frame * 2] - mixed) < 0.000001f &&
                  actual[frame * 2] == actual[frame * 2 + 1],
              "decoder retention must preserve overlapping preview PCM");
    }
  }
}

void testOverlappingWavAssetsRespectDescriptorCeiling(const std::filesystem::path &root) {
  constexpr int count = 96;
  constexpr int frames = 44100 * 4;
  constexpr char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::string body;
  std::vector<std::filesystem::path> paths;
  for (int index = 1; index <= count; ++index) {
    const auto name = "sound-" + std::to_string(index) + ".wav";
    paths.push_back(root / name);
    writeWav(paths.back(), 44100, std::vector<short>(frames, 1024));
    const std::string wav{digits[index / 36], digits[index % 36]};
    body += "#WAV" + wav + " " + name + "\n#00001:" + wav + "\n";
  }
  auto cache = std::make_shared<audio::DecodedAssetCache>(
      audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  auto source = audio::PagedPreviewSource::Create(*chart(root, body), cache, 0);
  require(source->Begin(44100), "cannot start descriptor-pressure preview");
  waitPrepared(source);
  const auto prepared = cache->GetStatistics();
  require(prepared.peakLiveDecoders <= parallel_worker_count(count),
          "full decoding must remain bounded by worker count");
  require(prepared.liveDecoders == 0,
          "descriptor-pressure preview must release every decoder");
  require(prepared.decodedFrames == std::uint64_t(frames) * count &&
              prepared.retainedBytes == std::size_t(frames) * count * sizeof(short),
          "descriptor-pressure preview must still preload and retain every native PCM frame");
  std::atomic_bool cancelled{false};
  const std::vector<short> expected(frames, 1023);
  for (const auto &path : paths) {
    const auto sound = cache->Load(path, cancelled);
    require(sound && sound->pcm == expected,
            "descriptor-pressure gameplay must retain exact PCM");
  }
  require(cache->GetStatistics().decoderOpens == prepared.decoderOpens,
          "preloaded gameplay must not reopen pressure-evicted decoders");
  const auto actual = renderAll(source);
  float mixed = 0;
  for (int voice = 0; voice < count; ++voice) mixed += 1023 * (0.9f / 32768.0f);
  require(actual.size() == frames * 2, "descriptor-pressure preview must render every source frame");
  for (const float sample : actual)
    require(std::abs(sample - mixed) < 0.000001f,
            "descriptor eviction must preserve the complete overlapping preview mix");
}

void testPreparationExitClosesDecoders(const std::filesystem::path &root) {
  for (const bool fail : {false, true}) {
    writeWav(root / "first.wav", 44100, std::vector<short>(100, 16384));
    writeWav(root / "second.wav", 44100, std::vector<short>(44100 * 4, 8192));
    auto cache = std::make_shared<audio::DecodedAssetCache>(
        audio::DecodedAssetCache::kUnlimitedRetainedBytes);
    auto source = audio::PagedPreviewSource::Create(*chart(root, "#00011:01\n#00031:02\n"), cache, 0);
    preparationBlocked.store(false);
    preparationGate.store(true);
    require(source->Begin(44100, true), "cannot begin decoder cleanup preview");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while ((!preparationBlocked.load() || !source->GetState().startupReady) &&
         std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    if (!preparationBlocked.load()) {
      preparationGate.store(false);
      require(false, "decoder cleanup preview did not reach preparation gate");
    }
    const auto active = cache->GetStatistics().liveDecoders;
    if (fail) {
      std::filesystem::remove(root / "second.wav");
      preparationGate.store(false);
      while (source->GetState().error.empty() && std::chrono::steady_clock::now() < deadline)
        source->WaitForChange(source->GetState().revision, 20ms);
      require(!source->GetState().error.empty(), "source removal during preparation must report failure");
      source->Cancel();
    } else {
      // Keep the producer gated until Cancel has requested termination, then
      // allow its normal checkpoint to unwind and release worker-owned decoders.
      std::jthread release([&] {
        while (!source->GetState().cancelled) std::this_thread::yield();
        preparationGate.store(false);
      });
      source->Cancel();
    }
    require(active != 0 && cache->GetStatistics().liveDecoders == 0,
            "cancellation and operational failure must release all active decoders");
    require(cache->Contains(root / "first.wav"),
            "preparation exit must retain already prepared native PCM");
  }
}

void testPreparedRepeatedVoiceDoesNotRetainDecoder(const std::filesystem::path &root) {
  writeWav(root / "first.wav", 44100, std::vector<short>(100, 16384));
  writeWav(root / "second.wav", 44100, std::vector<short>(44100 * 4, 8192));
  auto parsed = chart(root, "#00011:0101\n#00111:02\n");
  for (auto *measure : parsed->Measures)
    for (auto *timeline : measure->TimeLines)
      for (const auto *note : timeline->Notes)
        if (note && note->Wav == 1 && timeline->Timing > 0)
          timeline->Timing = 1999000;
  auto cache = std::make_shared<audio::DecodedAssetCache>(
      audio::DecodedAssetCache::kUnlimitedRetainedBytes);
  auto source = audio::PagedPreviewSource::Create(*parsed, cache, 0);
  preparationBlocked.store(false);
  preparationGate.store(true);
  require(source->Begin(44100), "cannot begin repeated voice decoder preview");
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while ((!preparationBlocked.load() || !source->GetState().startupReady) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const auto startup = source->GetState();
  const auto retained = cache->GetStatistics();
  preparationGate.store(false);
  require(preparationBlocked.load() && startup.startupReady && !startup.preparationComplete,
          "later large source must remain gated after startup");
  // The second first.wav voice extends past the two-second startup window,
  // but all of that source's output is already ready. Only second.wav is warming.
  require(retained.liveDecoders == 1 && retained.decodedFrames == 100,
          "fully prepared repeated voices must close their decoder while later audio is warming");
  waitPrepared(source);
  require(cache->GetStatistics().liveDecoders == 0,
          "repeated voice preparation must finish with no live decoders");
}
}
int main() {
  bypassPreparationGate = true;
  const auto root = std::filesystem::temp_directory_path() /
      ("asobmashow-paged-source-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    for (const char *name : {"highlight", "resampling", "missing", "progressive", "timing", "downsampling", "preflight", "archive", "overlap", "pressure", "pin-exit", "repeated-pin", "parallel", "trim", "silent", "mixed-trim", "dense-trim"}) std::filesystem::create_directories(root / name);
    testCacheClearDuringDecode(root / "highlight");
    testUnusedDefinitionsAreNotPrepared(root / "highlight");
    testLeadingSilenceIsSkippedWithoutChangingLaterAudio(root / "trim");
    testSparseLeadingSilenceSkipsEmptyTime(root / "silent");
    testEntirelySilentPreviewReachesEnd(root / "silent");
    testSilenceScanNeverDropsAnAudibleVoice(root / "dense-trim");
    testSilenceDetectionUsesTheMixedAudio(root / "mixed-trim");
    testAppearanceOrderPrecedesWavTableOrder(root / "parallel");
    testContinuousQueueAndEarlyEnd(root / "parallel");
    testIndependentAssetsPrepareConcurrently(root / "parallel");
    testHighlightAndReuse(root / "highlight");
    testResamplingRejectsOversizedOutputBeforeAllocation(root / "resampling");
    testCancellationInterruptsResampling();
    testHighlightSelection(root / "highlight");
    testResampling(root / "resampling");
    testDownsamplingWarmsEveryNativePage(root / "downsampling");
    testMissingAndExpired(root / "missing");
    testBoundedAudioDurationMetadata();
    testBoundedLargeVorbisDuration(root / "progressive");
    testZipDurationProbes(root / "progressive");
    testExpiredSoundDoesNotBlockHighlight(root / "progressive");
    testUnknownDurationLookbackAndKnownLongTails(root / "progressive");
    testUnknownOggMinuteLookback(root / "progressive");
    testProgressiveStartup(root / "progressive");
    testFullCallbackPreflightPreservesScheduledAndActiveVoices(root / "preflight");
    testFractionalTiming(root / "timing");
    testArchiveEntryLimitsBeforeExtraction(root / "archive");
    testArchivedPreviewBatchesOpeningFutureAndReferencedAssets(root / "archive");
    testOverlappingCompressedAssetsDecodeOnce(root / "overlap");
    testOverlappingWavAssetsRespectDescriptorCeiling(root / "pressure");
    testPreparationExitClosesDecoders(root / "pin-exit");
    testPreparedRepeatedVoiceDoesNotRetainDecoder(root / "repeated-pin");
    std::filesystem::remove_all(root);
    std::cout << "Paged preview source tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    forbidAllocation = false;
    rejectLargeAllocations.store(false);
    preparationGate.store(false);
    std::filesystem::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
