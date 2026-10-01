#include "audio/AudioBackend.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
struct Probe {
  audio::RenderTiming timing{};
  std::array<long long, 10000> leads{};
  unsigned count=0;
};
void timing(audio::RenderTiming t, void *p) { static_cast<Probe*>(p)->timing=t; }
void render(void *out, std::uint32_t frames, int channels, void *p) {
  auto &probe=*static_cast<Probe*>(p);
  auto now=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  if(probe.timing.outputTimestampKnown && probe.count<probe.leads.size()) probe.leads[probe.count++]=probe.timing.outputSteadyMicros-now;
  std::fill_n(static_cast<short*>(out),frames*channels,0);
}
int main() {
  auto factory=audio::CreatePlatformBackendFactory();
  for(auto frames:{0U,128U,256U}) {
    Probe probe;std::string error;
    auto stream=factory->open({.bufferFrames=frames},render,&probe,error);
    if(!stream){std::printf("open failed: %s\n",error.c_str());return 1;}
    stream->setRenderTimingCallback(timing,&probe);
    for(int run=0;run<2;++run) {
      probe.count=0;
      if(!stream->start(error)){std::printf("start failed: %s\n",error.c_str());return 1;}
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if(!stream->stop(error)){std::printf("stop failed: %s\n",error.c_str());return 1;}
      auto state=stream->runtimeState();std::sort(probe.leads.begin(),probe.leads.begin()+probe.count);
      std::printf("request=%u run=%d actual=%u rate=%u count=%llu known=%d reported=%.3fms DAC-minus-receipt-p50=%lldus p99=%lldus underflows=%llu callbackMax=%uus\n",frames,run,state.effectiveBufferFrames,state.effectiveCallbackSampleRate,(unsigned long long)state.callbackCount,state.outputTimestampKnown,state.effectiveLatencyMs,probe.count?probe.leads[probe.count/2]:0,probe.count?probe.leads[(probe.count-1)*99/100]:0,(unsigned long long)state.outputUnderflowCount,state.maxCallbackDurationMicros);
      if(probe.count==0 || probe.leads[probe.count/2]<0 || probe.leads[probe.count/2]>100000)return 2;
    }
  }
}
