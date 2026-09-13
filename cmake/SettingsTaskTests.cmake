# Settings worker ownership and application-thread handoff fixtures.
set(settings_cache_scene_methods
    ${CMAKE_CURRENT_BINARY_DIR}/generated/settings_cache_scene_methods.inc)
add_custom_command(
    OUTPUT ${settings_cache_scene_methods}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/settings_cache_scene_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${settings_cache_scene_methods}
    DEPENDS tests/settings_cache_scene_extract.py
            tests/gameplay_terminal_scene_extract.py src/scene/SettingsScene.cpp
    VERBATIM
)
add_executable(settings_cache_maintenance_tests
    tests/settings_cache_maintenance_tests.cpp
    ${settings_cache_scene_methods}
    src/scene/SettingsCacheMaintenance.cpp
    src/archive/TemporaryCache.cpp
    src/path.cpp
)
target_include_directories(settings_cache_maintenance_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(settings_cache_maintenance_tests PRIVATE cxx_std_23)
target_link_libraries(settings_cache_maintenance_tests PRIVATE Threads::Threads)
asobmashow_register_test(settings_cache_maintenance_tests)

set(settings_library_lifecycle_methods
    ${CMAKE_CURRENT_BINARY_DIR}/generated/settings_library_lifecycle_methods.inc)
add_custom_command(
    OUTPUT ${settings_library_lifecycle_methods}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/settings_library_lifecycle_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${settings_library_lifecycle_methods}
    DEPENDS tests/settings_library_lifecycle_extract.py
            tests/gameplay_terminal_scene_extract.py src/scene/SettingsScenePreview.cpp
    VERBATIM
)
add_executable(settings_library_lifecycle_tests
    tests/settings_library_lifecycle_tests.cpp
    src/scene/SettingsLibraryTask.cpp
    ${settings_library_lifecycle_methods}
)
target_include_directories(settings_library_lifecycle_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(settings_library_lifecycle_tests PRIVATE cxx_std_23)
target_link_libraries(settings_library_lifecycle_tests PRIVATE Threads::Threads)
asobmashow_register_test(settings_library_lifecycle_tests)

set(settings_library_scene_methods
    ${CMAKE_CURRENT_BINARY_DIR}/generated/settings_library_scene_methods.inc)
add_custom_command(
    OUTPUT ${settings_library_scene_methods}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/settings_library_scene_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${settings_library_scene_methods}
    DEPENDS tests/settings_library_scene_extract.py
            tests/gameplay_terminal_scene_extract.py src/scene/SettingsSceneTables.cpp
    VERBATIM
)
add_executable(settings_library_task_tests
    tests/settings_library_task_tests.cpp
    ${settings_library_scene_methods}
    src/scene/SettingsLibraryTask.cpp
)
target_include_directories(settings_library_task_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(settings_library_task_tests PRIVATE cxx_std_23)
target_link_libraries(settings_library_task_tests PRIVATE Threads::Threads)
asobmashow_register_test(settings_library_task_tests)

# The fixed preview recipe uses real parser ownership without scene/rendering dependencies.
add_executable(settings_preview_chart_tests
    tests/settings_preview_chart_tests.cpp
    tests/support/AllocationLifetimeProbe.cpp
    src/scene/SettingsPreviewChart.cpp
    src/bms_parser.cpp
)
target_include_directories(settings_preview_chart_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(settings_preview_chart_tests PRIVATE cxx_std_23)
asobmashow_register_test(settings_preview_chart_tests)
