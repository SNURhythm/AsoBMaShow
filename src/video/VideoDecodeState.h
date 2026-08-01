#pragma once

namespace video {

enum class VideoDecodeAction {
  ReceiveFrame,
  ReadPacket,
  SendPacket,
  SendFlush,
  WaitForOutput,
  Finished,
};

enum class VideoReceiveResult { Frame, NeedInput, EndOfStream, Error };
enum class VideoDemuxResult { VideoPacket, SkippedPacket, EndOfStream, Error };
enum class VideoSendResult { Accepted, NeedDrain, EndOfStream, Error };

class VideoDecodeState {
public:
  [[nodiscard]] VideoDecodeAction
  nextAction(bool outputHasCapacity = true) const noexcept;
  void onReceive(VideoReceiveResult result) noexcept;
  void onDemux(VideoDemuxResult result) noexcept;
  void onPacketSend(VideoSendResult result) noexcept;
  void onFlushSend(VideoSendResult result) noexcept;
  void cancel() noexcept;
  void reset() noexcept;

  [[nodiscard]] bool hasPendingPacket() const noexcept;
  [[nodiscard]] bool flushSent() const noexcept;
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] bool cancelled() const noexcept;

private:
  bool needsReceive_ = true;
  bool packetPending_ = false;
  bool demuxEnded_ = false;
  bool flushSent_ = false;
  bool finished_ = false;
  bool failed_ = false;
  bool cancelled_ = false;
};

} // namespace video
