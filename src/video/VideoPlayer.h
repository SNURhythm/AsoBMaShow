#pragma once

#include <SDL2/SDL.h>
#include <bgfx/bgfx.h>
#include <semaphore>
#include <mutex>
#include "../utils/Stopwatch.h"
#include "../rendering/common.h"
#include <queue>
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
  VideoPlayer(Stopwatch *stopwatch);
  ~VideoPlayer();

  bool loadVideo(const std::string &videoPath, std::atomic<bool> &isCancelled);
  void update();
  void render();
  void play();
  void pause();
  void stop();
  void seek(int64_t micro);
  float viewWidth = 1920.0f;
  float viewHeight = 1080.0f;
  int viewId = rendering::bga_view;
  float viewX = 0.0f;
  float viewY = 0.0f;
  // float fps = 60.0f;

private:
  std::atomic<bool> isEOF = false;
  Stopwatch *stopwatch;
  std::atomic<bool> isPlaying;
  std::atomic<bool> isPaused;
  std::atomic<bool> stopRequested;
  std::atomic<bool> predecodingActive;
  std::atomic<int64_t> seekPosition;
  std::thread predecodeThread;

  AVFormatContext *formatContext = nullptr;
  AVCodecContext *codecContext = nullptr;
  AVFrame *frame = nullptr;
  AVPacket *packet = nullptr;
  SwsContext *swsContext = nullptr;
  int videoStreamIndex = -1;
  std::mutex videoMutex;

  std::vector<AVFrame *> frameBuffer; // Fixed-size ring buffer
  std::atomic<size_t> bufferHead = 0;
  std::atomic<size_t> bufferTail = 0;
  static const int maxBufferSize = 10; // Adjust as needed
  std::atomic<size_t> bufferSize = 0;
  std::condition_variable freeSpace;
  std::mutex bufferMutex; // Protect ring buffer operations

  long long startTime;       // Start time for playback
  double lastFramePTS = 0.0; // Last decoded frame's PTS for synchronization

  std::queue<AVFrame *> frameQueue; // Queue for pre-decoded frames
  std::condition_variable bufferCV; // Condition variable for buffer signaling

  std::mutex eofMutex;
  std::condition_variable eofCV; // Condition variable for eof signaling

  const size_t maxBufferFrames = 120; // Maximum number of frames in the buffer
  bool isBuffering = false;           // Indicates if buffering is in progress

  uint32_t setupFormat(char *chroma, unsigned *width, unsigned *height,
                       unsigned *pitches, unsigned *lines);
  void unloadVideo();
  void predecodeFrames();
  void stopPredecoding();

  void updateVideoTexture(unsigned int width, unsigned int height);
  std::mutex videoFrameMutex;

  int videoFrameWidth;
  int videoFrameHeight;
  bool hasVideoFrame;

  bgfx::UniformHandle s_texY = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_texU = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle s_texV = BGFX_INVALID_HANDLE;

  int64_t startPTS = 0;
  unsigned int getPrecisePosition();
  int currentTextureIndex = 0;
  bgfx::TextureHandle videoTextureY = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle videoTextureU = BGFX_INVALID_HANDLE;
  bgfx::TextureHandle videoTextureV = BGFX_INVALID_HANDLE;

  std::vector<AVFrame *> recyclePool;
  std::mutex recycleMutex;
  AVFrame *getRecycledFrame();
  void recycleFrame(AVFrame *frame);
};
