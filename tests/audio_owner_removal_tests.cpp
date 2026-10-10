#include "audio/AudioMix.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

// Protect only complete interior pages belonging to the schedule allocation.
// An unrelated owner stop must not read or rewrite chart events. On POSIX a
// regression faults deterministically rather than relying on a timing limit.
class InaccessibleSchedulePages {
public:
  explicit InaccessibleSchedulePages(const AudioCallbackState &state) {
#if defined(__unix__) || defined(__APPLE__)
    const long pageSize = sysconf(_SC_PAGESIZE);
    require(pageSize > 0, "page size is available");
    const auto page = static_cast<std::uintptr_t>(pageSize);
    const auto begin = reinterpret_cast<std::uintptr_t>(state.scheduledSounds.get());
    const auto end = begin + state.scheduledSoundCapacity * sizeof(ScheduledSound);
    const auto first = (begin / page + 1) * page;
    const auto last = end / page * page;
    require(last > first, "schedule contains complete interior pages");
    begin_ = reinterpret_cast<void *>(first);
    bytes_ = last - first;
    require(mprotect(begin_, bytes_, PROT_NONE) == 0, "protect schedule pages");
#else
    (void)state;
#endif
  }
  ~InaccessibleSchedulePages() {
#if defined(__unix__) || defined(__APPLE__)
    if (mprotect(begin_, bytes_, PROT_READ | PROT_WRITE) != 0) std::terminate();
#endif
  }
  InaccessibleSchedulePages(const InaccessibleSchedulePages &) = delete;
  InaccessibleSchedulePages &operator=(const InaccessibleSchedulePages &) = delete;

private:
#if defined(__unix__) || defined(__APPLE__)
  void *begin_ = nullptr;
  std::size_t bytes_ = 0;
#endif
};

void initializeSound(SoundData &sound) {
  sound.channels = 1;
  sound.outputData = {1000, 1000};
  sound.outputFrameCount = 2;
}

void unrelatedOwnerStopsDoNotTouchChartSchedule() {
  SoundData chart;
  initializeSound(chart);
  std::array<SoundData, 256> skinSounds;
  std::array<std::uint64_t, 256> sequences{};
  AudioCallbackState state;
  for (std::size_t i = 0; i < state.scheduledSoundCapacity; ++i) {
    require(audio::playback::InsertScheduledSound(
                state, {.soundData = &chart, .bus = audio::Bus::Bgm,
                        .startMicros = static_cast<long long>(1'000'000 + i)}),
            "stage full chart schedule");
  }
  require(audio::playback::AppendActiveSound(state, &chart, audio::Bus::Bgm, 0),
          "unrelated BGM voice starts");
  for (std::size_t i = 0; i < skinSounds.size(); ++i) {
    initializeSound(skinSounds[i]);
    require(audio::playback::AppendActiveSound(
                state, &skinSounds[i], audio::Bus::System, 0),
            "skin sound starts without ever being scheduled");
    const AudioCommand command{.type = AudioCommandType::StopOwner,
                               .soundData = &skinSounds[i]};
    require(i % 2 == 0
                ? audio::playback::EnqueueOwnerControlCommand(state, command, &sequences[i])
                : audio::playback::EnqueueOwnerRetirementCommand(state, command, &sequences[i]),
            "owner stop or retirement enters command queue");
  }
  {
    InaccessibleSchedulePages protectedSchedule(state);
    audio::playback::DrainCommands(state);
    for (std::size_t i = 0; i < skinSounds.size(); ++i) {
      require(!skinSounds[i].playing &&
                  skinSounds[i].ownerControlAcknowledgedSequence.load() == sequences[i],
              "unrelated owner stops remove active voices and acknowledge retirement");
    }
    std::atomic_bool acknowledged = false;
    const auto reservation = audio::playback::TryReserveRealtimeCommand(state);
    require(reservation.has_value() && audio::playback::CommitRealtimeCommand(
                state, *reservation,
                {.type = AudioCommandType::StopOwner, .soundData = &skinSounds[0],
                 .acknowledgement = &acknowledged, .submissionSequence = 999}),
            "realtime owner stop enters command queue");
    audio::playback::DrainRealtimeCommands(state);
    require(acknowledged.load() && skinSounds[0].ownerControlAcknowledgedSequence.load() == 999,
            "realtime owner stop also acknowledges without touching schedule");
  }
  require(state.playingSoundCount == 1 && state.playingSounds[0].soundData == &chart &&
              state.activeNonSystemVoices.load() == 1,
          "owner cleanup preserves active chart voice");
  require(state.scheduledSoundCount == state.scheduledSoundCapacity &&
              state.scheduledNonSystemSounds.load() == state.scheduledSoundCapacity,
          "owner cleanup preserves complete chart schedule counts");
  for (std::size_t i = 0; i < state.scheduledSoundCount; ++i) {
    require(state.scheduledSounds[i].soundData == &chart &&
                state.scheduledSounds[i].startMicros == static_cast<long long>(1'000'000 + i),
            "owner cleanup preserves every future chart event");
  }
}

void scheduledOwnersStillCancelAndAcknowledge() {
  SoundData owner, other;
  initializeSound(owner);
  initializeSound(other);
  AudioCallbackState state;
  require(audio::playback::InsertScheduledSound(
              state, {.soundData = &other, .startMicros = 100}) &&
              audio::playback::InsertScheduledSound(
                  state, {.soundData = &other, .startMicros = 300}) &&
              audio::playback::InsertScheduledSound(
                  state, {.soundData = &owner, .startMicros = 200}) &&
              audio::playback::InsertScheduledSound(
                  state, {.soundData = &owner, .startMicros = 250}),
          "owner's first scheduled event uses sorted insertion");
  require(audio::playback::AppendActiveSound(state, &owner, audio::Bus::Keysound, 0),
          "scheduled owner also has an active voice");
  std::atomic_bool acknowledged = false;
  std::uint64_t sequence = 0;
  require(audio::playback::EnqueueOwnerRetirementCommand(
              state, {.type = AudioCommandType::StopOwner, .soundData = &owner,
                      .acknowledgement = &acknowledged}, &sequence),
          "scheduled owner retirement enters queue");
  audio::playback::DrainCommands(state);
  require(acknowledged.load() && owner.ownerControlAcknowledgedSequence.load() == sequence &&
              state.playingSoundCount == 0 && state.scheduledSoundCount == 2 &&
              state.scheduledSounds[0].soundData == &other &&
              state.scheduledNonSystemSounds.load() == 2,
          "retirement cancels all owner events before acknowledging and preserves other owner");

  require(audio::playback::EnqueueCommand(
              state, {.type = AudioCommandType::Schedule, .soundData = &owner,
                      .startMicros = 400}),
          "previously removed owner can be scheduled again through commands");
  audio::playback::DrainCommands(state);
  acknowledged.store(false);
  const auto reservation = audio::playback::TryReserveRealtimeCommand(state);
  require(reservation.has_value() && audio::playback::CommitRealtimeCommand(
              state, *reservation,
              {.type = AudioCommandType::StopOwner, .soundData = &owner,
               .acknowledgement = &acknowledged, .submissionSequence = 1000}),
          "scheduled owner realtime stop enters queue");
  audio::playback::DrainRealtimeCommands(state);
  require(acknowledged.load() && owner.ownerControlAcknowledgedSequence.load() == 1000 &&
              state.scheduledSoundCount == 2 && state.scheduledSounds[0].soundData == &other,
          "realtime stop cancels newly scheduled owner before acknowledgement");
  audio::playback::RemoveSound(state, &other);
  require(state.scheduledSoundCount == 0 && state.scheduledNonSystemSounds.load() == 0,
          "owner whose first event used append also cancels all scheduled events");
}
}

int main() {
  try {
    unrelatedOwnerStopsDoNotTouchChartSchedule();
    scheduledOwnersStillCancelAndAcknowledge();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "audio_owner_removal_tests: %s\n", error.what());
    return 1;
  }
  return 0;
}
