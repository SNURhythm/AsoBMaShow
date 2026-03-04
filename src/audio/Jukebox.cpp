#include "Jukebox.h"
#include <SDL2/SDL.h>
#include <thread>
#include "../Utils.h"
#include "../game/GameState.h"
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/UniformCache.h"
#include "bgfx/bgfx.h"
#include <stb_image.h>
#ifdef _WIN32
#include <timeapi.h>
#include <windows.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#endif

Jukebox::Jukebox(Stopwatch *stopwatch)
    : audio(stopwatch), stopwatch(stopwatch) {
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
}

Jukebox::~Jukebox() {
  isPlaying = false;
  if (playThread.joinable())
    playThread.join();
  audio.stopSounds();
  audio.unloadSounds();
  for (auto &videoPlayer : videoPlayerTable) {
    delete videoPlayer.second;
  }
  for (auto &image : imageTable) {
    bgfx::destroy(image.second.texture);
  }
}
void Jukebox::render() {
  const int bga = currentBga.load(std::memory_order_relaxed);
  if (bga != -1) {
    auto videoIt = videoPlayerTable.find(bga);
    if (videoIt != videoPlayerTable.end()) {
      auto *videoPlayer = videoIt->second;
      videoPlayer->viewWidth = rendering::window_width;
      videoPlayer->viewHeight = rendering::window_height;
      videoPlayer->viewId = rendering::bga_view;
      videoPlayer->update();
      videoPlayer->render();
    } else {
      auto imageIt = imageTable.find(bga);
      if (imageIt != imageTable.end()) {
        renderImage(imageIt->second, rendering::bga_view);
      }
    }
  }
  const int bmpLayer = currentBmpLayer.load(std::memory_order_relaxed);
  if (bmpLayer != -1) {
    auto videoIt = videoPlayerTable.find(bmpLayer);
    if (videoIt != videoPlayerTable.end()) {
      auto *videoPlayer = videoIt->second;
      videoPlayer->viewWidth = rendering::window_width;
      videoPlayer->viewHeight = rendering::window_height;
      videoPlayer->viewId = rendering::bga_layer_view;
      videoPlayer->update();
      videoPlayer->render();
    } else {
      auto imageIt = imageTable.find(bmpLayer);
      if (imageIt != imageTable.end()) {
        renderImage(imageIt->second, rendering::bga_layer_view);
      }
    }
  }
}

bool Jukebox::hasActiveVisuals() const {
  return currentBga.load(std::memory_order_relaxed) != -1 ||
         currentBmpLayer.load(std::memory_order_relaxed) != -1;
}

void Jukebox::loadSounds(bms_parser::Chart &chart,
                         std::atomic_bool &isCancelled) {
  std::mutex wavTableLock;

  wavTableAbs.clear();

  parallel_for(chart.WavTable.size(), [&](int start, int end) {
    auto wav = std::next(chart.WavTable.begin(), start);
    for (int i = start; i < end; i++, ++wav) {
      if (isCancelled)
        return;
      bool found = false;
      std::filesystem::path basePath = chart.Meta.Folder / wav->second;
      std::filesystem::path path;

      // Try each extension until one succeeds
      for (const auto &ext : audioExtensions) {
        if (isCancelled)
          return;
        path = basePath;
        path.replace_extension(ext);
        if (!std::filesystem::exists(path)) {
          continue;
        }
        if (audio.loadSound(path.c_str(), isCancelled)) {
          std::lock_guard<std::mutex> lock(wavTableLock);
          auto idx = wav->first;
          SDL_Log("Loaded sound %d: %s", idx, path_t_to_utf8(path).c_str());
          wavTableAbs[idx] = path;
          found = true;
          break;
        }
      }
      if (!found) {
        SDL_Log("Failed to load sound for all extensions: %s",
                basePath.c_str());
      }
    }
  });
}
void Jukebox::loadBMPs(bms_parser::Chart &chart,
                       std::atomic_bool &isCancelled) {
  parallel_for(chart.BmpTable.size(), [&](int start, int end) {
    auto bmp = std::next(chart.BmpTable.begin(), start);
    for (int i = start; i < end; i++, ++bmp) {
      if (isCancelled)
        return;
      bool found = false;
      std::filesystem::path basePath = chart.Meta.Folder / bmp->second;
      std::filesystem::path path;

      // Try each extension until one succeeds
      for (const auto &ext : videoExtensions) {
        if (isCancelled)
          return;
        path = basePath;
        path.replace_extension(ext);
        if (!std::filesystem::exists(path)) {
          continue;
        }
        // calculate hash of base path
        // auto pathHash = bms_parser::md5(basePath.string());
        // auto fileName = pathHash + "-" + std::to_string(bmp->first) + ".mp4";
        // auto transcodedPath =
        //     (Utils::GetDocumentsPath("temp") / fileName).string();
        // if (!std::filesystem::exists(transcodedPath)) {
        //   // mkdir
        //   std::filesystem::create_directories(Utils::GetDocumentsPath("temp"));
        //   int result = transcode(path.string().c_str(),
        //   transcodedPath.c_str(),
        //                          &isCancelled);
        //   if (isCancelled) {
        //     // delete transcoded file
        //     std::filesystem::remove(transcodedPath);
        //     return;
        //   }
        //   if (result != 0) {
        //     SDL_Log("Failed to transcode video: %ls", path.c_str());
        //     continue;
        //   }
        // }
        // new video player
        auto videoPlayer = new VideoPlayer(stopwatch);
        path_t p = fspath_to_path_t(path);

        if (videoPlayer->loadVideo(path_t_to_utf8(p), isCancelled)) {
          std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
          videoPlayerTable[bmp->first] = videoPlayer;

          SDL_Log("video width: %f, video height: %f", videoPlayer->viewWidth,
                  videoPlayer->viewHeight);

          found = true;
          SDL_Log("Loaded video to id: %d", bmp->first);
          break;
        } else {
          SDL_Log("Failed to load video: %s", path.c_str());
          delete videoPlayer;
        }
      }

      // if not found, fall back to image loading
      if (!found) {
        for (const auto &ext : imageExtensions) {
          if (isCancelled)
            return;
          path = basePath;
          path.replace_extension(ext);
          if (!std::filesystem::exists(path)) {
            continue;
          }
          path_t p = fspath_to_path_t(path);
          std::string utf8Path = path_t_to_utf8(p);
          int width, height, channels;
          unsigned char *data =
              stbi_load(utf8Path.c_str(), &width, &height, &channels, 4);
          if (!data) {
            SDL_Log("Failed to load image: %s", utf8Path.c_str());
            continue;
          }
          SDL_Log("Loaded image: %s", utf8Path.c_str());
          {
            std::lock_guard<std::mutex> lock(imageTableMutex);
            imageTable[bmp->first] = {
                .texture = bgfx::createTexture2D(
                    width, height, false, 1, bgfx::TextureFormat::RGBA8,
                    BGFX_TEXTURE_NONE, bgfx::copy(data, width * height * 4)),
                .width = width,
                .height = height,
                .channels = channels,
            };
          }
          stbi_image_free(data);
          break;
        }
      }
    }
  });
}
void Jukebox::loadChart(bms_parser::Chart &chart, bool scheduleNotes,
                        std::atomic_bool &isCancelled) {
  isPlaying = false;
  if (playThread.joinable()) {
    SDL_Log("Joining playThread");
    playThread.join();
  }

  audio.stopSounds();
  audio.unloadSounds();

  currentBga.store(-1, std::memory_order_relaxed);
  currentBmpLayer.store(-1, std::memory_order_relaxed);
  for (auto &videoPlayer : videoPlayerTable) {
    delete videoPlayer.second;
  }
  videoPlayerTable.clear();

  for (auto &image : imageTable) {
    bgfx::destroy(image.second.texture);
  }
  imageTable.clear();
  if (isCancelled)
    return;
  SDL_Log("Loading sounds");
  std::thread loadSoundThread(
      [this, &chart, &isCancelled] { loadSounds(chart, isCancelled); });
  SDL_Log("Loading videos");
  loadBMPs(chart, isCancelled);
  loadSoundThread.join();

  if (isCancelled)
    return;
  schedule(chart, scheduleNotes, isCancelled);
  SDL_Log("Chart loaded");
}

void Jukebox::schedule(bms_parser::Chart &chart, bool scheduleNotes,
                       std::atomic_bool &isCancelled) {
  audioCursor = 0;
  bmpCursor = 0;
  bmpLayerCursor = 0;
  audioList.clear();
  bmpList.clear();
  bmpLayerList.clear();
  for (auto &measure : chart.Measures) {
    if (isCancelled)
      return;
    for (auto &timeline : measure->TimeLines) {
      if (isCancelled)
        return;
      if (timeline->BgaBase != -1) {
        bmpList.emplace_back(timeline->Timing, timeline->BgaBase);
      }
      if (timeline->BgaLayer != -1) {
        bmpLayerList.emplace_back(timeline->Timing, timeline->BgaLayer);
      }
      std::vector<std::pair<long long, int>> notes;
      if (scheduleNotes) {
        for (auto &note : timeline->Notes) {
          if (isCancelled)
            return;
          if (note == nullptr)
            continue;
          if (note->Wav == bms_parser::Parser::NoWav)
            continue;
          if (!wavTableAbs.contains(note->Wav))
            continue;
          notes.emplace_back(timeline->Timing, note->Wav);
        }
      }
      for (auto &bgNote : timeline->BackgroundNotes) {
        if (isCancelled)
          return;
        if (bgNote->Wav == bms_parser::Parser::NoWav)
          continue;
        if (!wavTableAbs.contains(bgNote->Wav))
          continue;
        notes.emplace_back(timeline->Timing, bgNote->Wav);
      }
      std::sort(notes.begin(), notes.end());
      for (auto &note : notes) {
        if (isCancelled)
          return;
        audioList.push_back(note);
      }
    }
  }
}
void Jukebox::playKeySound(int wav) {
  if (isPlaying && wavTableAbs.contains(wav)) {
    audio.playSound(wavTableAbs[wav].c_str());
  }
}

void Jukebox::play() {
  std::lock_guard<std::mutex> lock(playThreadLock);
  if (playThread.joinable())
    playThread.join();
  audio.startDevice();
  isPlaying = true;
  stopwatch->reset();
  stopwatch->start();
  const bool hasTickCallback = static_cast<bool>(onTickCb);
  const int hz = hasTickCallback ? 500 : 250;
  SDL_Log("Jukebox scheduler tick: %d Hz (onTick: %s)", hz,
          hasTickCallback ? "yes" : "no");

  playThread = std::thread([this, hz] {
#ifdef _WIN32
    // Set thread priority using MMCS for audio playback
    HANDLE taskHandle = nullptr;
    DWORD taskIndex = 0;
    taskHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (taskHandle) {
      // Set thread priority to high
      AvSetMmThreadPriority(taskHandle, AVRT_PRIORITY_CRITICAL);
    }
    timeBeginPeriod(1);
#endif
    using Clock = std::chrono::steady_clock;
    auto prevTimestamp = Clock::now();
    auto interval = std::chrono::microseconds(1000000 / hz);
    auto targetNextFrame = prevTimestamp + interval;
    while (isPlaying) {
      if (!stopwatch->isRunning()) {
        std::this_thread::sleep_for(interval);
        prevTimestamp = Clock::now();
        targetNextFrame = prevTimestamp + interval;
        continue;
      }
      {
        // Keep scheduling state consistent with seek/reset.
        std::lock_guard<std::mutex> lock(seekLock);
        auto positionMicro = stopwatch->elapsedMicros();

        lastPositionMicro = positionMicro;
        if (onTickCb) {
          onTickCb(positionMicro);
        }

        while (audioCursor < audioList.size()) {
          auto &target = audioList[audioCursor];
          if (positionMicro < target.first) {
            break;
          }
          if (const auto wavIt = wavTableAbs.find(target.second);
              wavIt != wavTableAbs.end()) {
            audio.playSound(wavIt->second.c_str());
          }
          audioCursor++;
        }

        while (bmpCursor < bmpList.size()) {
          auto &target = bmpList[bmpCursor];
          if (positionMicro < target.first) {
            break;
          }
          if (const auto videoIt = videoPlayerTable.find(target.second);
              videoIt != videoPlayerTable.end()) {
            auto *videoPlayer = videoIt->second;
            videoPlayer->seek(0);
            videoPlayer->play();
            videoPlayer->viewWidth = rendering::window_width;
            videoPlayer->viewHeight = rendering::window_height;
            videoPlayer->viewId = rendering::bga_view;
            currentBga.store(target.second, std::memory_order_relaxed);
          } else if (imageTable.find(target.second) != imageTable.end()) {
            currentBga.store(target.second, std::memory_order_relaxed);
          }
          bmpCursor++;
        }
        while (bmpLayerCursor < bmpLayerList.size()) {
          auto &target = bmpLayerList[bmpLayerCursor];
          if (positionMicro < target.first) {
            break;
          }
          if (const auto videoIt = videoPlayerTable.find(target.second);
              videoIt != videoPlayerTable.end()) {
            auto *videoPlayer = videoIt->second;
            videoPlayer->seek(0);
            videoPlayer->play();
            videoPlayer->viewWidth = rendering::window_width;
            videoPlayer->viewHeight = rendering::window_height;
            videoPlayer->viewId = rendering::bga_layer_view;
            currentBmpLayer.store(target.second, std::memory_order_relaxed);
          } else if (imageTable.find(target.second) != imageTable.end()) {
            currentBmpLayer.store(target.second, std::memory_order_relaxed);
          }
          bmpLayerCursor++;
        }
      }

      auto currentTimestamp = Clock::now();
      size_t idx =
          performanceAnalytics.loopDeltaIndex.load(std::memory_order_relaxed);
      performanceAnalytics.loopDeltaTimes[idx] =
          std::chrono::duration_cast<std::chrono::microseconds>(
              currentTimestamp - prevTimestamp)
              .count();
      size_t newIdx = (idx + 1) % Jukebox::PerformanceAnalytics::BUFFER_SIZE;
      performanceAnalytics.loopDeltaIndex.store(newIdx,
                                                std::memory_order_relaxed);
      prevTimestamp = currentTimestamp;

      targetNextFrame += interval;
      if (targetNextFrame <= currentTimestamp) {
        targetNextFrame = currentTimestamp + interval;
        continue;
      }
      std::this_thread::sleep_until(targetNextFrame);
    }
#ifdef _WIN32
    // Clean up MMCS handle
    if (taskHandle) {
      AvRevertMmThreadCharacteristics(taskHandle);
    }
    timeEndPeriod(1);
#endif
  });
}
void Jukebox::renderImage(ImageData &image, int viewId) {

  if (!bgfx::isValid(image.texture)) {
    return;
  }
  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};

  bgfx::allocTransientVertexBuffer(&tvb, 4, rendering::PosTexCoord0Vertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&tib, 6);
  auto *vertex = (rendering::PosTexCoord0Vertex *)tvb.data;
  // canvas extension (See "spread canvas" in
  // https://hitkey.nekokan.dyndns.info/cmds.htm#BMPXX-ADJUSTMENT)
  vertex[0].x = rendering::window_width / 2.0f - image.width / 2.0f;
  vertex[0].y = rendering::window_height / 2.0f - 256.0f / 2.0f + image.height;
  vertex[0].z = 0.0f;
  vertex[0].u = 0.0f;
  vertex[0].v = 1.0f;
  vertex[1].x = rendering::window_width / 2.0f + image.width / 2.0f;
  vertex[1].y = rendering::window_height / 2.0f - 256.0f / 2.0f + image.height;
  vertex[1].z = 0.0f;
  vertex[1].u = 1.0f;
  vertex[1].v = 1.0f;
  vertex[2].x = rendering::window_width / 2.0f - image.width / 2.0f;
  vertex[2].y = rendering::window_height / 2.0f - 256.0f / 2.0f;
  vertex[2].z = 0.0f;
  vertex[2].u = 0.0f;
  vertex[2].v = 0.0f;
  vertex[3].x = rendering::window_width / 2.0f + image.width / 2.0f;
  vertex[3].y = rendering::window_height / 2.0f - 256.0f / 2.0f;
  vertex[3].z = 0.0f;
  vertex[3].u = 1.0f;
  vertex[3].v = 0.0f;
  auto *indices = (uint16_t *)tib.data;
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 1;
  indices[4] = 3;
  indices[5] = 2;
  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  bgfx::setTexture(0, s_texColor, image.texture);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                 BGFX_STATE_BLEND_ALPHA);
  bgfx::submit(viewId, rendering::ShaderManager::getInstance().getProgram(
                           "vs_text.bin", viewId == rendering::bga_view
                                              ? "fs_text.bin"
                                              : "fs_bgalayer.bin"));
}

long long Jukebox::getTimeMicros() { return stopwatch->elapsedMicros(); }
void Jukebox::pause() {
  SDL_Log("Pausing");
  stopwatch->pause();
}
void Jukebox::resume() { stopwatch->resume(); }
bool Jukebox::isPaused() { return !stopwatch->isRunning(); }
void Jukebox::stop() {
  currentBga.store(-1, std::memory_order_relaxed);
  currentBmpLayer.store(-1, std::memory_order_relaxed);
  isPlaying = false;
  if (playThread.joinable())
    playThread.join();
  audio.stopSounds();
  for (auto &videoPlayer : videoPlayerTable) {
    videoPlayer.second->stop();
  }
}
void Jukebox::seek(long long micro) {
  /* TODO: should also play audio/video which starts earlier than seek but
      ends later than seek */
  std::lock_guard<std::mutex> lock(seekLock);
  stopwatch->seek(micro);
  audio.stopSounds();
  // move cursors to micro
  audioCursor = 0;
  bmpCursor = 0;
  while (audioCursor < audioList.size() &&
         audioList[audioCursor].first < micro) {
    audioCursor++;
  }
  while (bmpCursor < bmpList.size() && bmpList[bmpCursor].first < micro) {
    bmpCursor++;
  }
}

double Jukebox::getAvgDeltaTime() {
  size_t currentWriteIndex = performanceAnalytics.loopDeltaIndex.load(std::memory_order_acquire);
  while(performanceAnalytics.cursor != currentWriteIndex) {
    auto loopRunTime = performanceAnalytics.loopDeltaTimes[performanceAnalytics.cursor];
    performanceAnalytics.statsSum += loopRunTime;
    performanceAnalytics.statsCount ++;
    if(performanceAnalytics.statsCount >= 100) {
      performanceAnalytics.avgDeltaTime = performanceAnalytics.statsSum / performanceAnalytics.statsCount;
      performanceAnalytics.statsSum = 0;
      performanceAnalytics.statsCount = 0;
    }
    performanceAnalytics.cursor = (performanceAnalytics.cursor + 1) % PerformanceAnalytics::BUFFER_SIZE;
  }

  return performanceAnalytics.avgDeltaTime;

}
