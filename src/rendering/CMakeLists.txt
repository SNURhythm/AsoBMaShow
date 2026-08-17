target_sources(main PRIVATE common.cpp Color.cpp Camera.cpp BlurPass.cpp PostProcessPipeline.cpp RenderPlan.cpp UniformCache.cpp SimpleBatchRenderer.cpp TexBatchRenderer.cpp UiBatchRenderer.cpp)
if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS)
    target_sources(main PRIVATE SkinQuadBatchRenderer.cpp)
endif()
