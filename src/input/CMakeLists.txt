target_sources(main PRIVATE
    SDLInputSource.cpp
    RhythmInputHandler.cpp
    InputNormalizer.cpp
    SDLTouchInputSource.cpp
    InputProfile.cpp
    InputDefaults.cpp
    InputProfileStore.cpp
    InputBindingResolver.cpp
    InputCaptureController.cpp
    InputDeviceIdentity.cpp
    InputDeviceRegistry.cpp
    SDLInputBackend.cpp
    LogicalGameplayInputAdapter.cpp
    RealtimePhysicalInputRouter.cpp
)

function(asobmashow_add_midi_backend target_name)
    target_sources(${target_name} PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/LiveMidiDeviceIdAllocator.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MidiMessageParser.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/NativeCallbackLifetime.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/QueuedMidiInputBackend.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Utf16ToUtf8.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MidiInputBackendFactory.cpp
    )

    if(APPLE)
        target_sources(${target_name} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CoreMidiInputBackend.mm
        )
        target_link_libraries(${target_name} PRIVATE ${CORE_MIDI_FRAMEWORK})
    elseif(WIN32)
        target_sources(${target_name} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WinMidiInputBackend.cpp
        )
        target_link_libraries(${target_name} PRIVATE winmm)
    elseif(ANDROID)
        target_sources(${target_name} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AndroidMidiInputBackend.cpp
        )
    else()
        target_sources(${target_name} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/UnsupportedMidiInputBackend.cpp
        )
    endif()
endfunction()

function(asobmashow_add_gyroscope_backend target_name)
    target_sources(${target_name} PRIVATE
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GyroscopeTurntable.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GyroscopeInputBackendCore.cpp
        ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GyroscopeInputBackendFactory.cpp
    )

    if(IOS)
        target_sources(${target_name} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/IOSGyroscopeInputBackend.mm
        )
        target_link_libraries(${target_name} PRIVATE ${CORE_MOTION_FRAMEWORK})
    elseif(ANDROID)
        target_sources(${target_name} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AndroidGyroscopeInputBackend.cpp
        )
    else()
        target_sources(${target_name} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/UnsupportedGyroscopeInputBackend.cpp
        )
    endif()
endfunction()

function(asobmashow_add_windows_realtime_input_backend target_name)
    if(WIN32)
        target_sources(${target_name} PRIVATE
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WindowsRealtimeInputBackend.cpp
            ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WindowsRealtimeInputMapping.cpp
        )
        target_link_libraries(${target_name} PRIVATE rtworkq user32)
    endif()
endfunction()

if(APPLE)
    find_library(CORE_MIDI_FRAMEWORK CoreMIDI REQUIRED)
endif()

if(IOS)
    find_library(CORE_MOTION_FRAMEWORK CoreMotion REQUIRED)
endif()

asobmashow_add_midi_backend(main)
asobmashow_add_gyroscope_backend(main)
asobmashow_add_windows_realtime_input_backend(main)
