target_sources(main PRIVATE GamePlayScene.cpp BMSRenderer.cpp
                            RhythmLaneInputController.cpp
                            JudgementIndicatorRenderer.cpp Judge.cpp
                            GameplayJudgeRules.cpp
                            CompiledGameplayJudge.cpp GameplayDefinition.cpp
                            GameplaySimulation.cpp
                            RealtimeGameplayAuthorityPolicy.cpp
                            RealtimeGameplayWorker.cpp
                            RealtimeTouchInputRouter.cpp
                            ReplayKeysoundSchedule.cpp)
if(WIN32)
    target_link_libraries(main PRIVATE avrt)
endif()
