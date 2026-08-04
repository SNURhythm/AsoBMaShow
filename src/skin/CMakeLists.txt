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

if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS)
    target_sources(main PRIVATE
        LuaGameplaySkinFeatureEnabled.cpp
        beatoraja/LuaSkinHostModules.cpp
        beatoraja/LuaSkinRuntime.cpp
        beatoraja/LuaSkinTableDecoder.cpp
        beatoraja/SkinModelValidator.cpp
    )
else()
    target_sources(main PRIVATE LuaGameplaySkinFeatureUnavailable.cpp)
endif()

if(APPLE)
    target_sources(main PRIVATE package/SkinAliasDetectorApple.mm)
endif()
