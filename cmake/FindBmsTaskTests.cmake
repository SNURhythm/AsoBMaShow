# Find BMS worker ownership without network or archive backend dependencies.
set(find_bms_scene_methods
    ${CMAKE_CURRENT_BINARY_DIR}/generated/find_bms_scene_methods.inc)
add_custom_command(
    OUTPUT ${find_bms_scene_methods}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/find_bms_scene_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${find_bms_scene_methods}
    DEPENDS tests/find_bms_scene_extract.py tests/gameplay_terminal_scene_extract.py
            src/scene/MainMenuScene.cpp
    VERBATIM
)
add_executable(find_bms_task_tests
    tests/find_bms_task_tests.cpp
    ${find_bms_scene_methods}
    src/scene/FindBmsDialogPolicy.cpp
    src/scene/FindBmsProgressPresentation.cpp
    src/scene/FindBmsTask.cpp
)
target_include_directories(find_bms_task_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(find_bms_task_tests PRIVATE cxx_std_23)
target_link_libraries(find_bms_task_tests PRIVATE Threads::Threads)
asobmashow_register_test(find_bms_task_tests)
