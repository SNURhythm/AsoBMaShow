#include "video/VideoPlayer.h"
#include "video/VideoDecodeState.h"
#include "rendering/ShaderManager.h"
#include "rendering/UniformCache.h"
#include "visual_catch_up_video_fixture.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <inttypes.h>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace visual_catch_up_fixture {
namespace {

struct State {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Event> events;
  std::unordered_set<const VideoPlayer *> workers;
  std::unordered_set<const VideoPlayer *> releasedWorkers;
  std::unordered_map<const VideoPlayer *, const std::mutex *> outputMutexes;
  bool holdWorkers = false;
  bool catchUpProbe = false;
  bool probing = false;
  bool witnessed = false;
  bool rejected = false;
  const VideoPlayer *probedPlayer = nullptr;
  std::size_t seekOrdinal = 0;
  std::size_t failedSeekOrdinal = 0;
  const VideoPlayer *heldWaitPlayer = nullptr;
  bool heldWaitReady = false;
  bool heldWaitReached = false;
} state;

template <typename Predicate>
void await(std::unique_lock<std::mutex> &lock, Predicate predicate,
           const char *message) {
  if (!state.changed.wait_for(lock, std::chrono::seconds(5), predicate)) {
    throw std::runtime_error(message);
  }
}

void record(Event event) {
  std::lock_guard lock(state.mutex);
  state.events.push_back(event);
  if (state.probing && event.player == state.probedPlayer &&
      (event.boundary == Boundary::Receive || event.boundary == Boundary::Read ||
       event.boundary == Boundary::Send || event.boundary == Boundary::Drain)) {
    state.witnessed = true;
  }
  state.changed.notify_all();
}

std::int64_t markerMicros(const std::uint8_t *data, int pitch) {
  const unsigned int marker = data[0];
  bool valid = marker >= 16 && marker <= 168 && (marker - 16) % 8 == 0;
  for (int row = 0; row < 16; ++row) {
    for (int column = 0; column < 16; ++column) {
      valid = valid && data[row * pitch + column] == marker;
    }
  }
  return valid ? static_cast<std::int64_t>((marker - 16) / 8) * 50'000 : -1;
}

}

Session::Session(bool holdWorkers) {
  std::lock_guard lock(state.mutex);
  state.events.clear();
  state.workers.clear();
  state.releasedWorkers.clear();
  state.outputMutexes.clear();
  state.holdWorkers = holdWorkers;
  state.catchUpProbe = false;
  state.probing = false;
  state.witnessed = false;
  state.rejected = false;
  state.probedPlayer = nullptr;
  state.seekOrdinal = 0;
  state.failedSeekOrdinal = 0;
  state.heldWaitPlayer = nullptr;
  state.heldWaitReached = false;
}

Session::~Session() {
  releaseHeldWait();
  releaseWorkers();
}

void awaitWorkers(std::size_t count) {
  std::unique_lock lock(state.mutex);
  await(lock, [&] { return state.workers.size() == count; },
        "decoder workers must reach their startup barrier");
}

void releaseWorkers() {
  std::lock_guard lock(state.mutex);
  state.holdWorkers = false;
  state.changed.notify_all();
}

void armCatchUpProbe(std::size_t failedSeekOrdinal) {
  std::lock_guard lock(state.mutex);
  state.catchUpProbe = true;
  state.seekOrdinal = 0;
  state.failedSeekOrdinal = failedSeekOrdinal;
}

void finishCatchUpProbe() {
  std::lock_guard lock(state.mutex);
  state.probing = false;
  state.catchUpProbe = false;
}

std::vector<Event> events() {
  std::lock_guard lock(state.mutex);
  return state.events;
}

bool rejectedCatchUpAdmission() {
  std::lock_guard lock(state.mutex);
  return state.witnessed && state.rejected;
}

void awaitFrame(const VideoPlayer *player, std::int64_t micros,
                std::size_t after) {
  std::unique_lock lock(state.mutex);
  await(lock, [&] {
    return std::any_of(state.events.begin() + after, state.events.end(), [&](const auto &event) {
      return event.boundary == Boundary::Frame && event.player == player &&
             event.micros == micros;
    });
  }, "released decoder must produce the expected real marker frame");
}

void awaitBoundary(const VideoPlayer *player, Boundary boundary,
                   std::size_t after) {
  std::unique_lock lock(state.mutex);
  await(lock, [&] {
    return std::any_of(state.events.begin() + after, state.events.end(), [&](const auto &event) {
      return event.player == player && event.boundary == boundary;
    });
  }, "worker must reach the requested real boundary");
}

void holdNextOutputWait(const VideoPlayer *player, bool ready) {
  std::lock_guard lock(state.mutex);
  state.heldWaitPlayer = player;
  state.heldWaitReady = ready;
  state.heldWaitReached = false;
}

void awaitHeldWait() {
  std::unique_lock lock(state.mutex);
  await(lock, [] { return state.heldWaitReached; },
        "worker must reach the held real wait predicate");
}

void releaseHeldWait() {
  std::lock_guard lock(state.mutex);
  state.heldWaitPlayer = nullptr;
  state.changed.notify_all();
}

AVPacket *allocatePacket(const VideoPlayer *player) {
  {
    std::unique_lock lock(state.mutex);
    state.workers.insert(player);
    state.changed.notify_all();
    state.changed.wait(lock, [&] {
      return !state.holdWorkers || state.releasedWorkers.contains(player);
    });
  }
  return av_packet_alloc();
}

int seek(const VideoPlayer *player, AVFormatContext *context, int streamIndex,
         std::int64_t timestamp, int flags) {
  bool fail = false;
  {
    std::unique_lock lock(state.mutex);
    ++state.seekOrdinal;
    fail = state.seekOrdinal == state.failedSeekOrdinal;
    if (state.catchUpProbe && state.seekOrdinal == 1) {
      state.probedPlayer = player;
    } else if (state.catchUpProbe && state.seekOrdinal == 2) {
      if (player == state.probedPlayer) {
        throw std::runtime_error("admission probe needs two independent players");
      }
      state.probing = true;
      state.releasedWorkers.insert(state.probedPlayer);
      state.changed.notify_all();
      await(lock, [] { return state.witnessed; },
            "worker must positively witness rejection or decoder admission");
    }
  }
  const int result = fail ? AVERROR(EIO)
                          : av_seek_frame(context, streamIndex, timestamp, flags);
  const auto micros = av_rescale_q(timestamp,
      context->streams[streamIndex]->time_base, AVRational{1, AV_TIME_BASE});
  record({Boundary::Seek, player, micros, result});
  return result;
}

void flush(const VideoPlayer *player, AVCodecContext *context) {
  avcodec_flush_buffers(context);
  record({Boundary::Flush, player});
}

int receive(const VideoPlayer *player, AVCodecContext *context, AVFrame *frame) {
  record({Boundary::Receive, player});
  const int result = avcodec_receive_frame(context, frame);
  if (result == 0 && frame->width == 16 && frame->height == 16 &&
      frame->format == AV_PIX_FMT_YUV420P) {
    record({Boundary::Frame, player, markerMicros(frame->data[0], frame->linesize[0])});
  }
  return result;
}

int read(const VideoPlayer *player, AVFormatContext *context, AVPacket *packet) {
  record({Boundary::Read, player});
  return av_read_frame(context, packet);
}

int send(const VideoPlayer *player, AVCodecContext *context,
         const AVPacket *packet) {
  record({packet ? Boundary::Send : Boundary::Drain, player});
  return avcodec_send_packet(context, packet);
}

void freePacket(const VideoPlayer *player, AVPacket **packet) {
  av_packet_free(packet);
  record({Boundary::WorkerStopped, player});
}

void upload(const VideoPlayer *player, const std::uint8_t *data, int pitch) {
  record({Boundary::Upload, player, markerMicros(data, pitch)});
}

template <typename Predicate>
auto observeWait(const VideoPlayer *player, std::unique_lock<std::mutex> &lock,
                 Predicate predicate) {
  return [player, mutex = lock.mutex(), predicate = std::move(predicate)] {
    const bool ready = predicate();
    std::unique_lock observerLock(state.mutex);
    const auto [output, inserted] = state.outputMutexes.try_emplace(player, mutex);
    if (!ready && output->second == mutex) {
      state.events.push_back({Boundary::Rejected, player});
      if (state.probing && !state.witnessed && state.probedPlayer == player) {
        state.witnessed = true;
        state.rejected = true;
      }
      state.changed.notify_all();
    }
    if (!ready && output->second != mutex) {
      state.events.push_back({Boundary::EofWait, player});
      state.changed.notify_all();
    }
    if (output->second == mutex && state.heldWaitPlayer == player &&
        state.heldWaitReady == ready && !state.heldWaitReached) {
      state.heldWaitReached = true;
      state.changed.notify_all();
      state.changed.wait(observerLock, [player] {
        return state.heldWaitPlayer != player;
      });
    }
    return ready;
  };
}

}

namespace bgfx {
void observeCatchUpTextureUpload(
    const VideoPlayer *player, TextureHandle handle, std::uint16_t layer,
    std::uint8_t mip, std::uint16_t x, std::uint16_t y, std::uint16_t width,
    std::uint16_t height, const Memory *memory, std::uint16_t pitch) {
  if (width == 16 && height == 16) {
    visual_catch_up_fixture::upload(player, memory->data, pitch);
  }
  updateTexture2D(handle, layer, mip, x, y, width, height, memory, pitch);
}
}

#define av_packet_alloc() visual_catch_up_fixture::allocatePacket(this)
#define av_packet_free(...) visual_catch_up_fixture::freePacket(this, __VA_ARGS__)
#define av_seek_frame(...) visual_catch_up_fixture::seek(this, __VA_ARGS__)
#define avcodec_flush_buffers(...) visual_catch_up_fixture::flush(this, __VA_ARGS__)
#define avcodec_receive_frame(...) visual_catch_up_fixture::receive(this, __VA_ARGS__)
#define av_read_frame(...) visual_catch_up_fixture::read(this, __VA_ARGS__)
#define avcodec_send_packet(...) visual_catch_up_fixture::send(this, __VA_ARGS__)
#define wait(lock, ...) wait(lock, visual_catch_up_fixture::observeWait(this, lock, __VA_ARGS__))
#define updateTexture2D(...) observeCatchUpTextureUpload(this, __VA_ARGS__)
#include "video/VideoPlayer.cpp"
#undef updateTexture2D
#undef wait
#undef avcodec_send_packet
#undef av_read_frame
#undef avcodec_receive_frame
#undef avcodec_flush_buffers
#undef av_seek_frame
#undef av_packet_alloc
#undef av_packet_free
