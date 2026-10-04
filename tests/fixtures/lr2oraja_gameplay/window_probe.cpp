#include "GameplayJudgeRules.h"
#include <iostream>
int main(){for(int rank=1;rank<=400;rank++)for(int rate:{0,1,33,50,75,99,100,101,125,200}){
 auto r=gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2,2,100,rate,CourseJudgementConstraint::None,gameplay::CandidateSelectionMode::Lowest,7,rank);
 // Java NoteType order NOTE,LONGNOTE_END,SCRATCH,LONGSCRATCH_END
 int order[]={0,2,1,3};for(int c=0;c<4;c++)for(int i=0;i<(c==1||c==3?4:5);i++){
 auto w=r.contexts[order[c]].windows[i];std::cout<<rank<<","<<rate<<","<<c<<","<<i<<","<<w.earlyMicros<<","<<w.lateMicros<<"\n";
 }}}
