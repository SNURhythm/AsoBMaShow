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
