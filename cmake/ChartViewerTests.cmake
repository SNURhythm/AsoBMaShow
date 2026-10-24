# Chart Viewer geometry and scene-owned listening lifecycle.
add_executable(chart_viewer_note_geometry_tests
    tests/chart_viewer_note_geometry_tests.cpp
)
target_include_directories(chart_viewer_note_geometry_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(chart_viewer_note_geometry_tests PRIVATE cxx_std_23)


set(chart_viewer_lifecycle_source
    ${CMAKE_CURRENT_BINARY_DIR}/generated/chart_viewer_lifecycle_tests.cpp)
add_custom_command(
    OUTPUT ${chart_viewer_lifecycle_source}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/chart_viewer_lifecycle_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${chart_viewer_lifecycle_source}
    DEPENDS tests/chart_viewer_lifecycle_extract.py tests/gameplay_terminal_scene_extract.py
            tests/chart_viewer_lifecycle_fixture.cpp
            src/scene/ChartViewerScene.cpp src/scene/Scene.h
    VERBATIM
)
add_executable(chart_viewer_lifecycle_tests ${chart_viewer_lifecycle_source})
target_compile_features(chart_viewer_lifecycle_tests PRIVATE cxx_std_23)
target_link_libraries(chart_viewer_lifecycle_tests PRIVATE Threads::Threads)

add_executable(chart_source_tests tests/chart_source_tests.cpp src/bms_parser.cpp)
target_include_directories(chart_source_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(chart_source_tests PRIVATE cxx_std_23)
target_compile_definitions(chart_source_tests PRIVATE BMS_PARSER_VERBOSE=0)

add_executable(native_document_tests tests/native_document_tests.cpp src/platform/NativeDocument.cpp)
target_include_directories(native_document_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(native_document_tests PRIVATE cxx_std_23)
target_compile_definitions(native_document_tests PRIVATE ASOBMASHOW_NATIVE_DOCUMENT_TEST_STUB=1)
add_test(NAME native_document_tests COMMAND native_document_tests)

if(APPLE AND NOT IOS)
    add_executable(native_document_apple_tests tests/native_document_apple_tests.mm
        src/platform/NativeDocumentApple.mm src/platform/NativeDocument.cpp)
    target_include_directories(native_document_apple_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(native_document_apple_tests PRIVATE cxx_std_23)
    set_source_files_properties(tests/native_document_apple_tests.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_definitions(native_document_apple_tests PRIVATE ASOBMASHOW_NATIVE_DOCUMENT_TEST_STUB=1)
    target_link_libraries(native_document_apple_tests PRIVATE SDL3::SDL3 Threads::Threads "-framework Cocoa")
    add_test(NAME native_document_apple_tests COMMAND native_document_apple_tests)
endif()

if(WIN32)
    add_executable(native_document_windows_tests tests/native_document_windows_tests.cpp
        src/platform/NativeDocumentWindows.cpp src/platform/NativeDocument.cpp)
    target_include_directories(native_document_windows_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(native_document_windows_tests PRIVATE cxx_std_23)
    target_compile_definitions(native_document_windows_tests PRIVATE ASOBMASHOW_NATIVE_DOCUMENT_TEST_STUB=1)
    target_link_libraries(native_document_windows_tests PRIVATE SDL3::SDL3 Threads::Threads
        user32 comctl32 uuid ole32 oleaut32)
    add_test(NAME native_document_windows_tests COMMAND native_document_windows_tests)
endif()
