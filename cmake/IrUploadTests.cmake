# IR selection, preparation lifetime, and application-thread scene handoff.
set(ir_upload_scene_methods
    ${CMAKE_CURRENT_BINARY_DIR}/generated/ir_upload_scene_methods.inc)
add_custom_command(
    OUTPUT ${ir_upload_scene_methods}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/ir_upload_scene_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${ir_upload_scene_methods}
    DEPENDS tests/ir_upload_scene_extract.py tests/gameplay_terminal_scene_extract.py
            src/scene/IrUploadsScene.cpp
    VERBATIM
)
add_executable(ir_uploads_controller_tests
    tests/ir_uploads_controller_tests.cpp
    ${ir_upload_scene_methods}
    src/ir/IrSavedResultBatchUpload.cpp
    src/scene/IrUploadPreparationTask.cpp
    src/scene/IrUploadsController.cpp
    src/ir/IrOutboxModels.cpp
    src/ScoreProvenance.cpp
    src/scene/play/GameplayGaugeRules.cpp
    src/scene/play/GameplayJudgeRules.cpp
    src/scene/play/Judge.cpp
    src/Uuid.cpp
)
target_include_directories(ir_uploads_controller_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_features(ir_uploads_controller_tests PRIVATE cxx_std_23)
target_link_libraries(ir_uploads_controller_tests PRIVATE Threads::Threads)

