# Shared work sizing and indexed execution without media/platform dependencies.
add_executable(parallel_work_tests tests/parallel_work_tests.cpp)
target_include_directories(parallel_work_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(parallel_work_tests PRIVATE cxx_std_23)
target_link_libraries(parallel_work_tests PRIVATE Threads::Threads)
asobmashow_register_test(parallel_work_tests)
