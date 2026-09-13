# Music Player video acquisition and scene-owned teardown.
set(music_player_video_lifecycle_source
    ${CMAKE_CURRENT_BINARY_DIR}/generated/music_player_video_lifecycle_tests.cpp)
add_custom_command(
    OUTPUT ${music_player_video_lifecycle_source}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/music_player_video_lifecycle_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${music_player_video_lifecycle_source}
    DEPENDS tests/music_player_video_lifecycle_extract.py tests/gameplay_terminal_scene_extract.py
            tests/music_player_video_lifecycle_fixture.cpp
            src/scene/MusicPlayerScene.cpp src/scene/Scene.h
    VERBATIM
)
add_executable(music_player_video_lifecycle_tests ${music_player_video_lifecycle_source})
target_compile_features(music_player_video_lifecycle_tests PRIVATE cxx_std_23)
target_link_libraries(music_player_video_lifecycle_tests PRIVATE Threads::Threads)
asobmashow_register_test(music_player_video_lifecycle_tests)

set(music_player_sleep_timer_source
    ${CMAKE_CURRENT_BINARY_DIR}/generated/music_player_sleep_timer_tests.cpp)
add_custom_command(
    OUTPUT ${music_player_sleep_timer_source}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/music_player_sleep_timer_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${music_player_sleep_timer_source}
    DEPENDS tests/music_player_sleep_timer_extract.py tests/gameplay_terminal_scene_extract.py
            tests/music_player_sleep_timer_fixture.cpp src/audio/MusicPlayerService.cpp
    VERBATIM
)
add_executable(music_player_sleep_timer_tests ${music_player_sleep_timer_source})
target_include_directories(music_player_sleep_timer_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(music_player_sleep_timer_tests PRIVATE cxx_std_23)
target_link_libraries(music_player_sleep_timer_tests PRIVATE Threads::Threads)
asobmashow_register_test(music_player_sleep_timer_tests)
