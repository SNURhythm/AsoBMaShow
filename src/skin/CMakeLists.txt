target_sources(main PRIVATE
    DefaultSkin.cpp
    SkinProfileSettings.cpp
    SkinCommitCoordinator.cpp
    SkinStoragePaths.cpp
    beatoraja/LuaSkinFileSystem.cpp
    beatoraja/SkinCompatibilityDiagnostics.cpp
    beatoraja/SkinDiagnosticHistory.cpp
    beatoraja/GameplaySkinSourceFormat.cpp
    package/SkinAliasDetector.cpp
    package/SkinArchiveImporter.cpp
    package/SkinPackageCatalog.cpp
    package/SkinPackageOperationService.cpp
    package/SkinPackageStore.cpp
    package/SkinPathPolicy.cpp
    package/SkinTreeSnapshotter.cpp
)

if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS)
    target_compile_definitions(main PRIVATE
        ASOBMASHOW_ENABLE_SKIN_MOVIE_DEVICE=1)
    target_sources(main PRIVATE
        GameplaySkinLifecycle.cpp
        beatoraja/GameplaySkinBuiltinCatalog.cpp
        beatoraja/JsonGameplaySkinDecoder.cpp
        beatoraja/Lr2SkinCsvParser.cpp
        beatoraja/Lr2SkinHeaderDecoder.cpp
        beatoraja/GameplaySkinValidator.cpp
        beatoraja/BgfxSkinTextureDevice.cpp
        LuaGameplaySkinFeatureEnabled.cpp
        SkinConfigurationWriteQueue.cpp
        beatoraja/PlaySkinSession.cpp
        beatoraja/PlaySkinStateBridge.cpp
        beatoraja/PomyuCharaResource.cpp
        beatoraja/LuaSkinHostModules.cpp
        beatoraja/LuaSkinBindingDecoder.cpp
        beatoraja/LuaSkinRuntime.cpp
        beatoraja/LuaSkinTableDecoder.cpp
        beatoraja/NumericGlyphAtlas.cpp
        beatoraja/PlaySkinViewport.cpp
        beatoraja/Skin2DRenderer.cpp
        beatoraja/Skin2DRendererSubmit.cpp
        beatoraja/SyntheticReplayGhostOverlay.cpp
        beatoraja/SkinDestinationEvaluator.cpp
        beatoraja/SkinGaugeNodeExpansion.cpp
        beatoraja/SkinJudgeNormalization.cpp
        beatoraja/SkinJudgeNumberNormalization.cpp
        beatoraja/SkinNoteNormalization.cpp
        beatoraja/SkinNoteLaneGeometryNormalization.cpp
        beatoraja/SkinNoteLineNormalization.cpp
        beatoraja/SkinBpmGraphRenderer.cpp
        beatoraja/SkinBitmapFontParser.cpp
        beatoraja/SkinGaugeGraphRenderer.cpp
        beatoraja/SkinNoteDistributionGraphRenderer.cpp
        beatoraja/SkinHitErrorVisualizerRenderer.cpp
        beatoraja/SkinTimingVisualizerRenderer.cpp
        beatoraja/SkinTextGraphNormalization.cpp
        beatoraja/SkinCoverNormalization.cpp
        beatoraja/SkinObjectResolutionPrecedence.cpp
        beatoraja/SkinPerformanceTelemetry.cpp
        beatoraja/SkinOverlayDigestProvider.cpp
        beatoraja/SkinProcessResidentMemory.cpp
        beatoraja/SkinModelValidator.cpp
        beatoraja/SkinMovieCatalog.cpp
        beatoraja/SkinMovieCatalogFrame.cpp
        beatoraja/SkinResourceCatalog.cpp
        beatoraja/SkinTextAtlas.cpp
    )
else()
    target_sources(main PRIVATE LuaGameplaySkinFeatureUnavailable.cpp)
endif()

if(APPLE)
    target_sources(main PRIVATE package/SkinAliasDetectorApple.mm)
endif()
