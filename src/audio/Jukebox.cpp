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
#include <algorithm>
#include <limits>
#include <unordered_set>
#ifdef _WIN32
#include <timeapi.h>
#include <windows.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#endif

namespace {
constexpr long long kSchedulerTickMicros = 1000000LL / 8000;
constexpr long long kSchedulerMaxIdleSleepMicros = 250000;
} // namespace

Jukebox::Jukebox(Stopwatch *stopwatch)
    : audio(stopwatch), stopwatch(stopwatch) {
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
}

Jukebox::~Jukebox() {
  isPlaying = false;
  wakeScheduler();
  if (playThread.joinable())
    playThread.join();
  audio.stopSounds();
  audio.unloadSounds();
  clearVisualResources();
}
void Jukebox::render() {
  if (!visualsEnabled.load(std::memory_order_relaxed)) {
    return;
  }
  syncVisualClockToAudio();
  const int bga = currentBga.load(std::memory_order_relaxed);
  if (bga != -1) {
    bool rendered = false;
    {
      std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
      auto videoIt = videoPlayerTable.find(bga);
      if (videoIt != videoPlayerTable.end()) {
        auto *videoPlayer = videoIt->second;
        videoPlayer->update();
        const auto rect = calculateBgaRect(videoPlayer->getFrameWidth(),
                                           videoPlayer->getFrameHeight());
        videoPlayer->viewX = rect.x;
        videoPlayer->viewY = rect.y;
        videoPlayer->viewWidth = rect.width;
        videoPlayer->viewHeight = rect.height;
        videoPlayer->viewId = rendering::bga_view;
        videoPlayer->render();
        rendered = true;
      }
    }
    if (!rendered) {
      std::lock_guard<std::mutex> lock(imageTableMutex);
      auto imageIt = imageTable.find(bga);
      if (imageIt != imageTable.end()) {
        renderImage(imageIt->second, rendering::bga_view);
      }
    }
  }
  const int bmpLayer = currentBmpLayer.load(std::memory_order_relaxed);
  if (bmpLayer != -1) {
    bool rendered = false;
    {
      std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
      auto videoIt = videoPlayerTable.find(bmpLayer);
      if (videoIt != videoPlayerTable.end()) {
        auto *videoPlayer = videoIt->second;
        videoPlayer->update();
        const auto rect = calculateBgaRect(videoPlayer->getFrameWidth(),
                                           videoPlayer->getFrameHeight());
        videoPlayer->viewX = rect.x;
        videoPlayer->viewY = rect.y;
        videoPlayer->viewWidth = rect.width;
        videoPlayer->viewHeight = rect.height;
        videoPlayer->viewId = rendering::bga_layer_view;
        videoPlayer->render();
        rendered = true;
      }
    }
    if (!rendered) {
      std::lock_guard<std::mutex> lock(imageTableMutex);
      auto imageIt = imageTable.find(bmpLayer);
      if (imageIt != imageTable.end()) {
        renderImage(imageIt->second, rendering::bga_layer_view);
      }
    }
  }
}

bool Jukebox::hasActiveVisuals() const {
  return visualsEnabled.load(std::memory_order_relaxed) &&
         (currentBga.load(std::memory_order_relaxed) != -1 ||
          currentBmpLayer.load(std::memory_order_relaxed) != -1);
}

void Jukebox::setVisualsEnabled(bool enabled) {
  visualsEnabled.store(enabled, std::memory_order_relaxed);
  wakeScheduler();
  if (enabled) {
    return;
  }

  currentBga.store(-1, std::memory_order_relaxed);
  currentBmpLayer.store(-1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
  for (auto &videoPlayer : videoPlayerTable) {
    videoPlayer.second->stop();
  }
}

bool Jukebox::getVisualsEnabled() const {
  return visualsEnabled.load(std::memory_order_relaxed);
}

void Jukebox::setBgaOffsetMs(int offsetMs) {
  const int previous = bgaOffsetMs.exchange(offsetMs, std::memory_order_relaxed);
  if (previous != offsetMs) {
    wakeScheduler();
  }
}

void Jukebox::setBgaDisplayMode(AppSettings::BgaDisplayMode mode) {
  bgaDisplayMode.store(static_cast<int>(mode), std::memory_order_relaxed);
}

Jukebox::BgaRect Jukebox::calculateBgaRect(int sourceWidth,
                                           int sourceHeight) const {
  const float targetWidth = static_cast<float>(rendering::window_width);
  const float targetHeight = static_cast<float>(rendering::window_height);
  if (targetWidth <= 0.0f || targetHeight <= 0.0f) {
    return {};
  }

  const auto mode = static_cast<AppSettings::BgaDisplayMode>(
      bgaDisplayMode.load(std::memory_order_relaxed));
  if (mode == AppSettings::BgaDisplayMode::Stretch || sourceWidth <= 0 ||
      sourceHeight <= 0) {
    return {0.0f, 0.0f, targetWidth, targetHeight};
  }

  const float scaleX = targetWidth / static_cast<float>(sourceWidth);
  const float scaleY = targetHeight / static_cast<float>(sourceHeight);
  const float scale = mode == AppSettings::BgaDisplayMode::Fill
                          ? std::max(scaleX, scaleY)
                          : std::min(scaleX, scaleY);
  const float width = static_cast<float>(sourceWidth) * scale;
  const float height = static_cast<float>(sourceHeight) * scale;
  return {(targetWidth - width) * 0.5f, (targetHeight - height) * 0.5f, width,
          height};
}

void Jukebox::loadSounds(bms_parser::Chart &chart,
                         std::atomic_bool &isCancelled) {
  std::mutex wavTableLock;
  std::mutex loadedPathsLock;
  std::unordered_set<path_t> loadedPaths;

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

        const path_t soundPath = fspath_to_path_t(path);
        bool needsLoad = true;
        {
          std::lock_guard<std::mutex> lock(loadedPathsLock);
          needsLoad = !loadedPaths.contains(soundPath);
        }

        if (needsLoad && !audio.loadSound(soundPath, isCancelled)) {
          continue;
        }

        if (needsLoad) {
          std::lock_guard<std::mutex> lock(loadedPathsLock);
          loadedPaths.insert(soundPath);
          SDL_Log("Loaded sound %d: %s", wav->first,
                  path_t_to_utf8(soundPath).c_str());
        }

        {
          std::lock_guard<std::mutex> lock(wavTableLock);
          auto idx = wav->first;
          wavTableAbs[idx] = soundPath;
        }

        found = true;
        break;
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

void Jukebox::clearVisualResources() {
  currentBga.store(-1, std::memory_order_relaxed);
  currentBmpLayer.store(-1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    for (auto &videoPlayer : videoPlayerTable) {
      delete videoPlayer.second;
    }
    videoPlayerTable.clear();
  }
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    for (auto &image : imageTable) {
      bgfx::destroy(image.second.texture);
    }
    imageTable.clear();
  }
}

void Jukebox::scheduleVisuals(bms_parser::Chart &chart,
                              std::atomic_bool &isCancelled) {
  bmpCursor = 0;
  bmpLayerCursor = 0;
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
    }
  }
}

void Jukebox::loadVisuals(bms_parser::Chart &chart,
                          std::atomic_bool &isCancelled) {
  isPlaying = false;
  if (playThread.joinable()) {
    playThread.join();
  }
  clearVisualResources();
  scheduleVisuals(chart, isCancelled);
  if (isCancelled || !visualsEnabled.load(std::memory_order_relaxed)) {
    return;
  }
  loadBMPs(chart, isCancelled);
}

void Jukebox::unloadVisuals() { clearVisualResources(); }

void Jukebox::loadChart(bms_parser::Chart &chart, bool scheduleNotes,
                        std::atomic_bool &isCancelled) {
  isPlaying = false;
  if (playThread.joinable()) {
    SDL_Log("Joining playThread");
    playThread.join();
  }

  audio.stopSounds();
  audio.unloadSounds();

  clearVisualResources();
  if (isCancelled)
    return;
  SDL_Log("Loading sounds");
  std::thread loadSoundThread(
      [this, &chart, &isCancelled] { loadSounds(chart, isCancelled); });
  if (visualsEnabled.load(std::memory_order_relaxed)) {
    SDL_Log("Loading videos");
    loadBMPs(chart, isCancelled);
  }
  loadSoundThread.join();

  if (isCancelled)
    return;
  schedule(chart, scheduleNotes, isCancelled);
  SDL_Log("Chart loaded");
}

void Jukebox::schedule(bms_parser::Chart &chart, bool scheduleNotes,
                       std::atomic_bool &isCancelled) {
  audioCursor = 0;
  audioList.clear();
  scheduleVisuals(chart, isCancelled);
  for (auto &measure : chart.Measures) {
    if (isCancelled)
      return;
    for (auto &timeline : measure->TimeLines) {
      if (isCancelled)
        return;
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
  std::sort(audioList.begin(), audioList.end());
}
void Jukebox::playKeySound(int wav) {
  if (!isPlaying) {
    return;
  }
  if (const auto it = wavTableAbs.find(wav); it != wavTableAbs.end()) {
    audio.playSound(it->second.c_str());
  }
}

void Jukebox::scheduleAudioFromCursor() {
  while (audioCursor < audioList.size()) {
    const auto &target = audioList[audioCursor];
    if (const auto it = wavTableAbs.find(target.second);
        it != wavTableAbs.end()) {
      audio.scheduleSound(it->second, target.first);
    }
    audioCursor++;
  }
}

void Jukebox::wakeScheduler() { schedulerWakeCv.notify_all(); }

void Jukebox::syncVisualClockToAudio() {
  if (isPlaying.load(std::memory_order_relaxed) && stopwatch->isRunning()) {
    stopwatch->seek(getBgaTimelineMicros(audio.getTimeMicros()));
  }
}

long long Jukebox::getBgaOffsetMicros() const {
  return static_cast<long long>(bgaOffsetMs.load(std::memory_order_relaxed)) *
         1000LL;
}

long long Jukebox::getBgaTimelineMicros(long long rawSongMicros) const {
  return rawSongMicros + getBgaOffsetMicros();
}

long long
Jukebox::getRawSongMicrosForBgaTarget(long long bgaTargetMicros) const {
  return bgaTargetMicros - getBgaOffsetMicros();
}

bool Jukebox::activateVisual(int visualId, bgfx::ViewId viewId) {
  {
    std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
    auto videoIt = videoPlayerTable.find(visualId);
    if (videoIt != videoPlayerTable.end()) {
      auto *videoPlayer = videoIt->second;
      videoPlayer->seek(0);
      videoPlayer->play();
      videoPlayer->viewWidth = rendering::window_width;
      videoPlayer->viewHeight = rendering::window_height;
      videoPlayer->viewId = viewId;
      return true;
    }
  }
  {
    std::lock_guard<std::mutex> lock(imageTableMutex);
    if (imageTable.find(visualId) != imageTable.end()) {
      return true;
    }
  }
  return false;
}

void Jukebox::renderVisualsAt(long long micro) {
  if (!visualsEnabled.load(std::memory_order_relaxed)) {
    return;
  }

  std::lock_guard<std::mutex> lock(seekLock);
  stopwatch->seek(micro);
  while (bmpCursor < bmpList.size()) {
    const auto &target = bmpList[bmpCursor];
    if (micro < target.first) {
      break;
    }
    if (activateVisual(target.second, rendering::bga_view)) {
      currentBga.store(target.second, std::memory_order_relaxed);
    }
    bmpCursor++;
  }
  while (bmpLayerCursor < bmpLayerList.size()) {
    const auto &target = bmpLayerList[bmpLayerCursor];
    if (micro < target.first) {
      break;
    }
    if (activateVisual(target.second, rendering::bga_layer_view)) {
      currentBmpLayer.store(target.second, std::memory_order_relaxed);
    }
    bmpLayerCursor++;
  }
  render();
}

void Jukebox::play() {
  std::lock_guard<std::mutex> lock(playThreadLock);
  if (playThread.joinable())
    playThread.join();
  audio.stopSounds();
  isPlaying = true;
  stopwatch->reset();
  audio.seekClock(0);
  {
    std::lock_guard<std::mutex> lock(seekLock);
    audioCursor = 0;
    scheduleAudioFromCursor();
  }
  audio.startDevice();
  stopwatch->start();
  wakeScheduler();
  SDL_Log("Jukebox visual scheduler is event-driven");

  playThread = std::thread([this] {
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
    while (isPlaying) {
      if (!stopwatch->isRunning()) {
        std::unique_lock<std::mutex> waitLock(schedulerWaitMutex);
        schedulerWakeCv.wait_for(
            waitLock,
            std::chrono::microseconds(kSchedulerMaxIdleSleepMicros));
        prevTimestamp = Clock::now();
        continue;
      }

      long long nextWakeMicros = std::numeric_limits<long long>::max();
      {
        // Keep scheduling state consistent with seek/reset.
        std::lock_guard<std::mutex> lock(seekLock);
        const long long positionMicro = audio.getTimeMicros();
        const long long bgaPositionMicro = getBgaTimelineMicros(positionMicro);
        stopwatch->seek(bgaPositionMicro);
        auto scheduleNextWake = [&](long long targetMicros) {
          nextWakeMicros =
              std::min(nextWakeMicros, std::max(targetMicros, positionMicro));
        };

        if (onTickCb) {
          onTickCb(positionMicro);
          scheduleNextWake(positionMicro + kSchedulerTickMicros);
        }

        while (bmpCursor < bmpList.size()) {
          auto &target = bmpList[bmpCursor];
          if (bgaPositionMicro < target.first) {
            break;
          }
          if (!visualsEnabled.load(std::memory_order_relaxed)) {
            bmpCursor++;
            continue;
          }
          bool activated = false;
          {
            std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
            auto videoIt = videoPlayerTable.find(target.second);
            if (videoIt != videoPlayerTable.end()) {
              auto *videoPlayer = videoIt->second;
              videoPlayer->seek(0);
              videoPlayer->play();
              videoPlayer->viewWidth = rendering::window_width;
              videoPlayer->viewHeight = rendering::window_height;
              videoPlayer->viewId = rendering::bga_view;
              activated = true;
            }
          }
          if (!activated) {
            std::lock_guard<std::mutex> lock(imageTableMutex);
            if (imageTable.find(target.second) != imageTable.end()) {
              activated = true;
            }
          }
          if (activated) {
            currentBga.store(target.second, std::memory_order_relaxed);
          }
          bmpCursor++;
        }
        if (bmpCursor < bmpList.size()) {
          scheduleNextWake(
              getRawSongMicrosForBgaTarget(bmpList[bmpCursor].first));
        }
        while (bmpLayerCursor < bmpLayerList.size()) {
          auto &target = bmpLayerList[bmpLayerCursor];
          if (bgaPositionMicro < target.first) {
            break;
          }
          if (!visualsEnabled.load(std::memory_order_relaxed)) {
            bmpLayerCursor++;
            continue;
          }
          bool activated = false;
          {
            std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
            auto videoIt = videoPlayerTable.find(target.second);
            if (videoIt != videoPlayerTable.end()) {
              auto *videoPlayer = videoIt->second;
              videoPlayer->seek(0);
              videoPlayer->play();
              videoPlayer->viewWidth = rendering::window_width;
              videoPlayer->viewHeight = rendering::window_height;
              videoPlayer->viewId = rendering::bga_layer_view;
              activated = true;
            }
          }
          if (!activated) {
            std::lock_guard<std::mutex> lock(imageTableMutex);
            if (imageTable.find(target.second) != imageTable.end()) {
              activated = true;
            }
          }
          if (activated) {
            currentBmpLayer.store(target.second, std::memory_order_relaxed);
          }
          bmpLayerCursor++;
        }
        if (bmpLayerCursor < bmpLayerList.size()) {
          scheduleNextWake(getRawSongMicrosForBgaTarget(
              bmpLayerList[bmpLayerCursor].first));
        }
      }

      auto currentTimestamp = Clock::now();
      size_t idx =
          performanceAnalytics.loopDeltaIndex.load(std::memory_order_relaxed);
      const auto deltaMicros =
          std::chrono::duration_cast<std::chrono::microseconds>(
              currentTimestamp - prevTimestamp)
              .count();
      performanceAnalytics.loopDeltaTimes[idx].store(
          static_cast<uint32_t>(deltaMicros), std::memory_order_relaxed);
      size_t newIdx = (idx + 1) % Jukebox::PerformanceAnalytics::BUFFER_SIZE;
      performanceAnalytics.loopDeltaIndex.store(newIdx,
                                                std::memory_order_relaxed);
      prevTimestamp = currentTimestamp;

      long long sleepMicros = kSchedulerMaxIdleSleepMicros;
      if (nextWakeMicros != std::numeric_limits<long long>::max()) {
        const long long currentSongMicros = audio.getTimeMicros();
        const long long untilNextMicros = nextWakeMicros - currentSongMicros;
        if (untilNextMicros <= 0) {
          std::this_thread::yield();
          continue;
        }
        sleepMicros = std::min(kSchedulerMaxIdleSleepMicros, untilNextMicros);
      }
      std::unique_lock<std::mutex> waitLock(schedulerWaitMutex);
      schedulerWakeCv.wait_for(waitLock,
                               std::chrono::microseconds(sleepMicros));
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

  bgfx::allocTransientVertexBuffer(&tvb, 4,
                                   rendering::PosTexCoord0Vertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&tib, 6);
  auto *vertex = (rendering::PosTexCoord0Vertex *)tvb.data;
  const auto rect = calculateBgaRect(image.width, image.height);
  vertex[0].x = rect.x;
  vertex[0].y = rect.y + rect.height;
  vertex[0].z = 0.0f;
  vertex[0].u = 0.0f;
  vertex[0].v = 1.0f;
  vertex[1].x = rect.x + rect.width;
  vertex[1].y = rect.y + rect.height;
  vertex[1].z = 0.0f;
  vertex[1].u = 1.0f;
  vertex[1].v = 1.0f;
  vertex[2].x = rect.x;
  vertex[2].y = rect.y;
  vertex[2].z = 0.0f;
  vertex[2].u = 0.0f;
  vertex[2].v = 0.0f;
  vertex[3].x = rect.x + rect.width;
  vertex[3].y = rect.y;
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
  static const bgfx::ProgramHandle kBgaProgram =
      rendering::ShaderManager::getInstance().getProgram("vs_text.bin",
                                                         "fs_text.bin");
  static const bgfx::ProgramHandle kBgaLayerProgram =
      rendering::ShaderManager::getInstance().getProgram("vs_text.bin",
                                                         "fs_bgalayer.bin");
  bgfx::submit(viewId,
               viewId == rendering::bga_view ? kBgaProgram : kBgaLayerProgram);
}

long long Jukebox::getTimeMicros() {
  if (isPlaying.load(std::memory_order_relaxed)) {
    return audio.getTimeMicros();
  }
  return stopwatch->elapsedMicros();
}
void Jukebox::pause() {
  SDL_Log("Pausing");
  audio.seekClock(audio.getTimeMicros());
  stopwatch->pause();
  wakeScheduler();
}
void Jukebox::resume() {
  audio.seekClock(audio.getTimeMicros());
  stopwatch->resume();
  wakeScheduler();
}
bool Jukebox::isPaused() { return !stopwatch->isRunning(); }
void Jukebox::stop() {
  currentBga.store(-1, std::memory_order_relaxed);
  currentBmpLayer.store(-1, std::memory_order_relaxed);
  isPlaying = false;
  wakeScheduler();
  if (playThread.joinable())
    playThread.join();
  audio.stopSounds();
  std::lock_guard<std::mutex> lock(videoPlayerTableMutex);
  for (auto &videoPlayer : videoPlayerTable) {
    videoPlayer.second->stop();
  }
}
void Jukebox::seek(long long micro) {
  /* TODO: should also play audio/video which starts earlier than seek but
      ends later than seek */
  std::lock_guard<std::mutex> lock(seekLock);
  const long long bgaTimelineMicro = getBgaTimelineMicros(micro);
  stopwatch->seek(bgaTimelineMicro);
  audio.stopSounds();
  audio.seekClock(micro);
  // move cursors to micro
  audioCursor = 0;
  bmpCursor = 0;
  bmpLayerCursor = 0;
  while (audioCursor < audioList.size() &&
         audioList[audioCursor].first < micro) {
    audioCursor++;
  }
  scheduleAudioFromCursor();
  if (isPlaying) {
    audio.startDevice();
  }
  wakeScheduler();
  while (bmpCursor < bmpList.size() &&
         bmpList[bmpCursor].first < bgaTimelineMicro) {
    bmpCursor++;
  }
  while (bmpLayerCursor < bmpLayerList.size() &&
         bmpLayerList[bmpLayerCursor].first < bgaTimelineMicro) {
    bmpLayerCursor++;
  }
}

double Jukebox::getAvgDeltaTime() {
  size_t currentWriteIndex =
      performanceAnalytics.loopDeltaIndex.load(std::memory_order_acquire);
  while (performanceAnalytics.cursor != currentWriteIndex) {
    const auto loopRunTime = static_cast<double>(
        performanceAnalytics.loopDeltaTimes[performanceAnalytics.cursor].load(
            std::memory_order_relaxed));
    performanceAnalytics.statsSum += loopRunTime;
    performanceAnalytics.statsCount++;
    if (performanceAnalytics.statsCount >= 100) {
      performanceAnalytics.avgDeltaTime =
          performanceAnalytics.statsSum / performanceAnalytics.statsCount;
      performanceAnalytics.statsSum = 0;
      performanceAnalytics.statsCount = 0;
    }
    performanceAnalytics.cursor =
        (performanceAnalytics.cursor + 1) % PerformanceAnalytics::BUFFER_SIZE;
  }

  return performanceAnalytics.avgDeltaTime;
}
