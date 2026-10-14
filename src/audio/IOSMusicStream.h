#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace music_stream { class Source; }
struct IOSNativeMusicState;

// Only the feeder touches source PCM. Callbacks are delivered asynchronously on
// the main queue and must verify that this stream is still the active backend.
class IOSMusicStream {
public:
  enum class EventKind { StateChanged, Finished, Error };
  using Event = std::function<void(std::uint64_t epoch, EventKind kind,
                                   const std::string &error)>;
  static std::shared_ptr<IOSMusicStream> Create(
      std::shared_ptr<music_stream::Source> source, Event event,
      std::string &error);
  ~IOSMusicStream();
  bool Play(std::string &error);
  void Pause();
  void Stop();
  void Seek(long long positionMicros);
  void SetRate(float rate, bool timeStretch);
  IOSNativeMusicState State();
  void ConfigurationChanged(void *engine);
  bool IsCurrentEvent(std::uint64_t epoch) const;
  bool IsBuffering() const;
private:
  struct Impl;
  explicit IOSMusicStream(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl;
};
