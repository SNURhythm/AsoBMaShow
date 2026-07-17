target_sources(main PRIVATE GamePlayScene.cpp BMSRenderer.cpp
                            RhythmLaneInputController.cpp
                            JudgementIndicatorRenderer.cpp Judge.cpp
                            CompiledGameplayJudge.cpp GameplayDefinition.cpp
                            GameplaySimulation.cpp RealtimeGameplayWorker.cpp
                            RealtimeTouchInputRouter.cpp)
if(WIN32)
    target_link_libraries(main PRIVATE avrt)
endif()
