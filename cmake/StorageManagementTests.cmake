add_executable(storage_policy_tests tests/storage_policy_tests.cpp
    src/storage/StoragePolicy.cpp src/storage/VideoCompressionGeometry.cpp)
target_compile_features(storage_policy_tests PRIVATE cxx_std_23)
target_link_libraries(storage_policy_tests PRIVATE ${FFMPEG_LIBRARIES})
asobmashow_register_test(storage_policy_tests)

add_executable(storage_inventory_tests tests/storage_inventory_tests.cpp
    src/storage/StoragePathIndex.cpp src/storage/StorageDirectoryInventory.cpp
    src/storage/ChartResourceInventory.cpp src/storage/StoragePolicy.cpp
    src/audio/ChartVisualAssetResolver.cpp)
target_compile_features(storage_inventory_tests PRIVATE cxx_std_23)
asobmashow_register_test(storage_inventory_tests)

add_executable(storage_file_transaction_tests tests/storage_file_transaction_tests.cpp
    src/storage/StorageFileTransaction.cpp src/storage/StoragePlatform.cpp
    src/storage/ChartResourceInventory.cpp src/storage/StoragePolicy.cpp
    src/audio/ChartVisualAssetResolver.cpp src/AtomicFile.cpp src/FileChecksum.cpp src/Uuid.cpp)
target_compile_features(storage_file_transaction_tests PRIVATE cxx_std_23)
if(WIN32)
    target_link_libraries(storage_file_transaction_tests PRIVATE advapi32)
endif()
asobmashow_register_test(storage_file_transaction_tests)

add_executable(storage_media_tests tests/storage_media_tests.cpp src/storage/AudioTranscoder.cpp
    src/storage/VideoCompressionGeometry.cpp src/storage/StoragePolicy.cpp
    src/storage/StorageVideoEncoder.cpp src/storage/StorageVideoCapabilities.cpp
    src/audio/ChartVisualAssetResolver.cpp)
target_compile_features(storage_media_tests PRIVATE cxx_std_23)
target_link_libraries(storage_media_tests PRIVATE SndFile::sndfile ${FFMPEG_LIBRARIES})
asobmashow_register_test(storage_media_tests)

add_executable(storage_optimizer_tests tests/storage_optimizer_tests.cpp
    tests/storage_archive_tests.cpp
    tests/storage_directory_inventory_tests.cpp
    src/storage/StorageArchiveTransaction.cpp
    tests/storage_library_integration.cpp
    src/storage/StorageMeasurement.cpp src/storage/StorageArchiveMeasurement.cpp src/storage/StorageMeasurementState.cpp
    src/audio/decoder.cpp src/ArchiveFile.cpp src/archive/TemporaryCache.cpp
    src/archive/UnzipOutput.cpp src/archive/IndexBuildCoordinator.cpp
    src/MinizBridge.c src/Utils.cpp src/bms_parser.cpp src/path.cpp
    src/bms_search/DownloadStorageOptimization.cpp
    src/storage/StorageOptimizer.cpp src/storage/StorageDirectoryEvidence.cpp src/storage/StorageWorkScheduler.cpp src/storage/StoragePreflight.cpp src/storage/StorageDirectoryWorkingSet.cpp
    src/storage/StorageDirectoryInventory.cpp src/storage/StoragePathIndex.cpp src/storage/StorageScanScheduler.cpp
    src/ChartScanWorkScheduler.cpp src/storage/AudioTranscoder.cpp src/storage/BgaTranscoder.cpp
    src/storage/VideoCompressionGeometry.cpp src/storage/StoragePolicy.cpp
    src/storage/StorageVideoEncoder.cpp src/storage/StorageVideoCapabilities.cpp
    src/storage/ChartResourceInventory.cpp src/storage/StorageFileTransaction.cpp src/storage/StoragePlatform.cpp
    src/audio/ChartVisualAssetResolver.cpp src/AtomicFile.cpp src/FileChecksum.cpp src/Uuid.cpp)
target_compile_features(storage_optimizer_tests PRIVATE cxx_std_23)
target_link_libraries(storage_optimizer_tests PRIVATE ${COMMON_LIBS} SQLite3)
if(APPLE)
    target_link_libraries(storage_optimizer_tests PRIVATE iconv)
endif()
if(WIN32)
    target_link_libraries(storage_optimizer_tests PRIVATE shell32 ole32 advapi32)
endif()
asobmashow_register_test(storage_optimizer_tests)
add_test(NAME storage_archive_tests COMMAND storage_optimizer_tests --archive)
set_tests_properties(storage_archive_tests PROPERTIES TIMEOUT 60)

add_executable(storage_task_tests tests/storage_task_tests.cpp
    src/storage/StorageTask.cpp src/storage/StorageMaintenanceGuard.cpp)
target_compile_features(storage_task_tests PRIVATE cxx_std_23)
asobmashow_register_test(storage_task_tests)

add_executable(grouped_chart_list_tests tests/grouped_chart_list_tests.cpp
    src/library/GroupedChartListModel.cpp src/library/StorageArchiveList.cpp src/path.cpp)
target_compile_features(grouped_chart_list_tests PRIVATE cxx_std_23)
target_link_libraries(grouped_chart_list_tests PRIVATE ${COMMON_LIBS})
asobmashow_register_test(grouped_chart_list_tests)

set(storage_music_admission_source ${CMAKE_CURRENT_BINARY_DIR}/generated/storage_music_admission_tests.cpp)
add_custom_command(OUTPUT ${storage_music_admission_source}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/storage_music_admission_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${storage_music_admission_source}
    DEPENDS tests/storage_music_admission_extract.py tests/storage_music_admission_fixture.cpp
            tests/gameplay_terminal_scene_extract.py src/audio/MusicPlayerService.cpp)
add_executable(storage_music_admission_tests ${storage_music_admission_source})
target_include_directories(storage_music_admission_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(storage_music_admission_tests PRIVATE cxx_std_23)
target_link_libraries(storage_music_admission_tests PRIVATE Threads::Threads)
asobmashow_register_test(storage_music_admission_tests)

target_sources(archive_file_concurrency_tests PRIVATE tests/storage_inventory_task_tests.cpp
    src/storage/StorageInventoryTask.cpp src/storage/StoragePreflight.cpp
    src/storage/StorageDirectoryWorkingSet.cpp src/FileChecksum.cpp)
add_test(NAME storage_inventory_task_tests COMMAND archive_file_concurrency_tests --storage-inventory-task)
set_tests_properties(storage_inventory_task_tests PROPERTIES TIMEOUT 20)

add_executable(storage_scan_scheduler_tests tests/storage_scan_scheduler_tests.cpp
    src/storage/StorageScanScheduler.cpp src/ChartScanWorkScheduler.cpp src/Uuid.cpp)
target_compile_features(storage_scan_scheduler_tests PRIVATE cxx_std_23)
target_link_libraries(storage_scan_scheduler_tests PRIVATE SQLite3 Threads::Threads)
asobmashow_register_test(storage_scan_scheduler_tests)
set_tests_properties(storage_scan_scheduler_tests PROPERTIES TIMEOUT 30)

# Reuse the existing archive test binary/backend objects for measurement integration.
target_sources(archive_file_concurrency_tests PRIVATE tests/storage_measurement_tests.cpp
    src/storage/StorageMeasurement.cpp src/storage/StorageArchiveMeasurement.cpp
    src/storage/StoragePathIndex.cpp src/storage/StorageDirectoryInventory.cpp
    src/storage/StorageScanScheduler.cpp src/ChartScanWorkScheduler.cpp src/Uuid.cpp
    src/storage/ChartResourceInventory.cpp src/storage/StoragePolicy.cpp src/audio/ChartVisualAssetResolver.cpp)
target_link_libraries(archive_file_concurrency_tests PRIVATE SQLite3)
add_test(NAME storage_measurement_tests COMMAND archive_file_concurrency_tests --storage-measurement)

# Preflight now uses the real archive index. Reuse its existing integration
# binary instead of adding another full archive backend to a standalone test.
add_custom_target(storage_directory_inventory_tests DEPENDS storage_optimizer_tests)
add_test(NAME storage_directory_inventory_tests COMMAND storage_optimizer_tests --directory-inventory)

add_executable(storage_measurement_state_tests tests/storage_measurement_state_tests.cpp
    src/storage/StorageMeasurementState.cpp src/storage/StoragePathIndex.cpp)
target_compile_features(storage_measurement_state_tests PRIVATE cxx_std_23)
asobmashow_register_test(storage_measurement_state_tests)

add_executable(storage_work_scheduler_tests tests/storage_work_scheduler_tests.cpp src/storage/StorageWorkScheduler.cpp)
target_compile_features(storage_work_scheduler_tests PRIVATE cxx_std_23)
target_link_libraries(storage_work_scheduler_tests PRIVATE Threads::Threads)
asobmashow_register_test(storage_work_scheduler_tests)
set_tests_properties(storage_work_scheduler_tests PROPERTIES TIMEOUT 30)

# Deliberately excluded from the default build/CTest. Compile production code
# separately with optimization even in the developer's Debug build directory.
if(UNIX)
get_target_property(storage_benchmark_sources storage_optimizer_tests SOURCES)
list(FILTER storage_benchmark_sources EXCLUDE REGEX "^tests/|src/audio/decoder.cpp|src/bms_search/DownloadStorageOptimization.cpp")
add_executable(storage_scale_benchmark EXCLUDE_FROM_ALL tests/storage_scale_benchmark.cpp
    ${storage_benchmark_sources} src/storage/StorageMeasurement.cpp src/storage/StorageArchiveMeasurement.cpp)
target_compile_features(storage_scale_benchmark PRIVATE cxx_std_23)
target_compile_options(storage_scale_benchmark PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-O2;-g0>)
target_link_libraries(storage_scale_benchmark PRIVATE ${COMMON_LIBS} SQLite3)
if(APPLE)
    target_link_libraries(storage_scale_benchmark PRIVATE iconv)
endif()
endif()

add_executable(storage_video_capability_tests tests/storage_video_capability_tests.cpp
    src/storage/StorageVideoCapabilities.cpp src/storage/StorageVideoEncoder.cpp src/storage/StoragePolicy.cpp)
target_compile_features(storage_video_capability_tests PRIVATE cxx_std_23)
target_link_libraries(storage_video_capability_tests PRIVATE ${FFMPEG_LIBRARIES} Threads::Threads)
asobmashow_register_test(storage_video_capability_tests)
