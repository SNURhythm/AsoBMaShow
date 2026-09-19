set(library_picker_lifecycle_source
    ${CMAKE_CURRENT_BINARY_DIR}/generated/library_picker_lifecycle_tests.cpp)
add_custom_command(
    OUTPUT ${library_picker_lifecycle_source}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/library_picker_lifecycle_extract.py
            --root ${CMAKE_SOURCE_DIR} --output ${library_picker_lifecycle_source}
    DEPENDS tests/library_picker_lifecycle_extract.py tests/gameplay_terminal_scene_extract.py
            tests/library_picker_lifecycle_fixture.cpp
            src/library/ChartLibraryPlatform.cpp src/library/ChartLibraryPlatform.h
    VERBATIM
)
add_custom_target(library_picker_lifecycle_fixture DEPENDS ${library_picker_lifecycle_source})

# Exercise both mobile request branches with controlled native picker effects.
foreach(picker_platform IN ITEMS ios android)
    set(picker_target library_picker_${picker_platform}_tests)
    add_executable(${picker_target} ${library_picker_lifecycle_source})
    add_dependencies(${picker_target} library_picker_lifecycle_fixture)
    target_include_directories(${picker_target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(${picker_target} PRIVATE cxx_std_23)
    target_link_libraries(${picker_target} PRIVATE Threads::Threads)
    if(picker_platform STREQUAL "ios")
        target_compile_definitions(${picker_target} PRIVATE ASOBMASHOW_PICKER_TEST_IOS=1)
    else()
        target_compile_definitions(${picker_target} PRIVATE ASOBMASHOW_PICKER_TEST_IOS=0)
    endif()
    asobmashow_register_test(${picker_target}
        ${picker_platform}_sound_picker_startup_tests sound-startup)
    asobmashow_register_test(${picker_target}
        ${picker_platform}_sound_picker_admission_tests sound-admission)
    asobmashow_register_test(${picker_target}
        ${picker_platform}_library_picker_startup_tests folder-startup)
    set_tests_properties(
        ${picker_platform}_sound_picker_startup_tests
        ${picker_platform}_sound_picker_admission_tests
        ${picker_platform}_library_picker_startup_tests
        PROPERTIES TIMEOUT 15)
endforeach()
