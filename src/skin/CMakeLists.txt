target_sources(main PRIVATE
    DefaultSkin.cpp
    SkinProfileSettings.cpp
    SkinCommitCoordinator.cpp
    SkinStoragePaths.cpp
    beatoraja/LuaSkinFileSystem.cpp
    beatoraja/SkinCompatibilityDiagnostics.cpp
    package/SkinAliasDetector.cpp
    package/SkinArchiveImporter.cpp
    package/SkinPackageCatalog.cpp
    package/SkinPackageOperationService.cpp
    package/SkinPackageStore.cpp
    package/SkinPathPolicy.cpp
    package/SkinTreeSnapshotter.cpp
)

if(APPLE)
    target_sources(main PRIVATE package/SkinAliasDetectorApple.mm)
endif()
