target_sources(main PRIVATE GamePlayScene.cpp BMSRenderer.cpp
                            BuiltInPlayfieldPresentation.cpp
                            PlayfieldChartVisualModel.cpp
                            PlayfieldProjection.cpp
                            PlayfieldVisualState.cpp
                            RhythmLaneInputController.cpp
                            JudgementIndicatorRenderer.cpp Judge.cpp
                            GameplayJudgeRules.cpp
                            GameplayGaugeRules.cpp
                            GameplayRulesetPolicy.cpp
                            GameplayNoteJudgeRole.cpp
                            GameplayCandidateRules.cpp
                            CompiledGameplayJudge.cpp GameplayDefinition.cpp
                            GameplaySimulation.cpp
                            RealtimeGameplayAuthorityPolicy.cpp
                            RealtimeGameplayInputBridge.cpp
                            RealtimeGameplayWorker.cpp
                            RealtimeTouchInputRouter.cpp
                            ReplayKeysoundSchedule.cpp)
if(WIN32)
    target_link_libraries(main PRIVATE avrt)
endif()
