target_sources(main PRIVATE StoragePolicy.cpp ChartResourceInventory.cpp)
target_sources(main PRIVATE StoragePlatform.cpp StorageFileTransaction.cpp)
target_sources(main PRIVATE AudioTranscoder.cpp)
target_sources(main PRIVATE VideoCompressionGeometry.cpp)
target_sources(main PRIVATE BgaTranscoder.cpp StorageVideoEncoder.cpp StorageVideoCapabilities.cpp)
target_sources(main PRIVATE StorageOptimizer.cpp StorageDirectoryEvidence.cpp StorageWorkScheduler.cpp)
target_sources(main PRIVATE StorageTask.cpp StorageMaintenanceGuard.cpp)
target_sources(main PRIVATE StorageApplicationIntegration.cpp)

target_sources(main PRIVATE StorageInventoryTask.cpp)

target_sources(main PRIVATE StoragePathIndex.cpp StorageDirectoryInventory.cpp StorageScanScheduler.cpp)
target_sources(main PRIVATE StorageMeasurement.cpp StorageArchiveMeasurement.cpp)

target_sources(main PRIVATE StoragePreflight.cpp StorageDirectoryWorkingSet.cpp)

target_sources(main PRIVATE StorageMeasurementState.cpp)

find_package(ZLIB REQUIRED)
target_sources(main PRIVATE StorageArchive.cpp StorageArchiveIO.cpp StorageArchivePreflight.cpp)
target_link_libraries(main PRIVATE ZLIB::ZLIB)
target_sources(main PRIVATE StorageArchiveTransaction.cpp)
