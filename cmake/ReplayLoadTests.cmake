# Best-replay resolution and its owned asynchronous scene handoff.
add_executable(best_replay_resolver_tests
    tests/best_replay_resolver_tests.cpp
    src/ScoreProvenance.cpp
    src/replay/BestReplayResolver.cpp
    src/scene/play/GameplayGaugeRules.cpp
)
target_include_directories(best_replay_resolver_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(best_replay_resolver_tests PRIVATE cxx_std_23)


set(best_replay_scene_methods
    ${CMAKE_CURRENT_BINARY_DIR}/generated/best_replay_scene_methods.inc)
add_custom_command(
    OUTPUT ${best_replay_scene_methods}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/best_replay_scene_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${best_replay_scene_methods}
    DEPENDS tests/best_replay_scene_extract.py
            tests/gameplay_terminal_scene_extract.py src/scene/play/GamePlayScene.cpp
    VERBATIM
)
add_executable(best_replay_load_tests
    tests/best_replay_load_tests.cpp
    ${best_replay_scene_methods}
    src/bms_parser.cpp
    src/scene/play/BestReplayLoad.cpp
    src/scene/ReplayRecordTask.cpp
    src/replay/BestReplayResolver.cpp
    src/ScoreProvenance.cpp
    src/scene/play/GameplayGaugeRules.cpp
)
target_include_directories(best_replay_load_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(best_replay_load_tests PRIVATE cxx_std_23)
target_link_libraries(best_replay_load_tests PRIVATE Threads::Threads)
