add_executable(storage_policy_tests tests/storage_policy_tests.cpp
    src/storage/StoragePolicy.cpp src/storage/VideoCompressionGeometry.cpp)
target_compile_features(storage_policy_tests PRIVATE cxx_std_23)
target_link_libraries(storage_policy_tests PRIVATE ${FFMPEG_LIBRARIES})
asobmashow_register_test(storage_policy_tests)

add_executable(storage_inventory_tests tests/storage_inventory_tests.cpp
    src/storage/ChartResourceInventory.cpp src/storage/StoragePolicy.cpp
    src/audio/ChartVisualAssetResolver.cpp)
target_compile_features(storage_inventory_tests PRIVATE cxx_std_23)
asobmashow_register_test(storage_inventory_tests)

add_executable(storage_file_transaction_tests tests/storage_file_transaction_tests.cpp
    src/storage/StorageFileTransaction.cpp src/storage/StoragePlatform.cpp
    src/storage/ChartResourceInventory.cpp src/storage/StoragePolicy.cpp
    src/audio/ChartVisualAssetResolver.cpp src/AtomicFile.cpp src/FileChecksum.cpp src/Uuid.cpp)
target_compile_features(storage_file_transaction_tests PRIVATE cxx_std_23)
asobmashow_register_test(storage_file_transaction_tests)

add_executable(storage_media_tests tests/storage_media_tests.cpp src/storage/AudioTranscoder.cpp
    src/storage/BgaTranscoder.cpp src/storage/VideoCompressionGeometry.cpp src/storage/StoragePolicy.cpp
    src/audio/ChartVisualAssetResolver.cpp)
target_compile_features(storage_media_tests PRIVATE cxx_std_23)
target_link_libraries(storage_media_tests PRIVATE SndFile::sndfile ${FFMPEG_LIBRARIES})
asobmashow_register_test(storage_media_tests)
