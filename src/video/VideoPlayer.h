#pragma once

#include <SDL2/SDL.h>
#include <bgfx/bgfx.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>
#include "VideoFrameLayout.h"
#include "VideoMemoryBudget.h"
#include "../utils/Stopwatch.h"
#include "../rendering/common.h"
#include <thread>
#include <atomic>
#include <condition_variable>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

class VideoPlayer {
public:
  enum class MemoryPressureMode { PreserveActive, DiscardIdle };

  struct PreparedEmbeddedSubmission {
    video::EmbeddedYuvQuadLayout quad;
    std::uint64_t state = 0;
    std::optional<rendering::DrawableScissor> scissor;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    std::array<bgfx::TextureHandle, 3> textures{
        bgfx::TextureHandle{bgfx::kInvalidHandle},
        bgfx::TextureHandle{bgfx::kInvalidHandle},
        bgfx::TextureHandle{bgfx::kInvalidHandle}};
    std::array<bgfx::UniformHandle, 3> samplers{
        bgfx::UniformHandle{bgfx::kInvalidHandle},
        bgfx::UniformHandle{bgfx::kInvalidHandle},
        bgfx::UniformHandle{bgfx::kInvalidHandle}};
    bgfx::TransientVertexBuffer vertexBuffer{};
    bgfx::TransientIndexBuffer indexBuffer{};
  };

  struct LoadLimits {
    int maximumDimension = std::numeric_limits<std::uint16_t>::max();
    std::size_t maximumRgbaBytes = std::numeric_limits<std::size_t>::max();
    std::size_t maximumDecodedBytes = std::numeric_limits<std::size_t>::max();
    bool requirePreallocationBounds = false;
  };

  VideoPlayer(Stopwatch *stopwatch);
  ~VideoPlayer();
  VideoPlayer(const VideoPlayer &) = delete;
  VideoPlayer &operator=(const VideoPlayer &) = delete;
  VideoPlayer(VideoPlayer &&) = delete;
  VideoPlayer &operator=(VideoPlayer &&) = delete;

  bool loadVideo(const std::string &videoPath, std::atomic<bool> &isCancelled);
  bool loadVideo(const std::string &videoPath, std::atomic<bool> &isCancelled,
                 const LoadLimits &limits);
  void update();
  void render(bgfx::ViewId viewId, float viewX, float viewY, float viewWidth,
              float viewHeight) const;
  void renderEmbedded(
      bgfx::ViewId viewId, const video::EmbeddedYuvQuadLayout &quad,
      std::uint64_t state,
      std::optional<rendering::DrawableScissor> scissor = std::nullopt) const;
  [[nodiscard]] std::optional<PreparedEmbeddedSubmission>
  prepareEmbeddedSubmission(
      const video::EmbeddedYuvQuadLayout &quad, std::uint64_t state,
      std::optional<rendering::DrawableScissor> scissor = std::nullopt) const;
  [[nodiscard]] static const bgfx::VertexLayout &
  embeddedVertexLayout() noexcept;
  void commitPreparedEmbedded(
      PreparedEmbeddedSubmission &submission) const noexcept;
  void submitPreparedEmbedded(
      bgfx::ViewId viewId,
      const PreparedEmbeddedSubmission &submission) const noexcept;
  void play();
  void playFrom(int64_t micro);
  void pause();
  void stop();
  void seek(int64_t micro);
  void setDecodeSuspended(bool suspended);
  void handleMemoryPressure(MemoryPressureMode mode);
  long long getDurationMicros() const;
  int getFrameWidth() const { return videoFrameWidth; }
  int getFrameHeight() const { return videoFrameHeight; }
  std::size_t getReservedDecodedBytes() const noexcept {
    return reservedDecodedBytes;
  }
  // float fps = 60.0f;

private:
  std::atomic<bool> isEOF = false;
  Stopwatch *stopwatch;
  std::atomic<bool> isPlaying{false};
  std::atomic<bool> isPaused{false};
  std::atomic<bool> predecodingActive{false};
  std::atomic<bool> decodeSuspended{false};
  std::atomic<std::uint64_t> decodeGeneration{0};
  std::thread predecodeThread;

  AVFormatContext *formatContext = nullptr;
  AVCodecContext *codecContext = nullptr;
  SwsContext *swsContext = nullptr;
  int videoStreamIndex = -1;
  mutable std::mutex videoMutex;

  std::vector<AVFrame *> frameBuffer; // Fixed-size ring buffer
  std::atomic<size_t> bufferHead = 0;
  std::atomic<size_t> bufferTail = 0;
  static constexpr std::size_t maxBufferSize = 3;
  static constexpr std::size_t maxRecyclePoolSize = 2;
  std::atomic<size_t> bufferSize = 0;
  std::condition_variable freeSpace;
  std::mutex bufferMutex; // Protect ring buffer operations

  long long startTime;       // Start time for playback
  double lastFramePTS = 0.0; // Last decoded frame's PTS for synchronization

  std::mutex eofMutex;
  std::condition_variable eofCV; // Condition variable for eof signaling

  void unloadVideo();
  void destroyVideoTextures();
  void predecodeFrames();
  void stopPredecoding();

  bool updateVideoTexture(int width, int height);
  mutable std::mutex videoFrameMutex;

  int videoFrameWidth;
  int videoFrameHeight;
  bool hasVideoFrame;
  std::size_t reservedDecodedBytes = 0;

  bgfx::UniformHandle s_texY = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_texU = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_texV = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle fullscreenProgram = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle embeddedProgram = BGFX_INVALID_HANDLE;

  int64_t startPTS = 0;
  unsigned int getPrecisePosition();
  bgfx::TextureHandle videoTextureY = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle videoTextureU = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle videoTextureV = BGFX_INVALID_HANDLE;

  std::vector<AVFrame *> recyclePool;
  std::mutex recycleMutex;
  AVFrame *getRecycledFrame();
  void recycleFrame(AVFrame *frame);
};
