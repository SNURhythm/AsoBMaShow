#include "audio/AudioMix.h"
#include "audio/PagedPcmSound.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
thread_local bool countAllocations = false;
thread_local size_t allocationCount = 0;

void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

void fill(SoundData &sound, int channels, size_t frames, bool paged) {
  std::vector<short> pcm(frames * channels);
  for (size_t sample = 0; sample < pcm.size(); ++sample) {
    pcm[sample] = static_cast<short>(static_cast<int>(sample * 137 % 60001) - 30000);
  }
  if (paged) {
    sound.pagedOutput = std::make_shared<audio::PagedPcmSound>();
    require(sound.pagedOutput->publishMetadata(channels, frames), "publish metadata");
    for (size_t page = 0; page < sound.pagedOutput->metadata()->pageCount; ++page) {
      const size_t begin = page * audio::PagedPcmSound::kPageFrames * channels;
      require(sound.pagedOutput->publishPage(page, std::span(pcm).subspan(
          begin, std::min(pcm.size() - begin,
                          audio::PagedPcmSound::kPageFrames * channels))),
              "publish complete page");
    }
  } else {
    sound.channels = channels;
    sound.outputFrameCount = frames;
    sound.outputData = std::move(pcm);
  }
}

void testPendingInvalidAndPublicationValidation() {
  AudioCallbackState state;
  SoundData pending;
  pending.pagedOutput = std::make_shared<audio::PagedPcmSound>();
  require(audio::playback::InsertScheduledSound(state, {.soundData = &pending}),
          "schedule unresolved sound");
  require(!audio::playback::ScheduledMixReady(state, 0, 48000, 32, 100),
          "pending metadata stalls due sound");
  require(state.scheduledSoundCount == 1 && state.playingSoundCount == 0,
          "readiness preserves schedule");
  require(!pending.pagedOutput->publishMetadata(0, 10), "reject nonempty zero channels");
  require(!pending.pagedOutput->publishMetadata(1,
              static_cast<size_t>(std::numeric_limits<std::uint32_t>::max()) + 1),
          "reject frames beyond Q32 representation before allocation");
  require(pending.pagedOutput->publishMetadata(0, 0), "resolve invalid sound");
  require(!pending.pagedOutput->publishMetadata(1, 1), "metadata immutable");
  require(audio::playback::ScheduledMixReady(state, 0, 48000, 32, 100),
          "known invalid sound does not stall");
  audio::playback::ActivateScheduledSounds(state, 0, 48000, 32, 100);
  require(state.scheduledSoundCount == 0 && state.playingSoundCount == 0,
          "invalid sound consumed without voice");
}

void testInterpolationEndpointAndResume() {
  AudioCallbackState state;
  SoundData sound;
  sound.pagedOutput = std::make_shared<audio::PagedPcmSound>();
  require(sound.pagedOutput->publishMetadata(1, 4098), "publish boundary metadata");
  std::vector<short> first(4096, 16384);
  require(!sound.pagedOutput->publishPage(0, std::span(first).first(4095)),
          "reject short page");
  require(sound.pagedOutput->publishPage(0, first), "publish first page");
  require(!sound.pagedOutput->publishPage(0, first), "published page immutable");
  require(audio::playback::AppendActiveSound(state, &sound, audio::Bus::Bgm, 0, 4095),
          "activate paged sound using metadata");
  const auto position = state.playingSounds[0].sourceFrameQ32;
  require(!audio::playback::ScheduledMixReady(state, 0, 48000, 1, 50),
          "right interpolation endpoint in missing page stalls");
  require(state.playingSounds[0].sourceFrameQ32 == position && sound.playing,
          "underflow leaves active voice unchanged");
  const std::array<short, 2> tail{0, -16384};
  require(sound.pagedOutput->publishPage(1, tail), "publish final partial page");
  std::array<float, 4> mixed{};
  allocationCount = 0;
  countAllocations = true;
  const bool ready = audio::playback::ScheduledMixReady(state, 0, 48000, 2, 50);
  audio::playback::ActivateScheduledSounds(state, 0, 48000, 2, 50);
  audio::playback::MixActiveSounds(state, mixed, 2, 2, 1, 1, 50);
  countAllocations = false;
  require(ready && allocationCount == 0, "ready activation and mixing allocate nothing");
  require(mixed[0] == 0.45F && mixed[1] == 0.45F &&
              mixed[2] == 0.225F && mixed[3] == 0.225F,
          "resume uses exact boundary interpolation");
}

void testScheduledOffsetsAndExactContiguousEquivalence() {
  for (int channels : {1, 2}) {
    for (int rate : {0, 50, 75, 100, 137, 200}) {
      for (bool loop : {false, true}) {
        SoundData contiguous, paged;
        fill(contiguous, channels, 8201, false);
        fill(paged, channels, 8201, true);
        AudioCallbackState expected, actual;
        for (auto pair : {std::pair{&expected, &contiguous}, std::pair{&actual, &paged}}) {
          require(audio::playback::AppendActiveSound(*pair.first, pair.second,
              audio::Bus::Bgm, 3, 4090, 0.7F, loop), "append overlap tail");
          require(audio::playback::InsertScheduledSound(*pair.first,
              {.soundData = pair.second, .bus = audio::Bus::Keysound,
               .startMicros = 250, .sequence = 1, .startFrame = 8195,
               .gain = 0.4F, .loop = loop}), "schedule boundary voice");
          require(audio::playback::InsertScheduledSound(*pair.first,
              {.soundData = pair.second, .startMicros = 750, .sequence = 2,
               .startFrame = 1, .gain = 0.2F, .loop = loop}), "schedule offset voice");
        }
        for (int block = 0; block < 200; ++block) {
          for (std::uint32_t offset : {0U, 31U}) {
            std::array<float, 62> want{}, got{};
            allocationCount = 0;
            countAllocations = true;
            const bool ready = audio::playback::ScheduledMixReady(
                actual, block * 1292, 48000, 31, rate, offset);
            audio::playback::ActivateScheduledSounds(actual, block * 1292, 48000,
                                                       31, rate, offset);
            audio::playback::MixActiveSounds(actual, got, 31, 2, 0.8F, 0.6F, rate);
            countAllocations = false;
            require(ready && allocationCount == 0, "paged callback ready and allocation-free");
            audio::playback::ActivateScheduledSounds(expected, block * 1292, 48000,
                                                       31, rate, offset);
            audio::playback::MixActiveSounds(expected, want, 31, 2, 0.8F, 0.6F, rate);
            require(got == want, "paged and contiguous float output exactly equal");
            require(actual.playingSoundCount == expected.playingSoundCount &&
                        actual.scheduledSoundCount == expected.scheduledSoundCount,
                    "paged and contiguous voice lifecycle equal");
            for (size_t voice = 0; voice < actual.playingSoundCount; ++voice) {
              require(actual.playingSounds[voice].sourceFrameQ32 ==
                          expected.playingSounds[voice].sourceFrameQ32,
                      "paged and contiguous cursor equal");
            }
          }
        }
      }
    }
  }
}

void testFutureScheduleAndLoopReadiness() {
  SoundData sound;
  sound.pagedOutput = std::make_shared<audio::PagedPcmSound>();
  AudioCallbackState state;
  require(audio::playback::InsertScheduledSound(state,
      {.soundData = &sound, .startMicros = 1000}), "future schedule");
  require(audio::playback::ScheduledMixReady(state, 0, 1000, 1, 100),
          "future pending asset is not required");
  require(!audio::playback::ScheduledMixReady(state, 0, 1000, 2, 100),
          "due pending asset is required");
  require(sound.pagedOutput->publishMetadata(1, 4100), "loop metadata");
  const std::array<short, 4> tail{1, 2, 3, 4};
  require(sound.pagedOutput->publishPage(1, tail), "loop tail page");
  state.scheduledSounds[0].startFrame = 4099;
  state.scheduledSounds[0].loop = true;
  require(!audio::playback::ScheduledMixReady(state, 1000, 1000, 2, 100),
          "loop wrap requires first page");
  require(audio::playback::ScheduledMixReady(state, 1000, 1000, 1, 100),
          "last frame clamps interpolation endpoint");
  std::vector<short> prefix(4096, 10);
  require(sound.pagedOutput->publishPage(0, prefix), "loop prefix page");
  require(audio::playback::ScheduledMixReady(state, 1000, 1000, 2, 100),
          "loop resumes when wrapped page published");
  require(state.scheduledSoundCount == 1 && !sound.playing,
          "successful readiness is read-only too");
}
void testSparsePagesOnlyRequireActualInterpolationEndpoints() {
  SoundData sound;
  sound.pagedOutput = std::make_shared<audio::PagedPcmSound>();
  require(sound.pagedOutput->publishMetadata(1, 16384), "sparse metadata");
  std::vector<short> page(4096, 100);
  require(sound.pagedOutput->publishPage(0, page), "sparse first page");
  require(sound.pagedOutput->publishPage(2, page), "sparse third page");
  AudioCallbackState state;
  require(audio::playback::AppendActiveSound(state, &sound, audio::Bus::Bgm, 0),
          "sparse active sound");
  require(audio::playback::ScheduledMixReady(state, 0, 48000, 4095, 100),
          "page after last interpolation endpoint not required");
  require(!audio::playback::ScheduledMixReady(state, 0, 48000, 4096, 100),
          "block's final interpolation endpoint requires next page");
  require(audio::playback::ScheduledMixReady(state, 0, 48000, 2, 819200),
          "enormous rate skips unpublished intermediate page");
  state.playingSounds[0].sourceFrameQ32 = std::uint64_t{8192} << 32;
  state.playingSounds[0].loop = true;
  require(!audio::playback::ScheduledMixReady(state, 0, 48000, 8193, 100),
          "multi-page loop run discovers missing fourth page");
}

void testCapacityDropsAndCallbackOffsetsMatchActivation() {
  SoundData pending, complete;
  pending.pagedOutput = std::make_shared<audio::PagedPcmSound>();
  fill(complete, 1, 4, false);
  AudioCallbackState state;
  for (size_t index = 0; index < kMaxActiveSounds; ++index) {
    require(audio::playback::AppendActiveSound(state, &complete, audio::Bus::Bgm, 0),
            "fill voice capacity");
  }
  require(audio::playback::InsertScheduledSound(state, {.soundData = &pending}),
          "schedule over capacity");
  require(audio::playback::ScheduledMixReady(state, 0, 48000, 16, 100),
          "activation drops pending voice beyond capacity without requiring it");
  audio::playback::ClearCallbackSounds(state);
  require(audio::playback::InsertScheduledSound(state,
      {.soundData = &pending, .startMicros = 2000}), "schedule callback chunk event");
  require(audio::playback::ScheduledMixReady(state, 0, 1000, 1, 100, 1),
          "event at callback chunk end stays scheduled");
  require(!audio::playback::ScheduledMixReady(state, 0, 1000, 1, 100, 2),
          "offset callback chunk uses common rounding origin");
  require(!audio::playback::ScheduledMixReady(state, 0, 1000, 1, 100,
              std::numeric_limits<std::uint32_t>::max()),
          "overflowing chunk boundary rejected without mutation");
}

void testProducerPublicationMakesImmutablePcmVisible() {
  audio::PagedPcmSound sound;
  std::atomic_bool producerSucceeded{true};
  std::thread producer([&] {
    if (!sound.publishMetadata(2, 8193)) producerSucceeded.store(false);
    std::vector<short> page(8192, 12345);
    if (!sound.publishPage(1, page)) producerSucceeded.store(false);
    std::fill(page.begin(), page.end(), -23456);
    if (!sound.publishPage(0, page)) producerSucceeded.store(false);
    const std::array<short, 2> tail{111, -222};
    if (!sound.publishPage(2, tail)) producerSucceeded.store(false);
  });
  const audio::PagedPcmSound::Metadata *metadata = nullptr;
  while ((metadata = sound.metadata()) == nullptr) std::this_thread::yield();
  bool valid = metadata->channels == 2 && metadata->frameCount == 8193;
  for (size_t index = 0; index < 3; ++index) {
    const short *page = nullptr;
    while ((page = metadata->pageSamples(index)) == nullptr) std::this_thread::yield();
    if (index < 2) {
      const short expected = index == 0 ? -23456 : 12345;
      valid = valid && std::all_of(page, page + 8192,
                                  [=](short sample) { return sample == expected; });
    } else {
      valid = valid && page[0] == 111 && page[1] == -222;
    }
  }
  producer.join();
  require(valid && producerSucceeded.load(),
          "acquired pages expose complete PCM while producer fills other slots");
}

} // namespace

void *operator new(std::size_t size) {
  if (countAllocations) ++allocationCount;
  if (void *memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }

int main() {
  try {
    testPendingInvalidAndPublicationValidation();
    testInterpolationEndpointAndResume();
    testScheduledOffsetsAndExactContiguousEquivalence();
    testFutureScheduleAndLoopReadiness();
    testSparsePagesOnlyRequireActualInterpolationEndpoints();
    testCapacityDropsAndCallbackOffsetsMatchActivation();
    testProducerPublicationMakesImmutablePcmVisible();
    std::cout << "Paged PCM mixer tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
