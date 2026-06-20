#include "VideoPlayer.h"
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include <cstring>

#include <thread>
VideoPlayer::VideoPlayer(Stopwatch *stopwatch)
    : stopwatch(stopwatch), videoFrameWidth(0), videoFrameHeight(0),
      hasVideoFrame(false) {

  s_texY = bgfx::createUniform("s_texY", bgfx::UniformType::Sampler);
  s_texU = bgfx::createUniform("s_texU", bgfx::UniformType::Sampler);
  s_texV = bgfx::createUniform("s_texV", bgfx::UniformType::Sampler);
  frameBuffer.resize(maxBufferSize, nullptr);
}

VideoPlayer::~VideoPlayer() {

  bgfx::destroy(s_texY);
  bgfx::destroy(s_texU);
  bgfx::destroy(s_texV);
  if (bgfx::isValid(videoTextureY)) {
    bgfx::destroy(videoTextureY);
  }
  if (bgfx::isValid(videoTextureU)) {
    bgfx::destroy(videoTextureU);
  }
  if (bgfx::isValid(videoTextureV)) {
    bgfx::destroy(videoTextureV);
  }
  unloadVideo();
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
  {
    std::lock_guard<std::mutex> videoLock(videoMutex);
    AVFormatContext *tempFormatContext = avformat_alloc_context();
    if (tempFormatContext == nullptr) {
      return false;
    }
    auto fail = [&]() {
      if (codecContext != nullptr) {
        avcodec_free_context(&codecContext);
      }
      if (tempFormatContext != nullptr) {
        avformat_close_input(&tempFormatContext);
      }
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

    updateVideoTexture(codecContext->width, codecContext->height);

    formatContext = tempFormatContext;
    predecodingActive = true;
    predecodeThread = std::thread(&VideoPlayer::predecodeFrames, this);
    return true;
  }
}

void VideoPlayer::update() {
  if (!isPlaying)
    return;
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
  const uint16_t yPitch = static_cast<uint16_t>(currentFrame->linesize[0]);
  const uint16_t uPitch = static_cast<uint16_t>(currentFrame->linesize[1]);
  const uint16_t vPitch = static_cast<uint16_t>(currentFrame->linesize[2]);
  const uint32_t yBytes = static_cast<uint32_t>(currentFrame->linesize[0]) *
                          static_cast<uint32_t>(videoFrameHeight);
  const uint32_t uBytes = static_cast<uint32_t>(currentFrame->linesize[1]) *
                          static_cast<uint32_t>(videoFrameHeight / 2);
  const uint32_t vBytes = static_cast<uint32_t>(currentFrame->linesize[2]) *
                          static_cast<uint32_t>(videoFrameHeight / 2);

  bgfx::updateTexture2D(videoTextureY, 0, 0, 0, 0, videoFrameWidth,
                        videoFrameHeight,
                        bgfx::copy(currentFrame->data[0], yBytes), yPitch);
  bgfx::updateTexture2D(videoTextureU, 0, 0, 0, 0, videoFrameWidth / 2,
                        videoFrameHeight / 2,
                        bgfx::copy(currentFrame->data[1], uBytes), uPitch);
  bgfx::updateTexture2D(videoTextureV, 0, 0, 0, 0, videoFrameWidth / 2,
                        videoFrameHeight / 2,
                        bgfx::copy(currentFrame->data[2], vBytes), vPitch);

  lastFramePTS = frameTime;
  hasVideoFrame = true;

  // Recycle the displayed frame
  recycleFrame(currentFrame);
}
unsigned int VideoPlayer::getPrecisePosition() {
  // calculate the frame position in microseconds
  return static_cast<unsigned int>(lastFramePTS * 1000000);
}

void VideoPlayer::render() {
  if (!hasVideoFrame)
    return;
  if (!isPlaying) {
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

  static const bgfx::ProgramHandle kProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_YUVRGB);
  bgfx::submit(viewId, kProgram);
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

void VideoPlayer::pause() { isPaused = true; }

void VideoPlayer::stop() {
  isPlaying = false;
  hasVideoFrame = false;
}

void VideoPlayer::updateVideoTexture(unsigned int width, unsigned int height) {
  std::lock_guard<std::mutex> lock(videoFrameMutex);
  if (width != videoFrameWidth || height != videoFrameHeight) {
    if (bgfx::isValid(videoTextureY))
      bgfx::destroy(videoTextureY);
    if (bgfx::isValid(videoTextureU))
      bgfx::destroy(videoTextureU);
    if (bgfx::isValid(videoTextureV))
      bgfx::destroy(videoTextureV);

    videoFrameWidth = width;
    videoFrameHeight = height;

    // Create textures for Y, U, and V planes
    videoTextureY = bgfx::createTexture2D(
        uint16_t(videoFrameWidth), uint16_t(videoFrameHeight), false, 1,
        bgfx::TextureFormat::R8, BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE);

    videoTextureU = bgfx::createTexture2D(
        uint16_t(videoFrameWidth / 2), uint16_t(videoFrameHeight / 2), false, 1,
        bgfx::TextureFormat::R8, BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE);

    videoTextureV = bgfx::createTexture2D(
        uint16_t(videoFrameWidth / 2), uint16_t(videoFrameHeight / 2), false, 1,
        bgfx::TextureFormat::R8, BGFX_TEXTURE_NONE | BGFX_SAMPLER_NONE);
  }
}

void VideoPlayer::seek(int64_t micro) {
  std::lock_guard<std::mutex> videoLock(videoMutex);
  if (!formatContext || !codecContext || videoStreamIndex < 0)
    return;

  // Convert microseconds to stream time base
  int64_t seekTarget =
      av_rescale_q(micro, {1, AV_TIME_BASE},
                   formatContext->streams[videoStreamIndex]->time_base);

  {
    // Stop playback and clear the ring buffer
    std::lock_guard<std::mutex> lock(bufferMutex);
    avcodec_flush_buffers(codecContext);

    // Free all frames in the ring buffer
    for (size_t i = 0; i < maxBufferSize; ++i) {
      if (frameBuffer[i] != nullptr) {
        recycleFrame(frameBuffer[i]);
        frameBuffer[i] = nullptr;
      }
    }
    bufferHead = bufferTail = 0; // Reset buffer indices

    // Reset freeSpace
    bufferSize = 0;
    freeSpace.notify_all();
  }

  // Perform the seek operation
  if (av_seek_frame(formatContext, videoStreamIndex, seekTarget,
                    AVSEEK_FLAG_BACKWARD) < 0) {
    SDL_Log("Failed to seek to %lld microseconds", micro);
    return;
  }

  // Reinitialize timing
  lastFramePTS = 0;
  startTime = stopwatch->elapsedMicros();
  hasVideoFrame = false;

  // Notify predecoding thread to continue from the new position
  SDL_Log("Seeked to %lld microseconds", micro);
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

  while (predecodingActive) {
    {
      std::unique_lock<std::mutex> lock(bufferMutex);
      freeSpace.wait(lock, [this] {
        return bufferSize < maxBufferSize || !predecodingActive;
      });
    }

    if (!predecodingActive)
      break;

    bool readFailed = false;
    {
      std::lock_guard<std::mutex> videoLock(videoMutex);
      if (!formatContext || !codecContext) {
        continue;
      }

      av_packet_unref(localPacket);
      if (av_read_frame(formatContext, localPacket) >= 0) {
        if (localPacket->stream_index == videoStreamIndex) {

          // Send packet to decoder
          int send_ret = avcodec_send_packet(codecContext, localPacket);

          if (send_ret >= 0) {
            // FFmpeg 7.1 Fix: Drain all available frames from the decoder
            while (true) {
              av_frame_unref(decodedFrame);
              int receive_ret =
                  avcodec_receive_frame(codecContext, decodedFrame);

              if (receive_ret == 0) {
                // Frame decoded successfully. Now scale it on this thread.
                AVFrame *targetFrame = getRecycledFrame();
                if (targetFrame) {
                  if (videoFrameWidth <= 0 || videoFrameHeight <= 0) {
                    recycleFrame(targetFrame);
                    continue;
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
                      continue;
                    }
                  }
                  if (av_frame_make_writable(targetFrame) < 0) {
                    SDL_Log("Failed to make target frame writable");
                    recycleFrame(targetFrame);
                    continue;
                  }

                  // Perform sws_scale
                  // We need to set up the data pointers for sws_scale
                  // Since targetFrame is allocated by av_frame_get_buffer, its
                  // data/linesize are set.

                  sws_scale(swsContext, decodedFrame->data,
                            decodedFrame->linesize, 0, codecContext->height,
                            targetFrame->data, targetFrame->linesize);

                  const int64_t bestPts =
                      decodedFrame->best_effort_timestamp != AV_NOPTS_VALUE
                          ? decodedFrame->best_effort_timestamp
                          : decodedFrame->pts;
                  targetFrame->pts = bestPts;

                  std::lock_guard<std::mutex> lock(bufferMutex);
                  if (bufferSize < maxBufferSize) {
                    frameBuffer[bufferTail] = targetFrame;
                    bufferTail = (bufferTail + 1) % maxBufferSize;
                    ++bufferSize;
                  } else {
                    recycleFrame(targetFrame);
                    break;
                  }

                  // If buffer is full, we must stop receiving for now
                  if (bufferSize >= maxBufferSize) {
                    break;
                  }
                } else {
                  SDL_Log("Failed to get recycled frame");
                }
              } else {
                if (receive_ret == AVERROR(EAGAIN) ||
                    receive_ret == AVERROR_EOF)
                  break;
                readFailed = true; // Actual error
                break;
              }
            }
          }
        }
        av_packet_unref(localPacket);
      } else {
        isEOF = true;
        readFailed = true;
      }
    }

    if (readFailed && isEOF) {
      std::unique_lock<std::mutex> lock(eofMutex);
      eofCV.wait(lock, [this] { return !isEOF || !predecodingActive; });
    }
  }

  av_frame_free(&decodedFrame);
  av_packet_free(&localPacket);
}

void VideoPlayer::stopPredecoding() {
  predecodingActive = false;

  // Release all semaphores to unblock any waiting threads
  bufferSize = 0;
  freeSpace.notify_all(); // Release all free space
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
  if (recyclePool.size() >= maxBufferSize * 2) {
    av_frame_free(&frame);
    return;
  }
  recyclePool.push_back(frame);
}
