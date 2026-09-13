add_executable(intro_scene_navigation_tests
    tests/intro_scene_navigation_tests.cpp
    src/music_select/MusicSelectInputProcessor.cpp
)
target_include_directories(intro_scene_navigation_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(intro_scene_navigation_tests PRIVATE cxx_std_23)

set(intro_scene_lifecycle_source
    ${CMAKE_CURRENT_BINARY_DIR}/generated/intro_scene_lifecycle_tests.cpp)
add_custom_command(
    OUTPUT ${intro_scene_lifecycle_source}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/intro_scene_lifecycle_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${intro_scene_lifecycle_source}
    DEPENDS tests/intro_scene_lifecycle_extract.py tests/gameplay_terminal_scene_extract.py
            tests/intro_scene_lifecycle_fixture.cpp src/scene/IntroScene.cpp src/scene/Scene.h
    VERBATIM
)
add_executable(intro_scene_lifecycle_tests ${intro_scene_lifecycle_source})
target_link_libraries(intro_scene_lifecycle_tests PRIVATE input_registry_test_support)
