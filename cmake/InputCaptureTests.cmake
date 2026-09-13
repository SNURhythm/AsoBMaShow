set(input_capture_controller_sources
    src/input/InputCaptureController.cpp
    src/input/InputBindingResolver.cpp
    src/input/InputDefaults.cpp
    src/input/InputProfile.cpp
    src/input/VirtualControllerConfig.cpp
)

add_executable(input_capture_controller_tests
    tests/input_capture_controller_tests.cpp
    ${input_capture_controller_sources}
    src/input/InputDeviceIdentity.cpp
    src/input/InputDeviceRegistry.cpp
    src/input/SDLInputBackend.cpp
)
asobmashow_add_midi_backend(input_capture_controller_tests)
asobmashow_add_gyroscope_backend(input_capture_controller_tests)
asobmashow_add_windows_realtime_input_backend(input_capture_controller_tests)

# This executable supplies a failing registry boundary and retains the real
# controller, resolver, profile, and configuration implementations.
add_executable(input_capture_startup_failure_tests
    tests/input_capture_startup_failure_tests.cpp
    ${input_capture_controller_sources}
    src/input/GyroscopeTurntable.cpp
)

foreach(input_capture_target IN ITEMS
    input_capture_controller_tests input_capture_startup_failure_tests)
    target_include_directories(${input_capture_target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(${input_capture_target} PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(${input_capture_target} PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(${input_capture_target} PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(${input_capture_target} PRIVATE SDL2)
    endif()
endforeach()
