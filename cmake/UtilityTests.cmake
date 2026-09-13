# Shared work sizing and indexed execution without media/platform dependencies.
add_executable(parallel_work_tests tests/parallel_work_tests.cpp)
target_include_directories(parallel_work_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(parallel_work_tests PRIVATE cxx_std_23)
target_link_libraries(parallel_work_tests PRIVATE Threads::Threads)
asobmashow_register_test(parallel_work_tests)

# Stable IDs and cache filenames must retain their stored byte representation.
add_executable(stable_hash_tests tests/stable_hash_tests.cpp)
target_include_directories(stable_hash_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(stable_hash_tests PRIVATE cxx_std_23)
asobmashow_register_test(stable_hash_tests)

# Run the same stop-request contract against native threads and Android's
# production fallback, selected locally after loading the host standard library.
foreach(thread_test IN ITEMS thread_compat_tests thread_compat_fallback_tests)
    add_executable(${thread_test} tests/thread_compat_tests.cpp)
    target_include_directories(${thread_test} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${thread_test} PRIVATE Threads::Threads)
    asobmashow_register_test(${thread_test})
    set_tests_properties(${thread_test} PROPERTIES TIMEOUT 10)
endforeach()
set_target_properties(thread_compat_fallback_tests PROPERTIES CXX_STANDARD 17)
target_compile_definitions(thread_compat_fallback_tests PRIVATE
    ASOBMASHOW_TEST_ANDROID_THREAD_FALLBACK)
