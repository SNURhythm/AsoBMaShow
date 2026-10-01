#include "audio/AudioMix.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>
using Clock=std::chrono::steady_clock;
int main(){
  SoundData sound;sound.channels=2;sound.outputFrameCount=48000;sound.outputData.resize(96000,1000);
  for(unsigned frames:{64,128,512}) for(unsigned voices:{16,64,128,256,512}) {
    AudioCallbackState s;std::vector<float> buffer(frames*2);std::vector<double> times;
    for(int k=0;k<550;k++){
      s.playingSoundCount=voices;for(unsigned j=0;j<voices;j++)s.playingSounds[j]={.soundData=&sound};
      std::fill(buffer.begin(),buffer.end(),0);auto t=Clock::now();
      audio::playback::MixActiveSounds(s,buffer,frames,2,1,1,100,audio::playback::MixScope::AllBuses);
      auto us=std::chrono::duration<double,std::micro>(Clock::now()-t).count();if(k>=50)times.push_back(us);
    }
    std::sort(times.begin(),times.end());std::printf("frames=%u voices=%u mix_p50=%.1fus mix_p99=%.1fus period=%.1fus\n",frames,voices,times[250],times[494],frames*1e6/48000);
  }
  for(unsigned n:{1000,10000,65536,200000}){
    AudioCallbackState s; audio::playback::PrepareScheduledSoundCapacity(s,n);std::vector<double> times;
    for(int k=0;k<250;k++){
      s.scheduledSoundCount=n;s.playingSoundCount=0;
      for(unsigned j=0;j<n;j++)s.scheduledSounds[j]={.soundData=&sound,.startMicros=j?1000000000:0};
      auto t=Clock::now();audio::playback::ActivateScheduledSounds(s,0,48000,128,100);
      auto us=std::chrono::duration<double,std::micro>(Clock::now()-t).count();if(k>=50)times.push_back(us);
    }
    std::sort(times.begin(),times.end());std::printf("scheduled=%u activate_one_p50=%.1fus p99=%.1fus\n",n,times[100],times[197]);
  }
}
