add_executable(download_jobs_tests
    tests/download_jobs_tests.cpp
    src/downloads/DownloadJobs.cpp
    src/downloads/DownloadUrlPolicy.cpp
    src/AtomicFile.cpp
    src/storage/StoragePolicy.cpp
)
target_include_directories(download_jobs_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(download_jobs_tests PRIVATE cxx_std_23)
target_link_libraries(download_jobs_tests PRIVATE Threads::Threads)
asobmashow_register_test(download_jobs_tests)

add_executable(download_coordinator_tests
    tests/download_coordinator_tests.cpp
    src/downloads/DownloadCoordinator.cpp
    src/downloads/DownloadJobs.cpp
    src/downloads/DownloadUrlPolicy.cpp
    src/AtomicFile.cpp
    src/storage/StoragePolicy.cpp
)
target_include_directories(download_coordinator_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(download_coordinator_tests PRIVATE cxx_std_23)
target_link_libraries(download_coordinator_tests PRIVATE Threads::Threads)
asobmashow_register_test(download_coordinator_tests)

add_executable(download_publication_tests
    tests/download_publication_tests.cpp
    src/downloads/DownloadPublication.cpp
    src/AtomicFile.cpp
)
target_include_directories(download_publication_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(download_publication_tests PRIVATE cxx_std_23)
asobmashow_register_test(download_publication_tests)
