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
