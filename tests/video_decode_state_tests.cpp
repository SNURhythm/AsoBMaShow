#include "video/VideoDecodeState.h"

#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testReceiveBeforeReadAndSend() {
  video::VideoDecodeState state;
  expect(state.nextAction() == video::VideoDecodeAction::ReceiveFrame,
         "decoder output is drained before reading input");
  state.onReceive(video::VideoReceiveResult::NeedInput);
  expect(state.nextAction() == video::VideoDecodeAction::ReadPacket,
         "input is read only after the decoder requests it");
  state.onDemux(video::VideoDemuxResult::SkippedPacket);
  expect(state.nextAction() == video::VideoDecodeAction::ReadPacket,
         "non-video packets do not perturb codec state");
  state.onDemux(video::VideoDemuxResult::VideoPacket);
  expect(state.hasPendingPacket() &&
             state.nextAction() == video::VideoDecodeAction::SendPacket,
         "a video packet remains pending until accepted");
}

void testSendEagainRetainsPacketUntilDrain() {
  video::VideoDecodeState state;
  state.onReceive(video::VideoReceiveResult::NeedInput);
  state.onDemux(video::VideoDemuxResult::VideoPacket);
  state.onPacketSend(video::VideoSendResult::NeedDrain);
  expect(state.hasPendingPacket() &&
             state.nextAction() == video::VideoDecodeAction::ReceiveFrame,
         "send EAGAIN retains the packet and returns to receive");
  state.onReceive(video::VideoReceiveResult::Frame);
  expect(state.hasPendingPacket() &&
             state.nextAction() == video::VideoDecodeAction::ReceiveFrame,
         "available output is fully drained with the packet retained");
  state.onReceive(video::VideoReceiveResult::NeedInput);
  expect(state.nextAction() == video::VideoDecodeAction::SendPacket,
         "retained packet is retried after draining");
  state.onPacketSend(video::VideoSendResult::Accepted);
  expect(!state.hasPendingPacket() &&
             state.nextAction() == video::VideoDecodeAction::ReceiveFrame,
         "accepted packet is released and its output is drained");
}

void testOutputCapacityPausesEveryCodecAction() {
  video::VideoDecodeState state;
  expect(state.nextAction(false) == video::VideoDecodeAction::WaitForOutput,
         "full frame buffer pauses decoding before receive");
  state.onReceive(video::VideoReceiveResult::NeedInput);
  state.onDemux(video::VideoDemuxResult::VideoPacket);
  expect(state.nextAction(false) == video::VideoDecodeAction::WaitForOutput &&
             state.hasPendingPacket(),
         "full frame buffer cannot discard a pending packet");
}

void testEofSendsOneFlushAndDrainsDelayedFrames() {
  video::VideoDecodeState state;
  state.onReceive(video::VideoReceiveResult::NeedInput);
  state.onDemux(video::VideoDemuxResult::EndOfStream);
  expect(state.nextAction() == video::VideoDecodeAction::SendFlush,
         "demux EOF schedules a null packet flush");
  state.onFlushSend(video::VideoSendResult::NeedDrain);
  expect(!state.flushSent() &&
             state.nextAction() == video::VideoDecodeAction::ReceiveFrame,
         "flush EAGAIN drains output before retrying the null packet");
  state.onReceive(video::VideoReceiveResult::NeedInput);
  expect(state.nextAction() == video::VideoDecodeAction::SendFlush,
         "unaccepted flush is retried");
  state.onFlushSend(video::VideoSendResult::Accepted);
  expect(state.flushSent() &&
             state.nextAction() == video::VideoDecodeAction::ReceiveFrame,
         "accepted flush is sent once and enters decoder drain");
  state.onReceive(video::VideoReceiveResult::Frame);
  expect(state.nextAction() == video::VideoDecodeAction::ReceiveFrame,
         "delayed frames continue draining after the flush");
  state.onReceive(video::VideoReceiveResult::EndOfStream);
  expect(state.nextAction() == video::VideoDecodeAction::Finished,
         "decoder EOF completes only after delayed frames");
}

void testCancellationFailureAndSeekReset() {
  video::VideoDecodeState state;
  state.onReceive(video::VideoReceiveResult::Error);
  expect(state.failed() &&
             state.nextAction() == video::VideoDecodeAction::Finished,
         "codec error terminates with failure evidence");
  state.reset();
  expect(!state.failed() && !state.cancelled() && !state.flushSent() &&
             !state.hasPendingPacket() &&
             state.nextAction() == video::VideoDecodeAction::ReceiveFrame,
         "seek reset restores the receive-first state");
  state.cancel();
  expect(state.cancelled() &&
             state.nextAction() == video::VideoDecodeAction::Finished,
         "cancellation terminates without further codec work");
}

} // namespace

int main() {
  testReceiveBeforeReadAndSend();
  testSendEagainRetainsPacketUntilDrain();
  testOutputCapacityPausesEveryCodecAction();
  testEofSendsOneFlushAndDrainsDelayedFrames();
  testCancellationFailureAndSeekReset();
  if (failures != 0) {
    std::cerr << failures << " video decode state test(s) failed\n";
    return 1;
  }
  std::cout << "Video decode state tests passed\n";
  return 0;
}
