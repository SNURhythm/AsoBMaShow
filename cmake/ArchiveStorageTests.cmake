# Storage and extraction policy tests compile their production owners without
# linking archive backend or media libraries.
add_executable(temporary_archive_cache_tests
    tests/temporary_archive_cache_tests.cpp
    src/archive/TemporaryCache.cpp
    src/path.cpp
)
target_include_directories(temporary_archive_cache_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(temporary_archive_cache_tests PRIVATE cxx_std_23)
target_link_libraries(temporary_archive_cache_tests PRIVATE Threads::Threads)
asobmashow_register_test(temporary_archive_cache_tests)

add_executable(unzip_output_tests
    tests/unzip_output_tests.cpp
    src/archive/UnzipOutput.cpp
)
target_include_directories(unzip_output_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(unzip_output_tests PRIVATE cxx_std_23)
target_link_libraries(unzip_output_tests PRIVATE Threads::Threads)
asobmashow_register_test(unzip_output_tests)

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

add_executable(archive_index_build_coordinator_tests
    tests/archive_index_build_coordinator_tests.cpp
    src/archive/IndexBuildCoordinator.cpp
)
target_include_directories(archive_index_build_coordinator_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(archive_index_build_coordinator_tests PRIVATE cxx_std_23)
target_link_libraries(archive_index_build_coordinator_tests PRIVATE Threads::Threads)
asobmashow_register_test(archive_index_build_coordinator_tests)
