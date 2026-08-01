#include "VideoDecodeState.h"

namespace video {

VideoDecodeAction
VideoDecodeState::nextAction(bool outputHasCapacity) const noexcept {
  if (finished_) {
    return VideoDecodeAction::Finished;
  }
  if (!outputHasCapacity) {
    return VideoDecodeAction::WaitForOutput;
  }
  if (needsReceive_) {
    return VideoDecodeAction::ReceiveFrame;
  }
  if (packetPending_) {
    return VideoDecodeAction::SendPacket;
  }
  if (!demuxEnded_) {
    return VideoDecodeAction::ReadPacket;
  }
  if (!flushSent_) {
    return VideoDecodeAction::SendFlush;
  }
  return VideoDecodeAction::ReceiveFrame;
}

void VideoDecodeState::onReceive(VideoReceiveResult result) noexcept {
  switch (result) {
  case VideoReceiveResult::Frame:
    needsReceive_ = true;
    break;
  case VideoReceiveResult::NeedInput:
    if (demuxEnded_ && flushSent_) {
      finished_ = true;
    } else {
      needsReceive_ = false;
    }
    break;
  case VideoReceiveResult::EndOfStream:
    finished_ = true;
    break;
  case VideoReceiveResult::Error:
    failed_ = true;
    finished_ = true;
    break;
  }
}

void VideoDecodeState::onDemux(VideoDemuxResult result) noexcept {
  switch (result) {
  case VideoDemuxResult::VideoPacket:
    packetPending_ = true;
    needsReceive_ = false;
    break;
  case VideoDemuxResult::SkippedPacket:
    needsReceive_ = false;
    break;
  case VideoDemuxResult::EndOfStream:
    demuxEnded_ = true;
    needsReceive_ = false;
    break;
  case VideoDemuxResult::Error:
    demuxEnded_ = true;
    failed_ = true;
    needsReceive_ = false;
    break;
  }
}

void VideoDecodeState::onPacketSend(VideoSendResult result) noexcept {
  switch (result) {
  case VideoSendResult::Accepted:
    packetPending_ = false;
    needsReceive_ = true;
    break;
  case VideoSendResult::NeedDrain:
    needsReceive_ = true;
    break;
  case VideoSendResult::EndOfStream:
    packetPending_ = false;
    finished_ = true;
    break;
  case VideoSendResult::Error:
    packetPending_ = false;
    failed_ = true;
    finished_ = true;
    break;
  }
}

void VideoDecodeState::onFlushSend(VideoSendResult result) noexcept {
  switch (result) {
  case VideoSendResult::Accepted:
    flushSent_ = true;
    needsReceive_ = true;
    break;
  case VideoSendResult::NeedDrain:
    needsReceive_ = true;
    break;
  case VideoSendResult::EndOfStream:
    flushSent_ = true;
    finished_ = true;
    break;
  case VideoSendResult::Error:
    failed_ = true;
    finished_ = true;
    break;
  }
}

void VideoDecodeState::cancel() noexcept {
  cancelled_ = true;
  finished_ = true;
}

void VideoDecodeState::reset() noexcept { *this = VideoDecodeState{}; }

bool VideoDecodeState::hasPendingPacket() const noexcept {
  return packetPending_;
}

bool VideoDecodeState::flushSent() const noexcept { return flushSent_; }
bool VideoDecodeState::failed() const noexcept { return failed_; }
bool VideoDecodeState::cancelled() const noexcept { return cancelled_; }

} // namespace video
