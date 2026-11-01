add_executable(download_jobs_tests
    tests/download_jobs_tests.cpp
    src/downloads/DownloadJobs.cpp
    src/downloads/BrowserPolicy.cpp
    src/AtomicFile.cpp
    src/storage/StoragePolicy.cpp
)
target_include_directories(download_jobs_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(download_jobs_tests PRIVATE cxx_std_23)
target_link_libraries(download_jobs_tests PRIVATE Threads::Threads)
asobmashow_register_test(download_jobs_tests)
