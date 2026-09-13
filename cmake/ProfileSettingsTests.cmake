# Profile controller policy and asynchronous archive execution.
set(profile_archive_scene_methods
    ${CMAKE_CURRENT_BINARY_DIR}/generated/profile_archive_scene_methods.inc)
add_custom_command(
    OUTPUT ${profile_archive_scene_methods}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/profile_archive_scene_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${profile_archive_scene_methods}
    DEPENDS tests/profile_archive_scene_extract.py tests/gameplay_terminal_scene_extract.py
            src/scene/SettingsSceneProfiles.cpp
    VERBATIM
)
add_executable(profile_settings_controller_tests
    tests/profile_settings_controller_tests.cpp
    ${profile_archive_scene_methods}
    src/scene/ProfileSettingsController.cpp
    src/scene/ProfileArchiveWorker.cpp
    src/scene/ProfileRuntimeReapply.cpp
)
target_include_directories(profile_settings_controller_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_features(profile_settings_controller_tests PRIVATE cxx_std_23)

target_link_libraries(profile_settings_controller_tests PRIVATE Threads::Threads)
