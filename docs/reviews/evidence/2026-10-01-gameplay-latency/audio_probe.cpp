#include <portaudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
struct Sample { unsigned long frames; double lead; unsigned long flags; };
struct Probe { std::array<Sample, 10000> data{}; std::atomic<unsigned> count{0}; };
int callback(const void*, void* out, unsigned long frames, const PaStreamCallbackTimeInfo* t, PaStreamCallbackFlags f, void* u) {
  auto& p = *static_cast<Probe*>(u); unsigned i=p.count.load();
  if(i<p.data.size()){p.data[i]={frames,(t->outputBufferDacTime-t->currentTime)*1000,f};p.count.store(i+1);}
  std::memset(out,0,frames*2*sizeof(short)); return paContinue;
}
int main(){
  if(Pa_Initialize()!=paNoError)return 1;
  auto id=Pa_GetDefaultOutputDevice();auto* d=Pa_GetDeviceInfo(id);if(!d)return 2;
  std::printf("device=%s sampleRate=%.0f defaultLow=%.3fms\n",d->name,d->defaultSampleRate,d->defaultLowOutputLatency*1000);
  for(auto n: {0UL,64UL,128UL,256UL,512UL}) {
    Probe p;PaStream* s=nullptr;PaStreamParameters o{};o.device=id;o.channelCount=2;o.sampleFormat=paInt16;o.suggestedLatency=d->defaultLowOutputLatency;
    auto e=Pa_OpenStream(&s,nullptr,&o,d->defaultSampleRate,n,paNoFlag,callback,&p);
    if(e){std::printf("open %lu: %s\n",n,Pa_GetErrorText(e));continue;}
    double reported=Pa_GetStreamInfo(s)->outputLatency*1000;
    e=Pa_StartStream(s);if(e){std::printf("start: %s\n",Pa_GetErrorText(e));Pa_CloseStream(s);continue;}
    Pa_Sleep(1000);Pa_StopStream(s);Pa_CloseStream(s);
    unsigned count=std::min<unsigned>(p.count.load(),p.data.size());std::array<double,10000> lead{};unsigned flags=0;unsigned long lo=999999,hi=0;
    for(unsigned i=0;i<count;i++){lead[i]=p.data[i].lead;flags+=p.data[i].flags!=0;lo=std::min(lo,p.data[i].frames);hi=std::max(hi,p.data[i].frames);}
    std::sort(lead.begin(),lead.begin()+count);
    std::printf("requested=%lu callbacks=%u actualFrames=%lu..%lu reported=%.3fms DAClead_p50=%.3fms DAClead_p99=%.3fms flagged=%u\n",n,count,lo,hi,reported,count?lead[count/2]:0,count?lead[(count-1)*99/100]:0,flags);
  }
  Pa_Terminate();
}
