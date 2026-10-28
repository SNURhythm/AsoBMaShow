target_sources(main PRIVATE
    DisplaySettingsManager.cpp
    FramePacer.cpp
    RendererAccessCoordinator.cpp
    SDLDisplayBackend.cpp
    VideoDecodeState.cpp
    VideoFrameLayout.cpp
    VideoPlayer.cpp
)

if(APPLE)
    target_sources(main PRIVATE MetalDisplayLink.mm)
endif()
