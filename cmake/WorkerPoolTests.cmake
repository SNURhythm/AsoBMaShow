add_executable(chart_scan_work_scheduler_tests
    tests/chart_scan_work_scheduler_tests.cpp
    src/ChartScanWorkScheduler.cpp
)
target_include_directories(chart_scan_work_scheduler_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(chart_scan_work_scheduler_tests PRIVATE cxx_std_23)

add_executable(image_decode_coordinator_tests
    tests/image_decode_coordinator_tests.cpp
    src/view/DecodedImageCache.cpp
    src/view/ImageDecodeCoordinator.cpp
)
target_include_directories(image_decode_coordinator_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(image_decode_coordinator_tests PRIVATE cxx_std_23)

add_executable(worker_pool_startup_failure_tests
    tests/worker_pool_startup_failure_tests.cpp
    src/ChartScanWorkScheduler.cpp
    src/view/DecodedImageCache.cpp
    src/view/ImageDecodeCoordinator.cpp
)
target_include_directories(worker_pool_startup_failure_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(worker_pool_startup_failure_tests PRIVATE cxx_std_23)
target_link_libraries(worker_pool_startup_failure_tests PRIVATE Threads::Threads)
asobmashow_register_test(worker_pool_startup_failure_tests
    chart_scan_startup_failure_tests scheduler)
asobmashow_register_test(worker_pool_startup_failure_tests
    image_decode_startup_failure_tests image)
set_tests_properties(chart_scan_startup_failure_tests
    image_decode_startup_failure_tests PROPERTIES TIMEOUT 15)
