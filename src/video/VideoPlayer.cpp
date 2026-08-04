#include "VideoPlayer.h"
#include "VideoDecodeState.h"
#include "VideoFrameLayout.h"
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/UniformCache.h"
#include <algorithm>
#include <cstring>
#include <inttypes.h>

#include <thread>

namespace {

struct EmbeddedYuvVertex {
  float x;
  float y;
  float z;
  float u;
  float v;
  std::uint32_t abgr;
};

const bgfx::VertexLayout &embeddedYuvVertexLayout() {
  static const bgfx::VertexLayout layout = [] {
    bgfx::VertexLayout value;
    value.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
        .end();
    return value;
  }();
  return layout;
}

std::uint8_t colorByte(float value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F);
}

std::uint32_t packAbgr(const video::EmbeddedYuvQuadVertex &vertex) {
  const auto red = static_cast<std::uint32_t>(colorByte(vertex.r));
  const auto green = static_cast<std::uint32_t>(colorByte(vertex.g));
  const auto blue = static_cast<std::uint32_t>(colorByte(vertex.b));
  const auto alpha = static_cast<std::uint32_t>(colorByte(vertex.a));
  return (alpha << 24U) | (blue << 16U) | (green << 8U) | red;
}

} // namespace

const bgfx::VertexLayout &VideoPlayer::embeddedVertexLayout() noexcept {
  return embeddedYuvVertexLayout();
}

VideoPlayer::VideoPlayer(Stopwatch *stopwatch)
    : stopwatch(stopwatch), videoFrameWidth(0), videoFrameHeight(0),
      hasVideoFrame(false) {

  frameBuffer.resize(maxBufferSize, nullptr);
  auto &uniforms = rendering::UniformCache::getInstance();
  s_texY = uniforms.getSampler("s_texY");
  s_texU = uniforms.getSampler("s_texU");
  s_texV = uniforms.getSampler("s_texV");
  try {
    fullscreenProgram =
        rendering::ShaderManager::getInstance().getProgram(SHADER_YUVRGB);
  } catch (...) {
    fullscreenProgram = BGFX_INVALID_HANDLE;
  }
  try {
    embeddedProgram = rendering::ShaderManager::getInstance().getProgram(
        "vs_skin_yuvrgb.bin", "fs_skin_yuvrgb.bin");
  } catch (...) {
    embeddedProgram = BGFX_INVALID_HANDLE;
  }
}

VideoPlayer::~VideoPlayer() { unloadVideo(); }

void VideoPlayer::destroyVideoTextures() {
  std::lock_guard<std::mutex> lock(videoFrameMutex);
  if (bgfx::isValid(videoTextureY)) {
    bgfx::destroy(videoTextureY);
    videoTextureY = BGFX_INVALID_HANDLE;
  }
  if (bgfx::isValid(videoTextureU)) {
    bgfx::destroy(videoTextureU);
    videoTextureU = BGFX_INVALID_HANDLE;
  }
  if (bgfx::isValid(videoTextureV)) {
    bgfx::destroy(videoTextureV);
    videoTextureV = BGFX_INVALID_HANDLE;
  }
  videoFrameWidth = 0;
  videoFrameHeight = 0;
  hasVideoFrame = false;
}

void VideoPlayer::unloadVideo() {
  stopPredecoding();
  hasVideoFrame = false;
  {
    std::lock_guard<std::mutex> videoLock(videoMutex);
    if (swsContext) {
      sws_freeContext(swsContext);
      swsContext = nullptr;
    }
    if (formatContext) {
      avformat_close_input(&formatContext);
      formatContext = nullptr;
    }
    if (codecContext) {
      avcodec_free_context(&codecContext);
      codecContext = nullptr;
    }
    videoStreamIndex = -1;
  }
  destroyVideoTextures();
  {
    std::lock_guard<std::mutex> lock(recycleMutex);
    for (auto *f : recyclePool) {
      av_frame_free(&f);
    }
    recyclePool.clear();
  }
}

bool VideoPlayer::loadVideo(const std::string &videoPath,
                            std::atomic<bool> &isCancelled) {
  unloadVideo();
  if (isCancelled.load(std::memory_order_relaxed)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> videoLock(videoMutex);
    AVFormatContext *tempFormatContext = avformat_alloc_context();
    if (tempFormatContext == nullptr) {
      return false;
    }
    auto fail = [&]() {
      if (swsContext != nullptr) {
        sws_freeContext(swsContext);
        swsContext = nullptr;
      }
      if (codecContext != nullptr) {
        avcodec_free_context(&codecContext);
      }
      if (formatContext != nullptr) {
        avformat_close_input(&formatContext);
      } else if (tempFormatContext != nullptr) {
        avformat_close_input(&tempFormatContext);
      }
      destroyVideoTextures();
      videoStreamIndex = -1;
      return false;
    };
    // genpts
    tempFormatContext->flags |= AVFMT_FLAG_GENPTS | AVFMT_FLAG_SORT_DTS;
    if (avformat_open_input(&tempFormatContext, videoPath.c_str(), nullptr,
                            nullptr) < 0) {
      return fail();
    }
    if (avformat_find_stream_info(tempFormatContext, nullptr) < 0) {
      return fail();
    }

    for (unsigned i = 0; i < tempFormatContext->nb_streams; i++) {
      if (tempFormatContext->streams[i]->codecpar->codec_type ==
          AVMEDIA_TYPE_VIDEO) {
        videoStreamIndex = i;
        break;
      }
    }

    if (videoStreamIndex == -1) {
      return fail();
    }
    auto videoStream = tempFormatContext->streams[videoStreamIndex];
    const AVCodec *codec =
        avcodec_find_decoder(videoStream->codecpar->codec_id);
    if (!codec) {
      return fail();
    }
    codecContext = avcodec_alloc_context3(codec);
    if (codecContext == nullptr ||
        avcodec_parameters_to_context(codecContext, videoStream->codecpar) <
            0) {
      return fail();
    }

    // Fix missing extradata (SPS/PPS)
    if (!codecContext->extradata || codecContext->extradata_size <= 0) {
      SDL_Log("Fixing missing SPS/PPS extradata");
      AVCodecParameters *codecParams = videoStream->codecpar;
      if (codecParams->extradata_size > 0 && codecParams->extradata) {
        auto *extraData = static_cast<uint8_t *>(av_mallocz(
            codecParams->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (extraData == nullptr) {
          return fail();
        }
        memcpy(extraData, codecParams->extradata, codecParams->extradata_size);
        codecContext->extradata = extraData;
        codecContext->extradata_size = codecParams->extradata_size;
      }
    }
    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
      return fail();
    }
    startPTS = videoStream->start_time;
    if (startPTS == AV_NOPTS_VALUE) {
      startPTS = 0; // Default to 0 if start_time is not available
    }
    swsContext = sws_getContext(codecContext->width, codecContext->height,
                                codecContext->pix_fmt, codecContext->width,
                                codecContext->height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (swsContext == nullptr) {
      return fail();
    }
    // auto num = videoStream->avg_frame_rate.num;
    // auto den = videoStream->avg_frame_rate.den;
    // if (den == 0) {
    //   SDL_Log("Warning: video stream has zero frame rate");
    // } else {
    //   fps = static_cast<float>(num) / static_cast<float>(den);
    // }

    if (!updateVideoTexture(codecContext->width, codecContext->height)) {
      SDL_Log("Unsupported YUV420 video dimensions: %d x %d",
              codecContext->width, codecContext->height);
      return fail();
    }

    formatContext = tempFormatContext;
    tempFormatContext = nullptr;
    if (isCancelled.load(std::memory_order_relaxed)) {
      return fail();
    }
    isEOF = false;
    predecodingActive = true;
    try {
      predecodeThread = std::thread(&VideoPlayer::predecodeFrames, this);
    } catch (...) {
      predecodingActive = false;
      return fail();
    }
    return true;
  }
}

long long VideoPlayer::getDurationMicros() const {
  std::lock_guard<std::mutex> videoLock(videoMutex);
  if (formatContext == nullptr || videoStreamIndex < 0 ||
      static_cast<unsigned>(videoStreamIndex) >= formatContext->nb_streams) {
    return 0;
  }

  const AVStream *videoStream = formatContext->streams[videoStreamIndex];
  if (videoStream != nullptr && videoStream->duration != AV_NOPTS_VALUE &&
      videoStream->duration > 0) {
    return std::max<long long>(
        0, av_rescale_q(videoStream->duration, videoStream->time_base,
                        AVRational{1, AV_TIME_BASE}));
  }

  if (formatContext->duration != AV_NOPTS_VALUE &&
      formatContext->duration > 0) {
    return static_cast<long long>(formatContext->duration);
  }

  return 0;
}

void VideoPlayer::update() {
  if (!isPlaying)
    return;
  if (decodeSuspended.load(std::memory_order_acquire)) {
    return;
  }
  if (formatContext == nullptr || videoStreamIndex < 0) {
    return;
  }
  if (bufferSize.load(std::memory_order_acquire) == 0) {
    return;
  }

  AVFrame *currentFrame;
  double elapsedTime;
  double frameTime;
  // Determine the next frame to display
  // ... (existing buffering logic remains, but we get the frame from
  // frameBuffer) ...

  // Note: We need to handle frame skipping logic properly with the new pool
  // The existing loop:
  while (true) {
    {
      std::unique_lock<std::mutex> lock(bufferMutex);
      if (bufferSize == 0) {
        return;
      }
      currentFrame = frameBuffer[bufferHead];
      long long now = stopwatch->elapsedMicros();
      elapsedTime = (now - startTime) / 1000000.0;
      frameTime = (currentFrame->pts - startPTS) *
                  av_q2d(formatContext->streams[videoStreamIndex]->time_base);

      if (elapsedTime < frameTime) {
        // Not time yet
        return;
      }

      frameBuffer[bufferHead] = nullptr; // Clear buffer slot
      bufferHead = (bufferHead + 1) % maxBufferSize;
      --bufferSize;
    }

    // We consumed a frame from the buffer
    freeSpace.notify_one();

    if (elapsedTime > frameTime + 0.1) {
      lastFramePTS = frameTime;
      // Recycle the skipped frame
      recycleFrame(currentFrame);
      continue;
    }
    break;
  }

  // Upload to BGFX textures.
  // Use bgfx::copy + explicit pitch because decoder output can be padded
  // (linesize > plane width), and the frame is recycled right after upload.
  const auto layout = video::makeYuv420FrameLayout(
      videoFrameWidth, videoFrameHeight, currentFrame->linesize[0],
      currentFrame->linesize[1], currentFrame->linesize[2]);
  if (!layout || currentFrame->data[0] == nullptr ||
      currentFrame->data[1] == nullptr || currentFrame->data[2] == nullptr) {
    SDL_Log("Rejected invalid decoded YUV420 frame layout");
    recycleFrame(currentFrame);
    return;
  }

  bgfx::updateTexture2D(
      videoTextureY, 0, 0, 0, 0, layout->width, layout->height,
      bgfx::copy(currentFrame->data[0], layout->yBytes), layout->yPitch);
  bgfx::updateTexture2D(
      videoTextureU, 0, 0, 0, 0, layout->chromaWidth,
      layout->chromaHeight,
      bgfx::copy(currentFrame->data[1], layout->uBytes), layout->uPitch);
  bgfx::updateTexture2D(
      videoTextureV, 0, 0, 0, 0, layout->chromaWidth,
      layout->chromaHeight,
      bgfx::copy(currentFrame->data[2], layout->vBytes), layout->vPitch);

  lastFramePTS = frameTime;
  hasVideoFrame = true;

  // Recycle the displayed frame
  recycleFrame(currentFrame);
}
unsigned int VideoPlayer::getPrecisePosition() {
  // calculate the frame position in microseconds
  return static_cast<unsigned int>(lastFramePTS * 1000000);
}

void VideoPlayer::render(bgfx::ViewId viewId, float viewX, float viewY,
                         float viewWidth, float viewHeight) const {
  if (!hasVideoFrame)
    return;
  if (!isPlaying) {
    return;
  }
  if (!bgfx::isValid(fullscreenProgram)) {
    return;
  }

  // Submit a quad with the video texture
  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};

  //  SDL_Log("Rendering video texture frame %d; time: %f", currentFrame,
  //  currentFrame / 30.0f);

  bgfx::VertexLayout &layout = rendering::PosTexCoord0Vertex::ms_decl;
  bgfx::allocTransientVertexBuffer(&tvb, 4, layout);
  bgfx::allocTransientIndexBuffer(&tib, 6);
  auto *vertex = (rendering::PosTexCoord0Vertex *)tvb.data;

  // Define quad vertices
  vertex[0].x = viewX;
  vertex[0].y = viewY + viewHeight;
  vertex[0].z = 0.0f;
  vertex[0].u = 0.0f;
  vertex[0].v = 1.0f;

  vertex[1].x = viewX + viewWidth;
  vertex[1].y = viewY + viewHeight;
  vertex[1].z = 0.0f;
  vertex[1].u = 1.0f;
  vertex[1].v = 1.0f;

  vertex[2].x = viewX;
  vertex[2].y = viewY;
  vertex[2].z = 0.0f;
  vertex[2].u = 0.0f;
  vertex[2].v = 0.0f;

  vertex[3].x = viewX + viewWidth;
  vertex[3].y = viewY;
  vertex[3].z = 0.0f;
  vertex[3].u = 1.0f;
  vertex[3].v = 0.0f;

  // Define quad indices
  auto *indices = (uint16_t *)tib.data;
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 1;
  indices[4] = 3;
  indices[5] = 2;
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                 BGFX_STATE_BLEND_ALPHA);
  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);
  bgfx::setTexture(0, s_texY, videoTextureY);
  bgfx::setTexture(1, s_texU, videoTextureU);
  bgfx::setTexture(2, s_texV, videoTextureV);

  bgfx::submit(viewId, fullscreenProgram);
}

void VideoPlayer::renderEmbedded(
    bgfx::ViewId viewId, const video::EmbeddedYuvQuadLayout &quad,
    std::uint64_t state,
    std::optional<rendering::DrawableScissor> scissor) const {
  const auto submission = prepareEmbeddedSubmission(quad, state, scissor);
  if (submission) {
    auto committed = *submission;
    if (bgfx::getAvailTransientVertexBuffer(
            static_cast<std::uint32_t>(committed.quad.vertices.size()),
            embeddedYuvVertexLayout()) >= committed.quad.vertices.size() &&
        bgfx::getAvailTransientIndexBuffer(
            static_cast<std::uint32_t>(committed.quad.indices.size())) >=
            committed.quad.indices.size()) {
      commitPreparedEmbedded(committed);
      submitPreparedEmbedded(viewId, committed);
    }
  }
}

std::optional<VideoPlayer::PreparedEmbeddedSubmission>
VideoPlayer::prepareEmbeddedSubmission(
    const video::EmbeddedYuvQuadLayout &quad, std::uint64_t state,
    std::optional<rendering::DrawableScissor> scissor) const {
  std::lock_guard<std::mutex> frameLock(videoFrameMutex);
  if (!hasVideoFrame || !isPlaying || !bgfx::isValid(embeddedProgram) ||
      !bgfx::isValid(videoTextureY) || !bgfx::isValid(videoTextureU) ||
      !bgfx::isValid(videoTextureV) || !bgfx::isValid(s_texY) ||
      !bgfx::isValid(s_texU) || !bgfx::isValid(s_texV)) {
    return std::nullopt;
  }
  return PreparedEmbeddedSubmission{
      .quad = quad,
      .state = state,
      .scissor = scissor,
      .program = embeddedProgram,
      .textures = {videoTextureY, videoTextureU, videoTextureV},
      .samplers = {s_texY, s_texU, s_texV}};
}

void VideoPlayer::commitPreparedEmbedded(
    PreparedEmbeddedSubmission &submission) const noexcept {
  bgfx::allocTransientVertexBuffer(
      &submission.vertexBuffer,
      static_cast<std::uint32_t>(submission.quad.vertices.size()),
      embeddedYuvVertexLayout());
  bgfx::allocTransientIndexBuffer(
      &submission.indexBuffer,
      static_cast<std::uint32_t>(submission.quad.indices.size()));
  auto *vertices =
      reinterpret_cast<EmbeddedYuvVertex *>(submission.vertexBuffer.data);
  for (std::size_t index = 0; index < submission.quad.vertices.size();
       ++index) {
    const auto &source = submission.quad.vertices[index];
    vertices[index] = {.x = source.x,
                       .y = source.y,
                       .z = 0.0F,
                       .u = source.u,
                       .v = source.v,
                       .abgr = packAbgr(source)};
  }
  std::memcpy(submission.indexBuffer.data, submission.quad.indices.data(),
              submission.quad.indices.size() *
                  sizeof(submission.quad.indices.front()));
}

void VideoPlayer::submitPreparedEmbedded(
    bgfx::ViewId viewId,
    const PreparedEmbeddedSubmission &submission) const noexcept {
  bgfx::setState(submission.state);
  if (submission.scissor && submission.scissor->enabled) {
    bgfx::setScissor(static_cast<std::uint16_t>(submission.scissor->x),
                     static_cast<std::uint16_t>(submission.scissor->y),
                     static_cast<std::uint16_t>(submission.scissor->width),
                     static_cast<std::uint16_t>(submission.scissor->height));
  } else {
    bgfx::setScissor();
  }
  bgfx::setVertexBuffer(0, &submission.vertexBuffer);
  bgfx::setIndexBuffer(&submission.indexBuffer);
  constexpr std::uint32_t samplerFlags =
      BGFX_SAMPLER_UVW_CLAMP;
  bgfx::setTexture(0, submission.samplers[0], submission.textures[0],
                   samplerFlags);
  bgfx::setTexture(1, submission.samplers[1], submission.textures[1],
                   samplerFlags);
  bgfx::setTexture(2, submission.samplers[2], submission.textures[2],
                   samplerFlags);
  bgfx::submit(viewId, submission.program);
}

void VideoPlayer::play() {
  if (!isPlaying) {
    // Start playback
    startTime = stopwatch->elapsedMicros();
  } else if (isPaused) {
    // Resume playback
    long long now = stopwatch->elapsedMicros();
    double elapsedTime = (now - startTime) / 1000000.0;
    startTime =
        now - static_cast<long long>((lastFramePTS - elapsedTime) * 1000000);
  }
  isPlaying = true;
  isPaused = false;
  isEOF = false;
  eofCV.notify_all();
}

void VideoPlayer::playFrom(int64_t micro) {
  micro = std::max<int64_t>(0, micro);
  seek(micro);
  startTime = stopwatch->elapsedMicros() - micro;
  lastFramePTS = static_cast<double>(micro) / 1000000.0;
  isPlaying = true;
  isPaused = false;
  isEOF = false;
  eofCV.notify_all();
}

void VideoPlayer::pause() { isPaused = true; }

void VideoPlayer::stop() {
  isPlaying = false;
  hasVideoFrame = false;
}

void VideoPlayer::setDecodeSuspended(bool suspended) {
  const bool previous =
      decodeSuspended.exchange(suspended, std::memory_order_acq_rel);
  if (previous == suspended) {
    return;
  }
  freeSpace.notify_all();
  eofCV.notify_all();
}

void VideoPlayer::handleMemoryPressure(MemoryPressureMode mode) {
  if (mode == MemoryPressureMode::DiscardIdle) {
    setDecodeSuspended(true);
    std::lock_guard<std::mutex> videoLock(videoMutex);
    std::lock_guard<std::mutex> bufferLock(bufferMutex);
    for (AVFrame *&frame : frameBuffer) {
      if (frame != nullptr) {
        av_frame_free(&frame);
      }
    }
    bufferHead = 0;
    bufferTail = 0;
    bufferSize = 0;
  }
  {
    std::lock_guard<std::mutex> recycleLock(recycleMutex);
    for (AVFrame *&frame : recyclePool) {
      av_frame_free(&frame);
    }
    recyclePool.clear();
  }
  freeSpace.notify_all();
}

bool VideoPlayer::updateVideoTexture(int width, int height) {
  const auto layout = video::makeYuv420FrameLayout(width, height);
  if (!layout) {
    return false;
  }
  std::lock_guard<std::mutex> lock(videoFrameMutex);
  if (layout->width != videoFrameWidth || layout->height != videoFrameHeight) {
    if (bgfx::isValid(videoTextureY)) {
      bgfx::destroy(videoTextureY);
      videoTextureY = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(videoTextureU)) {
      bgfx::destroy(videoTextureU);
      videoTextureU = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(videoTextureV)) {
      bgfx::destroy(videoTextureV);
      videoTextureV = BGFX_INVALID_HANDLE;
    }

    videoFrameWidth = layout->width;
    videoFrameHeight = layout->height;

    // Create textures for Y, U, and V planes
    videoTextureY = bgfx::createTexture2D(
        layout->width, layout->height, false, 1,
        bgfx::TextureFormat::R8, BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE);

    videoTextureU = bgfx::createTexture2D(
        layout->chromaWidth, layout->chromaHeight, false, 1,
        bgfx::TextureFormat::R8, BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE);

    videoTextureV = bgfx::createTexture2D(
        layout->chromaWidth, layout->chromaHeight, false, 1,
        bgfx::TextureFormat::R8, BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE);
    if (!bgfx::isValid(videoTextureY) || !bgfx::isValid(videoTextureU) ||
        !bgfx::isValid(videoTextureV)) {
      if (bgfx::isValid(videoTextureY)) {
        bgfx::destroy(videoTextureY);
      }
      if (bgfx::isValid(videoTextureU)) {
        bgfx::destroy(videoTextureU);
      }
      if (bgfx::isValid(videoTextureV)) {
        bgfx::destroy(videoTextureV);
      }
      videoTextureY = BGFX_INVALID_HANDLE;
      videoTextureU = BGFX_INVALID_HANDLE;
      videoTextureV = BGFX_INVALID_HANDLE;
      videoFrameWidth = 0;
      videoFrameHeight = 0;
      return false;
    }
  }
  return true;
}

void VideoPlayer::seek(int64_t micro) {
  std::lock_guard<std::mutex> videoLock(videoMutex);
  if (!formatContext || !codecContext || videoStreamIndex < 0)
    return;

  // Convert microseconds to stream time base
  int64_t seekTarget =
      av_rescale_q(micro, {1, AV_TIME_BASE},
                   formatContext->streams[videoStreamIndex]->time_base);

  // Perform the seek operation
  if (av_seek_frame(formatContext, videoStreamIndex, seekTarget,
                    AVSEEK_FLAG_BACKWARD) < 0) {
    SDL_Log("Failed to seek to %" PRId64 " microseconds", micro);
    return;
  }
  avcodec_flush_buffers(codecContext);

  {
    std::lock_guard<std::mutex> lock(bufferMutex);
    for (size_t i = 0; i < maxBufferSize; ++i) {
      if (frameBuffer[i] != nullptr) {
        recycleFrame(frameBuffer[i]);
        frameBuffer[i] = nullptr;
      }
    }
    bufferHead = bufferTail = 0;
    bufferSize = 0;
  }
  decodeGeneration.fetch_add(1, std::memory_order_release);
  freeSpace.notify_all();

  // Reinitialize timing
  lastFramePTS = 0;
  startTime = stopwatch->elapsedMicros();
  hasVideoFrame = false;

  // Notify predecoding thread to continue from the new position
  SDL_Log("Seeked to %" PRId64 " microseconds", micro);
  isEOF = false;
  eofCV.notify_all();
}

void VideoPlayer::predecodeFrames() {
  AVPacket *localPacket = av_packet_alloc();
  AVFrame *decodedFrame = av_frame_alloc();
  if (localPacket == nullptr || decodedFrame == nullptr) {
    av_packet_free(&localPacket);
    av_frame_free(&decodedFrame);
    return;
  }

  video::VideoDecodeState decodeState;
  std::uint64_t observedGeneration =
      decodeGeneration.load(std::memory_order_acquire);

  auto queueDecodedFrame = [&]() {
    AVFrame *targetFrame = getRecycledFrame();
    if (targetFrame == nullptr) {
      SDL_Log("Failed to allocate a decoded video frame");
      return;
    }
    if (videoFrameWidth <= 0 || videoFrameHeight <= 0) {
      recycleFrame(targetFrame);
      return;
    }

    const bool canReuseExistingBuffer =
        targetFrame->buf[0] != nullptr &&
        targetFrame->format == AV_PIX_FMT_YUV420P &&
        targetFrame->width == videoFrameWidth &&
        targetFrame->height == videoFrameHeight;
    if (!canReuseExistingBuffer) {
      av_frame_unref(targetFrame);
      targetFrame->format = AV_PIX_FMT_YUV420P;
      targetFrame->width = videoFrameWidth;
      targetFrame->height = videoFrameHeight;
      if (av_frame_get_buffer(targetFrame, 32) < 0) {
        recycleFrame(targetFrame);
        return;
      }
    }
    if (av_frame_make_writable(targetFrame) < 0) {
      SDL_Log("Failed to make target frame writable");
      recycleFrame(targetFrame);
      return;
    }
    if (sws_scale(swsContext, decodedFrame->data, decodedFrame->linesize, 0,
                  codecContext->height, targetFrame->data,
                  targetFrame->linesize) <= 0) {
      SDL_Log("Failed to convert a decoded video frame");
      recycleFrame(targetFrame);
      return;
    }

    targetFrame->pts =
        decodedFrame->best_effort_timestamp != AV_NOPTS_VALUE
            ? decodedFrame->best_effort_timestamp
            : decodedFrame->pts;
    std::lock_guard<std::mutex> lock(bufferMutex);
    if (predecodingActive && bufferSize < maxBufferSize) {
      frameBuffer[bufferTail] = targetFrame;
      bufferTail = (bufferTail + 1) % maxBufferSize;
      ++bufferSize;
    } else {
      recycleFrame(targetFrame);
    }
  };

  while (predecodingActive.load(std::memory_order_acquire)) {
    const std::uint64_t generation =
        decodeGeneration.load(std::memory_order_acquire);
    if (generation != observedGeneration) {
      av_packet_unref(localPacket);
      decodeState.reset();
      observedGeneration = generation;
      isEOF = false;
    }

    {
      std::unique_lock<std::mutex> lock(bufferMutex);
      freeSpace.wait(lock, [this] {
        return !predecodingActive.load(std::memory_order_acquire) ||
               (!decodeSuspended.load(std::memory_order_acquire) &&
                bufferSize < maxBufferSize);
      });
    }

    if (!predecodingActive.load(std::memory_order_acquire)) {
      decodeState.cancel();
      break;
    }
    if (decodeSuspended.load(std::memory_order_acquire)) {
      continue;
    }

    const auto action = decodeState.nextAction(
        bufferSize.load(std::memory_order_acquire) < maxBufferSize);
    if (action == video::VideoDecodeAction::WaitForOutput) {
      continue;
    }
    if (action == video::VideoDecodeAction::Finished) {
      isEOF = true;
      std::unique_lock<std::mutex> lock(eofMutex);
      eofCV.wait(lock, [this, observedGeneration] {
        return !predecodingActive.load(std::memory_order_acquire) ||
               decodeGeneration.load(std::memory_order_acquire) !=
                   observedGeneration;
      });
      continue;
    }

    {
      std::lock_guard<std::mutex> videoLock(videoMutex);
      if (!formatContext || !codecContext) {
        decodeState.onReceive(video::VideoReceiveResult::Error);
        continue;
      }
      if (decodeGeneration.load(std::memory_order_acquire) !=
          observedGeneration) {
        continue;
      }

      if (action == video::VideoDecodeAction::ReceiveFrame) {
        av_frame_unref(decodedFrame);
        const int result = avcodec_receive_frame(codecContext, decodedFrame);
        if (result == 0) {
          decodeState.onReceive(video::VideoReceiveResult::Frame);
          queueDecodedFrame();
        } else if (result == AVERROR(EAGAIN)) {
          decodeState.onReceive(video::VideoReceiveResult::NeedInput);
        } else if (result == AVERROR_EOF) {
          decodeState.onReceive(video::VideoReceiveResult::EndOfStream);
        } else {
          SDL_Log("Video decoder receive failed: %d", result);
          decodeState.onReceive(video::VideoReceiveResult::Error);
        }
      } else if (action == video::VideoDecodeAction::ReadPacket) {
        av_packet_unref(localPacket);
        const int result = av_read_frame(formatContext, localPacket);
        if (result >= 0) {
          if (localPacket->stream_index == videoStreamIndex) {
            decodeState.onDemux(video::VideoDemuxResult::VideoPacket);
          } else {
            av_packet_unref(localPacket);
            decodeState.onDemux(video::VideoDemuxResult::SkippedPacket);
          }
        } else if (result == AVERROR_EOF) {
          decodeState.onDemux(video::VideoDemuxResult::EndOfStream);
        } else {
          SDL_Log("Video demux failed before EOF: %d", result);
          decodeState.onDemux(video::VideoDemuxResult::Error);
        }
      } else if (action == video::VideoDecodeAction::SendPacket) {
        const int result = avcodec_send_packet(codecContext, localPacket);
        if (result == 0) {
          av_packet_unref(localPacket);
          decodeState.onPacketSend(video::VideoSendResult::Accepted);
        } else if (result == AVERROR(EAGAIN)) {
          decodeState.onPacketSend(video::VideoSendResult::NeedDrain);
        } else if (result == AVERROR_EOF) {
          av_packet_unref(localPacket);
          decodeState.onPacketSend(video::VideoSendResult::EndOfStream);
        } else {
          SDL_Log("Video decoder rejected a packet: %d", result);
          av_packet_unref(localPacket);
          decodeState.onPacketSend(video::VideoSendResult::Error);
        }
      } else if (action == video::VideoDecodeAction::SendFlush) {
        const int result = avcodec_send_packet(codecContext, nullptr);
        if (result == 0) {
          decodeState.onFlushSend(video::VideoSendResult::Accepted);
        } else if (result == AVERROR(EAGAIN)) {
          decodeState.onFlushSend(video::VideoSendResult::NeedDrain);
        } else if (result == AVERROR_EOF) {
          decodeState.onFlushSend(video::VideoSendResult::EndOfStream);
        } else {
          SDL_Log("Video decoder flush failed: %d", result);
          decodeState.onFlushSend(video::VideoSendResult::Error);
        }
      }
    }
  }

  av_packet_unref(localPacket);
  av_frame_free(&decodedFrame);
  av_packet_free(&localPacket);
}

void VideoPlayer::stopPredecoding() {
  predecodingActive = false;

  freeSpace.notify_all();
  eofCV.notify_all();
  if (predecodeThread.joinable()) {
    predecodeThread.join();
  }

  // Clear the buffer
  {
    std::lock_guard<std::mutex> lock(bufferMutex);
    for (size_t i = 0; i < maxBufferSize; ++i) {
      if (frameBuffer[i] != nullptr) {
        // Free frames from buffer using recycled pool or direct free
        recycleFrame(frameBuffer[i]);
        frameBuffer[i] = nullptr;
      }
    }
    bufferHead = bufferTail = 0; // Reset buffer indices
    bufferSize = 0;
  }
}

AVFrame *VideoPlayer::getRecycledFrame() {
  std::lock_guard<std::mutex> lock(recycleMutex);
  if (recyclePool.empty()) {
    return av_frame_alloc();
  }
  AVFrame *frame = recyclePool.back();
  recyclePool.pop_back();
  return frame;
}

void VideoPlayer::recycleFrame(AVFrame *frame) {
  if (!frame)
    return;
  std::lock_guard<std::mutex> lock(recycleMutex);
  if (recyclePool.size() >= maxRecyclePoolSize) {
    av_frame_free(&frame);
    return;
  }
  recyclePool.push_back(frame);
}
