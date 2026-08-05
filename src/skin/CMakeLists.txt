target_sources(main PRIVATE
    DefaultSkin.cpp
    SkinProfileSettings.cpp
    SkinCommitCoordinator.cpp
    SkinStoragePaths.cpp
    beatoraja/LuaSkinFileSystem.cpp
    beatoraja/SkinCompatibilityDiagnostics.cpp
    beatoraja/SkinDiagnosticHistory.cpp
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
        GameplaySkinLifecycle.cpp
        beatoraja/GameplaySkinBuiltinCatalog.cpp
        beatoraja/GameplaySkinValidator.cpp
        beatoraja/BgfxSkinTextureDevice.cpp
        LuaGameplaySkinFeatureEnabled.cpp
        SkinConfigurationWriteQueue.cpp
        beatoraja/PlaySkinSession.cpp
        beatoraja/PlaySkinStateBridge.cpp
        beatoraja/LuaSkinHostModules.cpp
        beatoraja/LuaSkinBindingDecoder.cpp
        beatoraja/LuaSkinRuntime.cpp
        beatoraja/LuaSkinTableDecoder.cpp
        beatoraja/NumericGlyphAtlas.cpp
        beatoraja/PlaySkinViewport.cpp
        beatoraja/Skin2DRenderer.cpp
        beatoraja/Skin2DRendererSubmit.cpp
        beatoraja/SkinDestinationEvaluator.cpp
        beatoraja/SkinGaugeNodeExpansion.cpp
        beatoraja/SkinJudgeNormalization.cpp
        beatoraja/SkinJudgeNumberNormalization.cpp
        beatoraja/SkinNoteNormalization.cpp
        beatoraja/SkinNoteLaneGeometryNormalization.cpp
        beatoraja/SkinNoteLineNormalization.cpp
        beatoraja/SkinTextGraphNormalization.cpp
        beatoraja/SkinCoverNormalization.cpp
        beatoraja/SkinObjectResolutionPrecedence.cpp
        beatoraja/SkinPerformanceTelemetry.cpp
        beatoraja/SkinOverlayDigestProvider.cpp
        beatoraja/SkinProcessResidentMemory.cpp
        beatoraja/SkinModelValidator.cpp
        beatoraja/SkinResourceCatalog.cpp
        beatoraja/SkinTextAtlas.cpp
    )
else()
    target_sources(main PRIVATE LuaGameplaySkinFeatureUnavailable.cpp)
endif()

if(APPLE)
    target_sources(main PRIVATE package/SkinAliasDetectorApple.mm)
endif()
