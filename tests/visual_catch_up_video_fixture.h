#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class VideoPlayer;

namespace visual_catch_up_fixture {

enum class Boundary {
  Seek, Flush, Receive, Read, Send, Drain, Frame, Upload, Rejected, EofWait,
  WorkerStopped
};

struct Event {
  Boundary boundary;
  const VideoPlayer *player;
  std::int64_t micros = -1;
  int result = 0;
};

class Session {
public:
  explicit Session(bool holdWorkers = true);
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
};

void awaitWorkers(std::size_t count);
void releaseWorkers();
void armCatchUpProbe(std::size_t failedSeekOrdinal = 0);
void finishCatchUpProbe();
std::vector<Event> events();
bool rejectedCatchUpAdmission();
void awaitFrame(const VideoPlayer *player, std::int64_t micros,
                std::size_t after = 0);
void awaitBoundary(const VideoPlayer *player, Boundary boundary,
                   std::size_t after = 0);
void holdNextOutputWait(const VideoPlayer *player, bool ready);
void awaitHeldWait();
void releaseHeldWait();

}
