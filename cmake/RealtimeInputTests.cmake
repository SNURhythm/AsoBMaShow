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
asobmashow_add_native_realtime_input_backend(input_registry_test_support)
if(TARGET SDL3::SDL3)
    target_link_libraries(input_registry_test_support PUBLIC SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
    target_link_libraries(input_registry_test_support PUBLIC SDL3::SDL3-static)
elseif(TARGET SDL3)
    target_link_libraries(input_registry_test_support PUBLIC SDL3)
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

add_executable(native_keyboard_input_tests tests/native_keyboard_input_tests.cpp)
target_link_libraries(native_keyboard_input_tests PRIVATE input_registry_test_support)
add_test(NAME native_keyboard_input_tests COMMAND native_keyboard_input_tests)

add_executable(latency_telemetry_tests tests/latency_telemetry_tests.cpp)
target_include_directories(latency_telemetry_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(latency_telemetry_tests PRIVATE Threads::Threads)
add_test(NAME latency_telemetry_tests COMMAND latency_telemetry_tests)
