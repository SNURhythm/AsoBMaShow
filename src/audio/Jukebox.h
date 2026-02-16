#pragma once
#include "../bms_parser.hpp"
#include "AudioWrapper.h"
#include <array>
#include <future>
#include <thread>
#include <unordered_map>
#include <atomic>
#include "../path.h"
#include "../video/VideoPlayer.h"
#include "../utils/Stopwatch.h"
#include <functional>

#include <cassert>

struct ImageData {
  bgfx::TextureHandle texture;
  int width;
  int height;
  int channels;
};
class Jukebox {
public:
  struct PerformanceAnalytics{
    static const int BUFFER_SIZE = 10000;
    // ring buffer to store loop delta time
    std::array<double, BUFFER_SIZE> loopDeltaTimes;
    std::atomic<size_t> loopDeltaIndex{0};
    size_t cursor = 0;
    double avgDeltaTime = 0;
    double statsSum = 0;
    size_t statsCount = 0;
  };
  PerformanceAnalytics performanceAnalytics;
  
  Jukebox(Stopwatch *stopwatch);
  ~Jukebox();

  // NOTE: Reading delta time is NOT THREAD SAFE, call this from render thread
  double getAvgDeltaTime();

  void loadChart(bms_parser::Chart &chart, bool scheduleNotes,
                 std::atomic_bool &isCancelled);
  void schedule(bms_parser::Chart &chart, bool scheduleNotes,
                std::atomic_bool &isCancelled);
  void playKeySound(int wav);
  void play();
  void stop();
  void render();
  bool hasActiveVisuals() const;

  long long getTimeMicros();
  void seek(long long micro);
  std::function<void(long long)> onTickCb;
  void onTick(const std::function<void(long long)> &cb) {
    assert(!isPlaying && "onTick callback should be set before playing");
    onTickCb = cb;
  }
  void removeOnTick() { onTickCb = nullptr; }
  void pause();
  void resume();
  bool isPaused();

private:
  bgfx::UniformHandle s_texColor;
  // seek lock
  std::mutex seekLock;
  // playthread lock
  std::mutex playThreadLock;
  void loadSounds(bms_parser::Chart &chart, std::atomic_bool &isCancelled);
  void loadBMPs(bms_parser::Chart &chart, std::atomic_bool &isCancelled);
  void renderImage(ImageData &image, int viewId);
  std::atomic_bool isPlaying = false;
  std::thread playThread;
  Stopwatch *stopwatch;
  AudioWrapper audio;
  std::vector<std::pair<long long, int>> audioList;
  size_t audioCursor = 0;
  std::vector<std::pair<long long, int>> bmpList;
  std::vector<std::pair<long long, int>> bmpLayerList;
  size_t bmpCursor = 0;
  size_t bmpLayerCursor = 0;
  std::unordered_map<int, path_t> wavTableAbs;
  std::unordered_map<int, VideoPlayer *> videoPlayerTable;
  std::mutex videoPlayerTableMutex;
  std::unordered_map<int, ImageData> imageTable;
  std::mutex imageTableMutex;
  std::vector<std::future<bool>> asyncVideoLoads;
  long long lastPositionMicro = 0;
  double diffSamples[100];
  int diffSampleCount = 0;
  std::atomic<int> currentBga{-1};
  std::atomic<int> currentBmpLayer{-1};
  const std::string audioExtensions[4] = {"flac", "wav", "ogg", "mp3"};
  const std::string videoExtensions[9] = {"mp4",  "wmv", "m4v", "webm", "mpg",
                                          "mpeg", "m1v", "m2v", "avi"};
  const std::string imageExtensions[6] = {"jpg", "jpeg", "gif",
                                          "bmp", "png",  "tga"};
};
