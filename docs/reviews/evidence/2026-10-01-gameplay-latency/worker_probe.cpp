#include "scene/play/RealtimeGameplayWorker.h"
#include "scene/play/Judge.h"
#include "bms_parser.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
using Clock=std::chrono::steady_clock;
long long micros(){return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();}
std::optional<std::int64_t> map(void*,std::int64_t){return 0;}
std::optional<std::int64_t> now(void*){return 0;}
std::atomic<long long> committed{0};
bool reserve(void*,gameplay::NoteId,gameplay::RealtimeGameplayAudioReservation&){return true;}
bool commit(void*,gameplay::RealtimeGameplayAudioReservation,gameplay::NoteId){committed.store(micros());return true;}
int main(){
 for(int count:{1000,10000,50000,100000}){
  bms_parser::Chart chart;chart.Meta.TotalNotes=count;chart.Meta.KeyMode=7;
  auto* measure=new bms_parser::Measure();chart.Measures.push_back(measure);
  for(int i=0;i<count;i++){auto* t=new bms_parser::TimeLine(8,false);t->Timing=1000000LL+i*1000LL;t->SetNote(1,new bms_parser::Note(11));measure->TimeLines.push_back(t);}
  gameplay::RealtimeGameplayWorkerConfig config{.epoch=1,.simulation={.judge=gameplay::CompiledGameplayJudge::from(Judge(1))},.clock={.mapSteadyToSong=map,.currentSongTime=now},.audio={.reserve=reserve,.commit=commit},.inputTriggeredKeysounds=true,.activationSongTimeMicros=1000000000000LL};
  gameplay::RealtimeGameplayWorker w(gameplay::buildGameplayDefinition(chart,0),config);w.start();
  std::vector<double> snapshots,audio;std::uint64_t previous=0;
  for(int k=0;k<450;k++){
    bool pressed=(k%2)==0; committed.store(0);auto start=micros();
    if(!w.enqueueInput({.epoch=1,.type=pressed?gameplay::RealtimeGameplayInputType::Press:gameplay::RealtimeGameplayInputType::Release,.lane=1,.compensateLane=1,.steadyTimestampMicros=start}))return 2;
    while(true){auto s=w.acquireLatestSnapshot();if(s->transactionSequence>previous&&s->lanePressed[1]==pressed){previous=s->transactionSequence;break;}if(micros()-start>2000000){std::fprintf(stderr,"timeout count=%d k=%d fault=%d\n",count,k,int(w.fault()));return 3;}std::this_thread::yield();}
    auto end=micros();if(k>=50){snapshots.push_back(end-start);if(pressed&&committed.load())audio.push_back(committed.load()-start);}
  }
  w.stop();std::sort(snapshots.begin(),snapshots.end());std::sort(audio.begin(),audio.end());
  std::printf("notes=%d enqueue_to_snapshot_p50=%.1fus p99=%.1fus enqueue_to_fake_audio_p50=%.1fus p99=%.1fus\n",count,snapshots[snapshots.size()/2],snapshots[snapshots.size()*99/100],audio.empty()?0:audio[audio.size()/2],audio.empty()?0:audio[audio.size()*99/100]);
 }
}
