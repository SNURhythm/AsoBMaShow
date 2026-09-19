# Both consumers exercise the same compiled registry and native backend stack.
add_library(input_registry_test_support STATIC
    src/input/InputDeviceIdentity.cpp
    src/input/InputDeviceRegistry.cpp
    src/input/SDLInputBackend.cpp
)
target_include_directories(input_registry_test_support PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(input_registry_test_support PUBLIC cxx_std_23)
asobmashow_add_midi_backend(input_registry_test_support)
asobmashow_add_gyroscope_backend(input_registry_test_support)
asobmashow_add_windows_realtime_input_backend(input_registry_test_support)
if(TARGET SDL2::SDL2)
    target_link_libraries(input_registry_test_support PUBLIC SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(input_registry_test_support PUBLIC SDL2::SDL2-static)
elseif(TARGET SDL2)
    target_link_libraries(input_registry_test_support PUBLIC SDL2)
endif()

add_executable(input_device_registry_tests tests/input_device_registry_tests.cpp)
target_link_libraries(input_device_registry_tests PRIVATE input_registry_test_support)

add_executable(realtime_gameplay_input_registration_tests
    tests/realtime_gameplay_input_registration_tests.cpp
    src/scene/play/RealtimeGameplayInputRegistration.cpp
)
target_link_libraries(realtime_gameplay_input_registration_tests PRIVATE
    input_registry_test_support Threads::Threads
)
