target_sources(main PRIVATE
    DefaultSkin.cpp
    SkinProfileSettings.cpp
    SkinStoragePaths.cpp
    package/SkinAliasDetector.cpp
    package/SkinPathPolicy.cpp
    package/SkinTreeSnapshotter.cpp
)

if(APPLE)
    target_sources(main PRIVATE package/SkinAliasDetectorApple.mm)
endif()
