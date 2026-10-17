target_sources(main PRIVATE StoragePolicy.cpp ChartResourceInventory.cpp)
target_sources(main PRIVATE StoragePlatform.cpp StorageFileTransaction.cpp)
target_sources(main PRIVATE AudioTranscoder.cpp)
target_sources(main PRIVATE VideoCompressionGeometry.cpp)
target_sources(main PRIVATE BgaTranscoder.cpp)
target_sources(main PRIVATE StorageOptimizer.cpp)
target_sources(main PRIVATE StorageTask.cpp StorageMaintenanceGuard.cpp)
target_sources(main PRIVATE StorageApplicationIntegration.cpp)

target_sources(main PRIVATE StorageInventoryTask.cpp)

target_sources(main PRIVATE StoragePathIndex.cpp StorageDirectoryInventory.cpp StorageScanScheduler.cpp)
target_sources(main PRIVATE StorageMeasurement.cpp StorageArchiveMeasurement.cpp)
