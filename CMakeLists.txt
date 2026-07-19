cmake_minimum_required(VERSION 3.22)

include(ExternalProject)
project(AsoBMaShow VERSION 1.0 LANGUAGES C CXX)
include(CTest)
option(ASOBMASHOW_BUILD_TESTS "Build lightweight unit tests" ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Do not force CMAKE_OSX_DEPLOYMENT_TARGET here. Desktop builds link against
# local vcpkg static libraries, and forcing an older minimum macOS version than
# those libraries were built with emits one linker warning per archive object.
# Set MACOSX_DEPLOYMENT_TARGET or -DCMAKE_OSX_DEPLOYMENT_TARGET explicitly when
# building a release with matching dependency binaries.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Source directory
set(SRC_DIR src)
# Build directory
set(BUILD_DIR ${CMAKE_BINARY_DIR}/build)
# Libraries directory
set(LIB_DIR lib)

# Platform detection
if (APPLE AND NOT IOS)
    set(PLATFORM_MACOS ON)
else()
    set(PLATFORM_MACOS OFF)
endif()
if (ANDROID)
    set(PLATFORM_ANDROID ON)
else()
    set(PLATFORM_ANDROID OFF)
endif()
# Option to build macOS app bundle
if (PLATFORM_MACOS)
    option(BUILD_MACOS_BUNDLE "Build macOS app bundle" OFF)
    message(STATUS "Build macOS bundle: ${BUILD_MACOS_BUNDLE}")

    set(MACOS_BUNDLE_IDENTIFIER "com.SNURhythm.AsoBMaShow")
    set(MACOS_BUNDLE_NAME "AsoBMaShow")
    if (BUILD_MACOS_BUNDLE)
        set(MACOS_EXECUTABLE_NAME "${MACOS_BUNDLE_NAME}")
    else()
        set(MACOS_EXECUTABLE_NAME "main")
    endif()
    set(MACOS_INFO_PLIST "${CMAKE_BINARY_DIR}/Info.plist")
    configure_file(${CMAKE_SOURCE_DIR}/Info.plist ${MACOS_INFO_PLIST} @ONLY)
endif()
# Common Definitions
add_definitions(-DSOL_LUAJIT=1 -DNOMINMAX)
set(BX_CONFIG_DEBUG OFF CACHE BOOL "" FORCE)
set(BGFX_CONFIG_DEBUG OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# Include directories
include_directories(include)
include_directories(bgfx/bgfx/include)
include_directories(bgfx/bx/include)
include_directories(bgfx/bimg/include)
include_directories(bgfx/bgfx/3rdparty)
# Compatibility includes
if (MINGW)
    include_directories(bgfx/bx/include/compat/mingw)
elseif (UNIX AND NOT APPLE)
    include_directories(bgfx/bx/include/compat/linux)
elseif (PLATFORM_MACOS)
    include_directories(bgfx/bx/include/compat/osx)
elseif (IOS)
    include_directories(bgfx/bx/include/compat/ios)
elseif (MSVC)
    include_directories(bgfx/bx/include/compat/msvc)
endif ()

# Yoga
include_directories(yoga)
include_directories(yoga/lib)

# Compiler definitions for MSVC/others
if (MSVC)
    add_compile_options(/Zc:__cplusplus /Zc:preprocessor /wd4819 /utf-8)
    add_definitions(-DSTBI_WINDOWS_UTF8)
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_definitions(-DAMD64)
    else ()
        add_definitions(-DIA32)
    endif ()
else ()
    add_compile_options(-Wno-strict-prototypes -Wno-unused-but-set-variable -pedantic -Wno-ignored-attributes -Wstrict-aliasing -Wstrict-overflow -g)
endif ()


# ------------------------------------------------------------------------------
# Dependency Management
# ------------------------------------------------------------------------------

# 1. SndFile (Common)
find_package(SndFile CONFIG REQUIRED)
set(COMMON_LIBS SndFile::sndfile)

# 2. SDL2, SDL2_ttf, FFmpeg, Lua, LuaJIT
if (IOS)
    # --- iOS: Use Submodules ---
    message(STATUS "Configuring for iOS - Using submodules for SDL2/SDL_ttf")

    # Add subdirectories
    add_subdirectory(SDL)
    add_subdirectory(SDL_ttf)

    # Setup variables for linking
    set(SDL2_LIB SDL2)
    set(SDL2_TFF_LIB SDL2_ttf)
    # TODO: FFmpeg / Lua handling for iOS if needed

    find_library(UIKIT UIKit)
    find_library(FOUNDATION Foundation)
    find_library(CORE_GRAPHICS CoreGraphics)
    find_library(CORE_TEXT CoreText)
    find_library(PHOTOS Photos)
    list(APPEND COMMON_LIBS ${UIKIT} ${FOUNDATION} ${CORE_GRAPHICS}
        ${CORE_TEXT} ${PHOTOS} ${SDL2_LIB} ${SDL2_TFF_LIB})

elseif(ANDROID)
    # --- Android: SDL submodules + vcpkg-compatible native packages ---
    message(STATUS "Configuring for Android - Using submodules for SDL2/SDL_ttf")

    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    set(SDL2TTF_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL2TTF_SAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL2TTF_VENDORED ON CACHE BOOL "" FORCE)

    add_subdirectory(SDL)
    add_subdirectory(SDL_ttf)

    list(APPEND COMMON_LIBS SDL2 SDL2main SDL2_ttf)

    find_package(FFMPEG REQUIRED)
    include_directories(${FFMPEG_INCLUDE_DIRS})
    list(APPEND COMMON_LIBS ${FFMPEG_LIBRARIES})

    find_package(CURL REQUIRED)
    list(APPEND COMMON_LIBS CURL::libcurl)

    find_package(LibArchive REQUIRED)
    if(TARGET LibArchive::LibArchive)
        list(APPEND COMMON_LIBS LibArchive::LibArchive)
    else()
        include_directories(${LibArchive_INCLUDE_DIRS})
        list(APPEND COMMON_LIBS ${LibArchive_LIBRARIES})
    endif()

    find_package(7zip CONFIG REQUIRED)
    list(APPEND COMMON_LIBS 7zip::7zip)

    find_package(unarr CONFIG REQUIRED)
    list(APPEND COMMON_LIBS unarr::unarr)

    find_library(ANDROID_LIBRARY android REQUIRED)
    find_library(LOG_LIBRARY log REQUIRED)
    find_library(EGL_LIBRARY EGL REQUIRED)
    find_library(GLESV2_LIBRARY GLESv2 REQUIRED)
    find_library(VULKAN_LIBRARY vulkan REQUIRED)
    list(APPEND COMMON_LIBS
        ${ANDROID_LIBRARY}
        ${LOG_LIBRARY}
        ${EGL_LIBRARY}
        ${GLESV2_LIBRARY}
        ${VULKAN_LIBRARY}
    )

else()
    # --- Desktop: Use Vcpkg ---
    message(STATUS "Configuring for Desktop - Using vcpkg packages")

    # SDL2
    find_package(SDL2 CONFIG REQUIRED)
    list(APPEND COMMON_LIBS
        $<TARGET_NAME_IF_EXISTS:SDL2::SDL2main>
        $<IF:$<TARGET_EXISTS:SDL2::SDL2>,SDL2::SDL2,SDL2::SDL2-static>
    )

    # SDL2_ttf
    # Try different package names as vcpkg naming can vary
    find_package(SDL2_ttf CONFIG REQUIRED)
    list(APPEND COMMON_LIBS
        $<IF:$<TARGET_EXISTS:SDL2_ttf::SDL2_ttf>,SDL2_ttf::SDL2_ttf,SDL2_ttf::SDL2_ttf-static>
    )

    # FFmpeg
    find_package(FFMPEG REQUIRED)
    include_directories(${FFMPEG_INCLUDE_DIRS})
    list(APPEND COMMON_LIBS ${FFMPEG_LIBRARIES})

    # PortAudio
    find_package(PortAudio CONFIG REQUIRED)
    if(TARGET PortAudio::PortAudio)
        list(APPEND COMMON_LIBS PortAudio::PortAudio)
    elseif(TARGET portaudio_static)
        list(APPEND COMMON_LIBS portaudio_static)
    elseif(TARGET portaudio)
        list(APPEND COMMON_LIBS portaudio)
    endif()

    # libcurl for desktop difficulty table downloads
    find_package(CURL REQUIRED)
    list(APPEND COMMON_LIBS CURL::libcurl)

    # libarchive for BMS archive downloads (.zip, .7z, .rar, .lzh, .tar.*)
    find_package(LibArchive REQUIRED)
    if(TARGET LibArchive::LibArchive)
        list(APPEND COMMON_LIBS LibArchive::LibArchive)
    else()
        include_directories(${LibArchive_INCLUDE_DIRS})
        list(APPEND COMMON_LIBS ${LibArchive_LIBRARIES})
    endif()

    # 7-Zip SDK backend for indexed reads of 7z/RAR/LZH-style archives.
    find_package(7zip CONFIG REQUIRED)
    list(APPEND COMMON_LIBS 7zip::7zip)

    # unarr gives direct offset-based reads for non-solid RAR4 archives.
    find_package(unarr CONFIG REQUIRED)
    list(APPEND COMMON_LIBS unarr::unarr)

    # # Fontconfig & LibASS (Required by FFmpeg[all-gpl] static build)
    # find_package(PkgConfig REQUIRED)

    # pkg_check_modules(FONTCONFIG REQUIRED fontconfig IMPORTED_TARGET)
    # list(APPEND COMMON_LIBS PkgConfig::FONTCONFIG)

    # pkg_check_modules(LIBASS REQUIRED libass IMPORTED_TARGET)
    # list(APPEND COMMON_LIBS PkgConfig::LIBASS)

    # Lua / LuaJIT
    # LuaJIT (Project requires LuaJIT specifically, FindLua might pick Lua 5.4)
    if (WIN32)
        # On Windows with vcpkg, LuaJIT is installed as lua51.lib (ABI version 5.1)
        # vcpkg sets CMAKE_PREFIX_PATH, so we can use find_library
        find_library(LUAJIT_LIB
            NAMES lua51
            PATHS ${CMAKE_PREFIX_PATH}
            PATH_SUFFIXES lib
            NO_DEFAULT_PATH
        )
        # Fallback: try vcpkg_installed directory
        if (NOT LUAJIT_LIB)
            find_library(LUAJIT_LIB
                NAMES lua51
                PATHS ${CMAKE_BINARY_DIR}/vcpkg_installed
                PATH_SUFFIXES lib x64-windows/lib
            )
        endif()
        find_path(LUAJIT_INCLUDE_DIR
            NAMES luajit.h
            PATHS ${CMAKE_PREFIX_PATH}
            PATH_SUFFIXES include/luajit
            NO_DEFAULT_PATH
        )
        if (NOT LUAJIT_INCLUDE_DIR)
            find_path(LUAJIT_INCLUDE_DIR
                NAMES luajit.h
                PATHS ${CMAKE_BINARY_DIR}/vcpkg_installed
                PATH_SUFFIXES include/luajit x64-windows/include/luajit
            )
        endif()
        if (LUAJIT_LIB AND LUAJIT_INCLUDE_DIR)
            include_directories(${LUAJIT_INCLUDE_DIR})
            list(APPEND COMMON_LIBS ${LUAJIT_LIB})
            message(STATUS "Found LuaJIT: ${LUAJIT_LIB}")
        else()
            message(FATAL_ERROR "LuaJIT not found. LUAJIT_LIB=${LUAJIT_LIB}, LUAJIT_INCLUDE_DIR=${LUAJIT_INCLUDE_DIR}")
        endif()
    else()
        # On Unix-like systems, use pkg-config
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LUAJIT REQUIRED luajit)
        include_directories(${LUAJIT_INCLUDE_DIRS})
        link_directories(${LUAJIT_LIBRARY_DIRS})
        list(APPEND COMMON_LIBS ${LUAJIT_LIBRARIES})
    endif()

    if (WIN32)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -mwindows")
    endif()
endif()

# ------------------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------------------

# Prefer the selected toolchain/vcpkg headers over stale host-wide installs.
# This is portable CMake behavior and avoids architecture-mismatched SDL
# headers on Apple Silicon without injecting compiler-specific -I flags.
set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)

add_library(TinyFileDialogs STATIC ${SRC_DIR}/tinyfiledialogs.c)
add_library(SQLite3 STATIC ${SRC_DIR}/sqlite3.c)

if (ANDROID)
    add_library(main SHARED)
elseif (PLATFORM_MACOS AND BUILD_MACOS_BUNDLE)
    add_executable(main MACOSX_BUNDLE)
    set_target_properties(main PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${MACOS_INFO_PLIST}
        MACOSX_BUNDLE_GUI_IDENTIFIER "${MACOS_BUNDLE_IDENTIFIER}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
        MACOSX_BUNDLE_BUNDLE_NAME "${MACOS_BUNDLE_NAME}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright © SNURhythm"
        OUTPUT_NAME "${MACOS_BUNDLE_NAME}"
    )
else()
    add_executable(main)
endif()

if (ANDROID)
    target_compile_definitions(main PRIVATE PLATFORM_ANDROID=1)
endif()

option(ENABLE_DEBUG_OPTIMIZED_BUILD
       "Use lightweight optimization in Debug builds for runtime profiling"
       ON)
if (ENABLE_DEBUG_OPTIMIZED_BUILD AND NOT MSVC)
    target_compile_options(main PRIVATE
        $<$<CONFIG:Debug>:-O1 -fno-omit-frame-pointer>
    )
endif()

if (PLATFORM_MACOS AND NOT BUILD_MACOS_BUNDLE)
    # Game Mode and several other macOS app capabilities are discovered from the
    # embedded Info.plist even for a plain executable.
    target_link_options(main PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${MACOS_INFO_PLIST}"
    )
endif()

if (PLATFORM_MACOS)
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries"
        ASOBMASHOW_HAS_NO_WARN_DUPLICATE_LIBRARIES)
    if (ASOBMASHOW_HAS_NO_WARN_DUPLICATE_LIBRARIES)
        target_link_options(main PRIVATE "LINKER:-no_warn_duplicate_libraries")
    endif()
endif()

# Linking
# Note: bgfx and yogacore are added via add_subdirectory later
target_link_libraries(main
    PRIVATE
    ${COMMON_LIBS}
    TinyFileDialogs
    SQLite3
    bgfx
    yogacore
)

if (PLATFORM_MACOS)
    # 7-Zip registers archive handlers through static initialization. With a
    # static lib, pull the RAR handler objects explicitly without loading the
    # whole SDK archive, which conflicts with libarchive's bundled PPMd code.
    set(SEVENZIP_RAR_SUPPORT_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/7zip-rar-support")
    set(SEVENZIP_RAR_CODECS_OBJECT
        "${SEVENZIP_RAR_SUPPORT_DIR}/RarCodecsRegister.cpp.o")
    add_custom_command(
        OUTPUT "${SEVENZIP_RAR_CODECS_OBJECT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SEVENZIP_RAR_SUPPORT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E rm -f "${SEVENZIP_RAR_CODECS_OBJECT}"
        COMMAND ${CMAKE_AR} -x "$<TARGET_FILE:7zip::7zip>" RarCodecsRegister.cpp.o
        WORKING_DIRECTORY "${SEVENZIP_RAR_SUPPORT_DIR}"
        COMMENT "Extracting 7-Zip RAR codec registration object"
        VERBATIM
    )
    set_source_files_properties("${SEVENZIP_RAR_CODECS_OBJECT}"
        PROPERTIES
        EXTERNAL_OBJECT TRUE
        GENERATED TRUE
    )
    target_sources(main PRIVATE "${SEVENZIP_RAR_CODECS_OBJECT}")
    target_link_options(main PRIVATE
        "LINKER:-u,__ZTVN8NArchive4NRar8CHandlerE"
        "LINKER:-u,__ZTVN8NArchive5NRar58CHandlerE"
    )
endif()

if (WIN32)
    target_link_libraries(main PRIVATE shell32 ole32 advapi32)
endif()

# Platform specific system libs for macOS
if (PLATFORM_MACOS)
    find_library(FOUNDATION Foundation)
    find_library(AUDIO_TOOLBOX AudioToolbox)
    find_library(VIDEO_TOOLBOX VideoToolbox)
    find_library(CORE_AUDIO CoreAudio)
    find_library(AV_KIT AVKit)
    find_library(CORE_IMAGE CoreImage)
    find_library(CORE_MEDIA CoreMedia)
    find_library(ACCELERATE Accelerate)
    find_library(OPENGL OpenGL)
    find_library(OPENCL OpenCL)
    find_library(COCOA Cocoa)
    find_library(METAL Metal)
    find_library(SECURITY Security)

    target_link_libraries(main PRIVATE
        ${FOUNDATION} ${AUDIO_TOOLBOX} ${VIDEO_TOOLBOX} ${CORE_AUDIO}
        ${AV_KIT} ${CORE_IMAGE} ${CORE_MEDIA} ${ACCELERATE}
        ${OPENGL} ${OPENCL} ${COCOA} ${METAL} ${SECURITY}
        bz2 z iconv
    )
endif()


# ------------------------------------------------------------------------------
# Post-Build Assets & Shaders
# ------------------------------------------------------------------------------

# Assets
if (ANDROID)
    # Gradle packages assets/ and shaders/ into the APK from sourceSets.
elseif (PLATFORM_MACOS AND BUILD_MACOS_BUNDLE)
    add_custom_command(
        TARGET main POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_DIR:main>/Contents/Resources
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_BUNDLE_DIR:main>/Contents/Resources/assets
    )
else()
    add_custom_command(
        TARGET main POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:main>/assets
    )
endif()

# Shaders
file(GLOB_RECURSE SHADER_FILES "${CMAKE_SOURCE_DIR}/shaders/*.*")

if (ANDROID)
    # Android reads packaged shader assets with SDL_RWFromFile.
elseif (PLATFORM_MACOS)
     # macOS (Metal)
    file(GLOB_RECURSE METAL_SHADER_FILES "${CMAKE_SOURCE_DIR}/shaders/metal/*.*")
    if (BUILD_MACOS_BUNDLE)
        add_custom_command(
            TARGET main POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_DIR:main>/Contents/Resources/shaders
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shaders/metal $<TARGET_BUNDLE_DIR:main>/Contents/Resources/shaders/metal
            DEPENDS ${METAL_SHADER_FILES}
        )
    else()
        add_custom_target(copy_shaders ALL
            DEPENDS ${METAL_SHADER_FILES}
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:main>/shaders
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shaders/metal $<TARGET_FILE_DIR:main>/shaders/metal
        )
        add_dependencies(main copy_shaders)
    endif()
else()
    # Other platforms (copy all)
    add_custom_target(copy_shaders ALL
        DEPENDS ${SHADER_FILES}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shaders $<TARGET_FILE_DIR:main>/shaders
    )
    add_dependencies(main copy_shaders)
endif()

# Installation
install(TARGETS main DESTINATION bin)
install(DIRECTORY assets DESTINATION bin)

# Subdirectories
add_subdirectory(bgfx)
add_subdirectory(src)
target_sources(main PRIVATE src/CourseIdentity.cpp)
if (ASOBMASHOW_BUILD_TESTS AND BUILD_TESTING)
    add_executable(ios_gyroscope_motion_adapter_tests
        tests/ios_gyroscope_motion_adapter_tests.cpp
    )
    target_include_directories(ios_gyroscope_motion_adapter_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ios_gyroscope_motion_adapter_tests PRIVATE cxx_std_23)

    add_executable(gyroscope_input_backend_core_tests
        tests/gyroscope_input_backend_core_tests.cpp
        src/input/GyroscopeInputBackendCore.cpp
        src/input/GyroscopeTurntable.cpp
    )
    target_include_directories(gyroscope_input_backend_core_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gyroscope_input_backend_core_tests PRIVATE cxx_std_23)

    add_subdirectory(yoga/yoga)
else()
    add_subdirectory(yoga/yoga)
endif()

if (ASOBMASHOW_BUILD_TESTS AND BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME result_persistence_flow_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_result_persistence_flow.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(result_persistence_flow_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME result_visual_layout_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_result_visual_layout.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(result_visual_layout_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME main_menu_ranking_flow_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_main_menu_ranking_flow.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(main_menu_ranking_flow_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME main_menu_settings_anchor_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_main_menu_settings_anchor.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(main_menu_settings_anchor_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME ir_ranking_detail_flow_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_ir_ranking_detail_flow.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(ir_ranking_detail_flow_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME records_ir_marker_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_records_ir_marker.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(records_ir_marker_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    add_executable(repository_boundary_tests
        tests/repository_boundary_tests.cpp
    )
    target_compile_features(repository_boundary_tests PRIVATE cxx_std_23)
    target_compile_definitions(repository_boundary_tests PRIVATE
        ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )

    add_executable(gyroscope_turntable_tests
        tests/gyroscope_turntable_tests.cpp
        src/input/GyroscopeTurntable.cpp
    )
    target_include_directories(gyroscope_turntable_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gyroscope_turntable_tests PRIVATE cxx_std_23)

    add_executable(prep_metronome_tests
        tests/prep_metronome_tests.cpp
        src/PrepMetronome.cpp
        src/bms_parser.cpp
    )
    target_include_directories(prep_metronome_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(prep_metronome_tests PRIVATE cxx_std_23)

    add_executable(preparation_plan_tests
        tests/preparation_plan_tests.cpp
        src/PreparationPlan.cpp
        src/PrepMetronome.cpp
        src/bms_parser.cpp
    )
    target_include_directories(preparation_plan_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(preparation_plan_tests PRIVATE cxx_std_23)

    add_executable(start_lane_indicator_geometry_tests
        tests/start_lane_indicator_geometry_tests.cpp
    )
    target_include_directories(start_lane_indicator_geometry_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(start_lane_indicator_geometry_tests PRIVATE cxx_std_23)

    add_executable(lane_cover_number_geometry_tests
        tests/lane_cover_number_geometry_tests.cpp
    )
    target_include_directories(lane_cover_number_geometry_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(lane_cover_number_geometry_tests PRIVATE cxx_std_23)

    add_executable(chart_viewer_note_geometry_tests
        tests/chart_viewer_note_geometry_tests.cpp
    )
    target_include_directories(chart_viewer_note_geometry_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(chart_viewer_note_geometry_tests PRIVATE cxx_std_23)

    add_executable(gameplay_scroll_geometry_tests
        tests/gameplay_scroll_geometry_tests.cpp
    )
    target_include_directories(gameplay_scroll_geometry_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_scroll_geometry_tests PRIVATE cxx_std_23)

    add_executable(gameplay_chart_entity_render_budget_tests
        tests/gameplay_chart_entity_render_budget_tests.cpp
    )
    target_include_directories(gameplay_chart_entity_render_budget_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_chart_entity_render_budget_tests PRIVATE
        cxx_std_23
    )

    add_executable(club_beat_tests
        tests/club_beat_tests.cpp
        src/audio/ClubBeat.cpp
        src/bms_parser.cpp
    )
    target_include_directories(club_beat_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(club_beat_tests PRIVATE cxx_std_23)

    add_executable(gbattle_tests
        tests/gbattle_tests.cpp
        src/bms_parser.cpp
        src/ScoreProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(gbattle_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(gbattle_tests PRIVATE cxx_std_23)

    add_executable(replay_summary_list_tests
        tests/replay_summary_list_tests.cpp
        src/ReplayResultStateBuilder.cpp
        src/ScoreProvenance.cpp
        src/bms_parser.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(replay_summary_list_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(replay_summary_list_tests PRIVATE cxx_std_23)

    add_executable(replay_record_filters_tests
        tests/replay_record_filters_tests.cpp
        src/bms_parser.cpp
    )
    target_include_directories(replay_record_filters_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(replay_record_filters_tests PRIVATE cxx_std_23)

    add_executable(course_identity_tests
        tests/course_identity_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ScoreProvenance.cpp
    )
    target_include_directories(course_identity_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(course_identity_tests PRIVATE cxx_std_23)

    add_executable(ir_driver_tests
        tests/ir_driver_tests.cpp
        src/ir/IrDriver.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrRankingModels.cpp
        src/ir/IrSubmission.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
    )
    target_include_directories(ir_driver_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(ir_driver_tests PRIVATE cxx_std_23)

    add_executable(tachi_batch_manual_tests
        tests/tachi_batch_manual_tests.cpp
        src/FileChecksum.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/tachi/TachiBatchManual.cpp
        src/ScoreProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/Uuid.cpp
    )
    target_include_directories(tachi_batch_manual_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(tachi_batch_manual_tests PRIVATE cxx_std_23)

    add_executable(tachi_driver_tests
        tests/tachi_driver_tests.cpp
        src/AtomicFile.cpp
        src/FileChecksum.cpp
        src/VersionedJson.cpp
        src/ir/IrDriver.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrRankingModels.cpp
        src/ir/IrSubmission.cpp
        src/ir/tachi/BokutachiCacheStore.cpp
        src/ir/tachi/TachiBatchManual.cpp
        src/ir/tachi/TachiDriver.cpp
        src/ir/tachi/TachiRankingParser.cpp
        src/ir/tachi/TachiResponseParser.cpp
        src/ScoreProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/Uuid.cpp
    )
    target_include_directories(tachi_driver_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(tachi_driver_tests PRIVATE cxx_std_23)

    add_executable(bokutachi_cache_store_tests
        tests/bokutachi_cache_store_tests.cpp
        src/AtomicFile.cpp
        src/VersionedJson.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/tachi/BokutachiCacheStore.cpp
    )
    target_include_directories(bokutachi_cache_store_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(bokutachi_cache_store_tests PRIVATE cxx_std_23)

    add_executable(tachi_ranking_parser_tests
        tests/tachi_ranking_parser_tests.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/tachi/TachiRankingParser.cpp
        src/Uuid.cpp
    )
    target_include_directories(tachi_ranking_parser_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(tachi_ranking_parser_tests PRIVATE cxx_std_23)

    add_executable(ir_submission_service_tests
        tests/ir_submission_service_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ir/IrDriver.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrRankingModels.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionService.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(ir_submission_service_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(ir_submission_service_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(ir_submission_service_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(ir_submission_service_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(ir_submission_service_tests PRIVATE SDL2)
    endif()

    add_executable(ir_ranking_service_tests
        tests/ir_ranking_service_tests.cpp
        src/ir/IrDriver.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrRankingModels.cpp
        src/ir/IrRankingService.cpp
        src/ir/IrSubmission.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
    )
    target_include_directories(ir_ranking_service_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(ir_ranking_service_tests PRIVATE cxx_std_23)

    add_executable(ir_settings_presentation_tests
        tests/ir_settings_presentation_tests.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrSettingsPresentation.cpp
        src/Uuid.cpp
    )
    target_include_directories(ir_settings_presentation_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_settings_presentation_tests PRIVATE cxx_std_23)

    add_executable(ir_result_presentation_tests
        tests/ir_result_presentation_tests.cpp
        src/ir/IrResultPresentation.cpp
        src/ScoreProvenance.cpp
    )
    target_include_directories(ir_result_presentation_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_result_presentation_tests PRIVATE cxx_std_23)

    add_executable(ir_ranking_modal_tests
        tests/ir_ranking_modal_tests.cpp
        src/ir/IrRankingModal.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/view/View.cpp
    )
    target_include_directories(ir_ranking_modal_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_ranking_modal_tests PRIVATE cxx_std_23)
    target_link_libraries(ir_ranking_modal_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )

    if (NOT IOS)
        add_executable(ir_http_client_tests
            tests/ir_http_client_tests.cpp
            src/ir/IrHttpClient.cpp
        )
        target_include_directories(ir_http_client_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(ir_http_client_tests PRIVATE cxx_std_23)
        target_link_libraries(ir_http_client_tests PRIVATE CURL::libcurl)
    endif()

    add_executable(replay_repository_tests
        tests/replay_repository_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/ir/IrOutboxModels.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(replay_repository_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(replay_repository_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(replay_repository_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(replay_repository_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(replay_repository_tests PRIVATE SDL2)
    endif()

    add_executable(music_playlist_repository_tests
        tests/music_playlist_repository_tests.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/bms_parser.cpp
        src/repositories/ChartStorageIdentity.cpp
        src/repositories/MusicPlaylistRepository.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(music_playlist_repository_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(music_playlist_repository_tests PRIVATE cxx_std_23)
    target_link_libraries(music_playlist_repository_tests PRIVATE ${COMMON_LIBS})
    if(APPLE)
        target_link_libraries(music_playlist_repository_tests PRIVATE iconv)
    endif()
    if(WIN32)
        target_link_libraries(music_playlist_repository_tests PRIVATE shell32 ole32)
    endif()

    add_executable(chart_repository_tests
        tests/chart_repository_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/bms_parser.cpp
        src/repositories/ChartRepository.cpp
        src/repositories/ChartScanStore.cpp
        src/repositories/ScoreRepositoryChartSession.cpp
        src/repositories/ChartRepositoryDifficulty.cpp
        src/repositories/ChartRepositoryQueries.cpp
        src/repositories/ChartStorageIdentity.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(chart_repository_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(chart_repository_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(chart_repository_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(chart_repository_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(chart_repository_tests PRIVATE SDL2)
    endif()
    target_link_libraries(chart_repository_tests PRIVATE ${COMMON_LIBS})
    if(APPLE)
        target_link_libraries(chart_repository_tests PRIVATE iconv)
    endif()
    if(WIN32)
        target_link_libraries(chart_repository_tests PRIVATE shell32 ole32)
    endif()

    add_executable(chart_library_scanner_tests
        tests/chart_library_scanner_tests.cpp
        src/ChartLibraryScanner.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/bms_parser.cpp
        src/repositories/ChartRepository.cpp
        src/repositories/ChartScanStore.cpp
        src/repositories/ScoreRepositoryChartSession.cpp
        src/repositories/ChartRepositoryDifficulty.cpp
        src/repositories/ChartRepositoryQueries.cpp
        src/repositories/ChartStorageIdentity.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(chart_library_scanner_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(chart_library_scanner_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(chart_library_scanner_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(chart_library_scanner_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(chart_library_scanner_tests PRIVATE SDL2)
    endif()
    target_link_libraries(chart_library_scanner_tests PRIVATE ${COMMON_LIBS})
    if(APPLE)
        target_link_libraries(chart_library_scanner_tests PRIVATE iconv)
    endif()
    if(WIN32)
        target_link_libraries(chart_library_scanner_tests PRIVATE shell32 ole32)
    endif()

    add_executable(difficulty_table_importer_tests
        tests/difficulty_table_importer_tests.cpp
        src/DifficultyTableImporter.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/bms_parser.cpp
        src/repositories/ChartRepository.cpp
        src/repositories/ChartScanStore.cpp
        src/repositories/ScoreRepositoryChartSession.cpp
        src/repositories/ChartRepositoryDifficulty.cpp
        src/repositories/ChartRepositoryQueries.cpp
        src/repositories/ChartStorageIdentity.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(difficulty_table_importer_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(difficulty_table_importer_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(difficulty_table_importer_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(difficulty_table_importer_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(difficulty_table_importer_tests PRIVATE SDL2)
    endif()
    target_link_libraries(difficulty_table_importer_tests PRIVATE ${COMMON_LIBS})
    if(APPLE)
        target_link_libraries(difficulty_table_importer_tests PRIVATE iconv)
    endif()
    if(WIN32)
        target_link_libraries(difficulty_table_importer_tests PRIVATE shell32 ole32)
    endif()

    add_executable(result_persistence_model_tests
        tests/result_persistence_model_tests.cpp
        src/ResultPersistenceModel.cpp
        src/Uuid.cpp
        src/FileChecksum.cpp
        src/ScoreProvenance.cpp
        src/Utils.cpp
        src/path.cpp
        src/bms_parser.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(result_persistence_model_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(result_persistence_model_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(result_persistence_model_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(result_persistence_model_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(result_persistence_model_tests PRIVATE SDL2)
    endif()
    if(WIN32)
        target_link_libraries(result_persistence_model_tests PRIVATE shell32 ole32)
    endif()

    add_executable(result_recall_builder_tests
        tests/result_recall_builder_tests.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/ResultRecallBuilder.cpp
        src/ReplayResultStateBuilder.cpp
        src/ResultPersistenceModel.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrOutboxModels.cpp
        src/FileChecksum.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/bms_parser.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(result_recall_builder_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(result_recall_builder_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(result_recall_builder_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(result_recall_builder_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(result_recall_builder_tests PRIVATE SDL2)
    endif()
    target_link_libraries(result_recall_builder_tests PRIVATE ${COMMON_LIBS})
    if(APPLE)
        target_link_libraries(result_recall_builder_tests PRIVATE iconv)
    endif()
    if(WIN32)
        target_link_libraries(result_recall_builder_tests PRIVATE shell32 ole32)
    endif()

    add_executable(result_persistence_coordinator_tests
        tests/result_persistence_coordinator_tests.cpp
        src/ResultPersistenceCoordinator.cpp
        src/ScoreProvenance.cpp
    )
    target_include_directories(result_persistence_coordinator_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(result_persistence_coordinator_tests PRIVATE
        cxx_std_23
    )

    add_executable(result_persistence_integration_tests
        tests/result_persistence_integration_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ir/IrDriver.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrRankingModels.cpp
        src/ir/IrSubmission.cpp
        src/ir/tachi/TachiBatchManual.cpp
        src/ir/tachi/TachiDriver.cpp
        src/ir/tachi/TachiRankingParser.cpp
        src/ir/tachi/TachiResponseParser.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/ResultPersistenceCoordinator.cpp
        src/ResultPersistenceModel.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/ScoreProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(result_persistence_integration_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(result_persistence_integration_tests PRIVATE
        cxx_std_23
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(result_persistence_integration_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(result_persistence_integration_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(result_persistence_integration_tests PRIVATE SDL2)
    endif()
    if(WIN32)
        target_link_libraries(result_persistence_integration_tests PRIVATE shell32 ole32)
    endif()

    add_executable(score_provenance_db_tests
        tests/score_provenance_db_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ResultPersistenceModel.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(score_provenance_db_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(score_provenance_db_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(score_provenance_db_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(score_provenance_db_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(score_provenance_db_tests PRIVATE SDL2)
    endif()

    add_executable(chart_record_filters_tests
        tests/chart_record_filters_tests.cpp
        src/bms_parser.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(chart_record_filters_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(chart_record_filters_tests PRIVATE cxx_std_23)

    add_executable(main_menu_library_tests
        tests/main_menu_library_tests.cpp
        src/repositories/ChartRepositoryQueries.cpp
        src/scene/MainMenuLibrary.cpp
        src/sqlite3.c
    )
    target_include_directories(main_menu_library_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(main_menu_library_tests PRIVATE cxx_std_23)
    target_compile_definitions(main_menu_library_tests PRIVATE
        ASOBMASHOW_FOLDER_QUERY_ONLY=1
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(main_menu_library_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(main_menu_library_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(main_menu_library_tests PRIVATE SDL2)
    endif()

    add_executable(chart_meta_index_order_tests
        tests/chart_meta_index_order_tests.cpp
        src/sqlite3.c
    )
    target_include_directories(chart_meta_index_order_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(chart_meta_index_order_tests PRIVATE cxx_std_23)

    add_executable(score_cache_query_tests
        tests/score_cache_query_tests.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(score_cache_query_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(score_cache_query_tests PRIVATE cxx_std_23)

    add_executable(app_database_initializer_tests
        tests/app_database_initializer_tests.cpp
    )
    target_include_directories(app_database_initializer_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(app_database_initializer_tests PRIVATE cxx_std_23)

    add_executable(application_startup_tests
        tests/application_startup_tests.cpp
        src/ApplicationStartup.cpp
    )
    target_include_directories(application_startup_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(application_startup_tests PRIVATE cxx_std_23)

    add_executable(application_result_recovery_tests
        tests/application_result_recovery_tests.cpp
        src/ApplicationResultRecovery.cpp
    )
    target_include_directories(application_result_recovery_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(application_result_recovery_tests PRIVATE cxx_std_23)

    add_executable(audio_video_settings_tests
        tests/audio_video_settings_tests.cpp
        src/AppSettings.cpp
        src/ir/IrProfileSettings.cpp
        src/Utils.cpp
        src/path.cpp
        src/settings/AudioVideoSettings.cpp
    )
    target_include_directories(audio_video_settings_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(audio_video_settings_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(audio_video_settings_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(audio_video_settings_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(audio_video_settings_tests PRIVATE SDL2)
    endif()
    if(WIN32)
        target_link_libraries(audio_video_settings_tests PRIVATE shell32 ole32)
    endif()

    add_executable(audio_mix_tests
        tests/audio_mix_tests.cpp
        src/audio/AudioMix.cpp
        src/audio/PrepMetronomeSound.cpp
        src/bms_parser.cpp
    )
    target_include_directories(audio_mix_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(audio_mix_tests PRIVATE cxx_std_23)

    add_executable(audio_wrapper_lifecycle_tests
        tests/audio_wrapper_lifecycle_tests.cpp
        src/audio/AudioBackend.cpp
        src/audio/AudioMix.cpp
        src/audio/AudioWrapper.cpp
        src/path.cpp
        src/utils/Stopwatch.cpp
    )
    target_include_directories(audio_wrapper_lifecycle_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(audio_wrapper_lifecycle_tests PRIVATE cxx_std_23)
    target_link_libraries(audio_wrapper_lifecycle_tests PRIVATE ${COMMON_LIBS})

    add_executable(jukebox_restore_tests
        tests/jukebox_restore_tests.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/Utils.cpp
        src/audio/AudioBackend.cpp
        src/audio/AudioDeviceManager.cpp
        src/audio/AudioMix.cpp
        src/audio/AudioWrapper.cpp
        src/audio/ClubBeat.cpp
        src/audio/Jukebox.cpp
        src/audio/PrepMetronomeSound.cpp
        src/audio/decoder.cpp
        src/bms_parser.cpp
        src/path.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/utils/Stopwatch.cpp
        src/video/VideoPlayer.cpp
    )
    target_include_directories(jukebox_restore_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(jukebox_restore_tests PRIVATE cxx_std_23)
    target_compile_definitions(jukebox_restore_tests PRIVATE
        ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(jukebox_restore_tests PRIVATE ${COMMON_LIBS} bgfx)
    if(APPLE)
        target_link_libraries(jukebox_restore_tests PRIVATE iconv)
    endif()
    if(WIN32)
        target_link_libraries(jukebox_restore_tests PRIVATE shell32 ole32)
    endif()

    add_executable(audio_device_manager_tests
        tests/audio_device_manager_tests.cpp
        src/audio/AudioDeviceManager.cpp
    )
    target_include_directories(audio_device_manager_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(audio_device_manager_tests PRIVATE cxx_std_23)

    add_executable(settings_audio_video_model_tests
        tests/settings_audio_video_model_tests.cpp
        src/scene/SettingsAudioVideoModel.cpp
        src/audio/AudioDeviceManager.cpp
        src/settings/AudioVideoSettings.cpp
        src/video/DisplaySettingsManager.cpp
        src/video/FramePacer.cpp
    )
    target_include_directories(settings_audio_video_model_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(settings_audio_video_model_tests PRIVATE cxx_std_23)

    add_executable(chart_filter_sort_panel_view_tests
        tests/chart_filter_sort_panel_view_tests.cpp
        src/bms_parser.cpp
    )
    target_include_directories(chart_filter_sort_panel_view_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(chart_filter_sort_panel_view_tests PRIVATE cxx_std_23)

    add_executable(dropdown_view_tests
        tests/dropdown_view_tests.cpp
        src/bms_parser.cpp
    )
    target_include_directories(dropdown_view_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(dropdown_view_tests PRIVATE cxx_std_23)

    add_executable(view_layout_tests
        tests/view_layout_tests.cpp
        src/ir/IrRankingModal.cpp
        src/rendering/Color.cpp
        src/view/View.cpp
        src/rendering/UniformCache.cpp
    )
    target_include_directories(view_layout_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(view_layout_tests PRIVATE cxx_std_23)
    target_link_libraries(view_layout_tests PRIVATE ${COMMON_LIBS} bgfx yogacore)

    add_executable(button_enabled_tests
        tests/button_enabled_tests.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/view/Button.cpp
        src/view/View.cpp
    )
    target_include_directories(button_enabled_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(button_enabled_tests PRIVATE cxx_std_23)
    target_link_libraries(button_enabled_tests PRIVATE ${COMMON_LIBS} bgfx yogacore)

    add_executable(play_options_panel_view_tests
        tests/play_options_panel_view_tests.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/SimpleBatchRenderer.cpp
        src/rendering/UniformCache.cpp
        src/view/Button.cpp
        src/view/DropdownView.cpp
        src/view/PlayOptionSectionView.cpp
        src/view/PlayOptionsPanelView.cpp
        src/view/ScrollView.cpp
        src/view/SnappedSlider.cpp
        src/view/TextInputBox.cpp
        src/view/TextView.cpp
        src/view/View.cpp
    )
    target_include_directories(play_options_panel_view_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(play_options_panel_view_tests PRIVATE cxx_std_23)
    target_link_libraries(play_options_panel_view_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )

    add_executable(score_provenance_tests
        tests/score_provenance_tests.cpp
        src/ScoreProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(score_provenance_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(score_provenance_tests PRIVATE cxx_std_23)

    add_executable(gameplay_ruleset_tests
        tests/gameplay_ruleset_tests.cpp
        src/ScoreProvenance.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(gameplay_ruleset_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(gameplay_ruleset_tests PRIVATE cxx_std_23)

    add_executable(gameplay_judge_rules_tests
        tests/gameplay_judge_rules_tests.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/CompiledGameplayJudge.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(gameplay_judge_rules_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_judge_rules_tests PRIVATE cxx_std_23)

    add_executable(gameplay_gauge_rules_tests
        tests/gameplay_gauge_rules_tests.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/bms_parser.cpp
    )
    target_include_directories(gameplay_gauge_rules_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_gauge_rules_tests PRIVATE cxx_std_23)

    add_executable(gameplay_ruleset_policy_tests
        tests/gameplay_ruleset_policy_tests.cpp
        src/bms_parser.cpp
        src/ScoreProvenance.cpp
        src/scene/play/CompiledGameplayJudge.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/GameplayRulesetPolicy.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(gameplay_ruleset_policy_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_ruleset_policy_tests PRIVATE cxx_std_23)

    add_executable(input_profile_tests
        tests/input_profile_tests.cpp
        src/input/GyroscopeTurntable.cpp
        src/input/InputProfile.cpp
        src/input/InputDefaults.cpp
        src/input/InputProfileStore.cpp
    )
    target_include_directories(input_profile_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(input_profile_tests PRIVATE cxx_std_23)
    target_compile_definitions(input_profile_tests PRIVATE INPUT_PROFILE_STORE_TESTING)
    if(TARGET SDL2::SDL2)
        target_link_libraries(input_profile_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(input_profile_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(input_profile_tests PRIVATE SDL2)
    endif()

    if (NOT IOS AND NOT PLATFORM_ANDROID)
        add_executable(platform_document_handoff_tests
            tests/platform_document_handoff_tests.cpp
            src/PlatformDocumentHandoff.cpp
        )
        target_include_directories(platform_document_handoff_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(platform_document_handoff_tests PRIVATE cxx_std_23)
        target_link_libraries(platform_document_handoff_tests PRIVATE TinyFileDialogs)
        if (WIN32)
            target_link_libraries(platform_document_handoff_tests PRIVATE advapi32)
        endif()

        add_executable(profile_export_staging_tests
            tests/profile_export_staging_tests.cpp
            src/ProfileExportStaging.cpp
            src/PlatformDocumentHandoff.cpp
        )
        target_include_directories(profile_export_staging_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(profile_export_staging_tests PRIVATE cxx_std_23)
        target_link_libraries(profile_export_staging_tests PRIVATE TinyFileDialogs)
        if (WIN32)
            target_link_libraries(profile_export_staging_tests PRIVATE advapi32)
        endif()
    endif()

    add_executable(input_binding_resolver_tests
        tests/input_binding_resolver_tests.cpp
        src/input/InputBindingResolver.cpp
    )
    target_include_directories(input_binding_resolver_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(input_binding_resolver_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(input_binding_resolver_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(input_binding_resolver_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(input_binding_resolver_tests PRIVATE SDL2)
    endif()

    add_executable(windows_realtime_input_mapping_tests
        tests/windows_realtime_input_mapping_tests.cpp
        src/input/WindowsRealtimeInputMapping.cpp
    )
    target_include_directories(windows_realtime_input_mapping_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(windows_realtime_input_mapping_tests PRIVATE
        cxx_std_23
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(windows_realtime_input_mapping_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(windows_realtime_input_mapping_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(windows_realtime_input_mapping_tests PRIVATE SDL2)
    endif()

    add_executable(input_timestamp_tests
        tests/input_timestamp_tests.cpp
    )
    target_include_directories(input_timestamp_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(input_timestamp_tests PRIVATE cxx_std_23)

    add_executable(input_capture_controller_tests
        tests/input_capture_controller_tests.cpp
        src/input/InputCaptureController.cpp
        src/input/InputBindingResolver.cpp
        src/input/InputDefaults.cpp
        src/input/InputDeviceIdentity.cpp
        src/input/InputDeviceRegistry.cpp
        src/input/InputProfile.cpp
        src/input/SDLInputBackend.cpp
    )
    target_include_directories(input_capture_controller_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(input_capture_controller_tests PRIVATE cxx_std_23)
    asobmashow_add_midi_backend(input_capture_controller_tests)
    asobmashow_add_gyroscope_backend(input_capture_controller_tests)
    asobmashow_add_windows_realtime_input_backend(input_capture_controller_tests)
    if(TARGET SDL2::SDL2)
        target_link_libraries(input_capture_controller_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(input_capture_controller_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(input_capture_controller_tests PRIVATE SDL2)
    endif()

    add_executable(app_settings_store_tests
        tests/app_settings_store_tests.cpp
        src/AppSettingsStore.cpp
        src/AtomicFile.cpp
        src/VersionedJson.cpp
        src/AppSettings.cpp
        src/ir/IrProfileSettings.cpp
        src/Utils.cpp
        src/path.cpp
        src/settings/AudioVideoSettings.cpp
    )
    target_include_directories(app_settings_store_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(app_settings_store_tests PRIVATE cxx_std_23)
    target_compile_definitions(app_settings_store_tests PRIVATE
        APP_SETTINGS_STORE_TESTING
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(app_settings_store_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(app_settings_store_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(app_settings_store_tests PRIVATE SDL2)
    endif()

    add_executable(ir_credential_store_tests
        tests/ir_credential_store_tests.cpp
        src/ir/IrCredentialStore.cpp
        src/AtomicFile.cpp
        src/VersionedJson.cpp
    )
    target_include_directories(ir_credential_store_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_credential_store_tests PRIVATE cxx_std_23)

    add_executable(practice_configuration_tests
        tests/practice_configuration_tests.cpp
        src/practice/PracticeConfiguration.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(practice_configuration_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(practice_configuration_tests PRIVATE cxx_std_23)

    add_executable(practice_rule_override_tests
        tests/practice_rule_override_tests.cpp
        src/ScoreProvenance.cpp
        src/bms_parser.cpp
        src/scene/play/Judge.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(practice_rule_override_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(practice_rule_override_tests PRIVATE cxx_std_23)

    add_executable(gameplay_score_state_tests
        tests/gameplay_score_state_tests.cpp
        src/bms_parser.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(gameplay_score_state_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_score_state_tests PRIVATE cxx_std_23)

    add_executable(practice_session_tests
        tests/practice_session_tests.cpp
        src/ScoreProvenance.cpp
        src/practice/PracticeSession.cpp
    )
    target_include_directories(practice_session_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(practice_session_tests PRIVATE cxx_std_23)

    add_executable(gameplay_practice_boundary_tests
        tests/gameplay_practice_boundary_tests.cpp
    )
    target_include_directories(gameplay_practice_boundary_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_practice_boundary_tests PRIVATE cxx_std_23)

    add_executable(gameplay_playback_startup_tests
        tests/gameplay_playback_startup_tests.cpp
        src/ScoreProvenance.cpp
        src/bms_parser.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(gameplay_playback_startup_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_playback_startup_tests PRIVATE cxx_std_23)

    add_executable(gameplay_practice_input_boundary_tests
        tests/gameplay_practice_input_boundary_tests.cpp
        src/ScoreProvenance.cpp
        src/bms_parser.cpp
        src/practice/PracticeAnalytics.cpp
        src/scene/play/Judge.cpp
        src/scene/play/CompiledGameplayJudge.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayNoteJudgeRole.cpp
        src/scene/play/GameplayCandidateRules.cpp
        src/scene/play/RhythmLaneInputController.cpp
    )
    target_include_directories(gameplay_practice_input_boundary_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_practice_input_boundary_tests PRIVATE
        cxx_std_23
    )
    target_compile_definitions(gameplay_practice_input_boundary_tests PRIVATE
        BX_CONFIG_DEBUG=1
    )
    target_link_libraries(gameplay_practice_input_boundary_tests PRIVATE
        $<IF:$<TARGET_EXISTS:SDL2_ttf::SDL2_ttf>,SDL2_ttf::SDL2_ttf,SDL2_ttf::SDL2_ttf-static>
    )

    add_executable(gameplay_simulation_tests
        tests/gameplay_simulation_tests.cpp
        src/bms_parser.cpp
        src/scene/play/CompiledGameplayJudge.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayNoteJudgeRole.cpp
        src/scene/play/GameplayCandidateRules.cpp
        src/scene/play/GameplayDefinition.cpp
        src/scene/play/GameplaySimulation.cpp
        src/scene/play/Judge.cpp
        src/scene/play/RhythmLaneInputController.cpp
    )
    target_include_directories(gameplay_simulation_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_simulation_tests PRIVATE cxx_std_23)
    target_compile_definitions(gameplay_simulation_tests PRIVATE
        BX_CONFIG_DEBUG=1
    )
    target_link_libraries(gameplay_simulation_tests PRIVATE
        $<IF:$<TARGET_EXISTS:SDL2_ttf::SDL2_ttf>,SDL2_ttf::SDL2_ttf,SDL2_ttf::SDL2_ttf-static>
    )

    add_executable(gameplay_automatic_authority_tests
        tests/gameplay_automatic_authority_tests.cpp
        src/bms_parser.cpp
        src/scene/play/CompiledGameplayJudge.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayNoteJudgeRole.cpp
        src/scene/play/GameplayCandidateRules.cpp
        src/scene/play/GameplayDefinition.cpp
        src/scene/play/GameplaySimulation.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(gameplay_automatic_authority_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_automatic_authority_tests PRIVATE
        cxx_std_23
    )
    target_compile_definitions(gameplay_automatic_authority_tests PRIVATE
        BX_CONFIG_DEBUG=1
    )

    add_executable(realtime_gameplay_worker_tests
        tests/realtime_gameplay_worker_tests.cpp
        src/bms_parser.cpp
        src/scene/play/CompiledGameplayJudge.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayNoteJudgeRole.cpp
        src/scene/play/GameplayCandidateRules.cpp
        src/scene/play/GameplayDefinition.cpp
        src/scene/play/GameplaySimulation.cpp
        src/scene/play/Judge.cpp
        src/scene/play/RealtimeGameplayWorker.cpp
    )
    target_include_directories(realtime_gameplay_worker_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(realtime_gameplay_worker_tests PRIVATE cxx_std_23)
    target_compile_definitions(realtime_gameplay_worker_tests PRIVATE
        BX_CONFIG_DEBUG=1
    )
    if(WIN32)
        target_link_libraries(realtime_gameplay_worker_tests PRIVATE avrt)
    endif()

    add_executable(realtime_gameplay_authority_policy_tests
        tests/realtime_gameplay_authority_policy_tests.cpp
        src/scene/play/RealtimeGameplayAuthorityPolicy.cpp
    )
    target_include_directories(realtime_gameplay_authority_policy_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(realtime_gameplay_authority_policy_tests PRIVATE
        cxx_std_23
    )

    add_executable(realtime_touch_input_router_tests
        tests/realtime_touch_input_router_tests.cpp
        src/scene/play/RealtimeTouchInputRouter.cpp
    )
    target_include_directories(realtime_touch_input_router_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(realtime_touch_input_router_tests PRIVATE cxx_std_23)
    target_compile_definitions(realtime_touch_input_router_tests PRIVATE
        BX_CONFIG_DEBUG=1
    )

    add_executable(replay_keysound_schedule_tests
        tests/replay_keysound_schedule_tests.cpp
        src/bms_parser.cpp
        src/scene/play/GameplayDefinition.cpp
        src/scene/play/ReplayKeysoundSchedule.cpp
    )
    target_include_directories(replay_keysound_schedule_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_keysound_schedule_tests PRIVATE cxx_std_23)

    add_executable(practice_analytics_tests
        tests/practice_analytics_tests.cpp
        src/ScoreProvenance.cpp
        src/bms_parser.cpp
        src/practice/PracticeAnalytics.cpp
    )
    target_include_directories(practice_analytics_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(practice_analytics_tests PRIVATE cxx_std_23)

    add_executable(practice_result_model_tests
        tests/practice_result_model_tests.cpp
        src/ScoreProvenance.cpp
        src/bms_parser.cpp
        src/practice/PracticeAnalytics.cpp
        src/practice/PracticeConfiguration.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/practice/PracticeResultModel.cpp
    )
    target_include_directories(practice_result_model_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(practice_result_model_tests PRIVATE cxx_std_23)

    add_executable(practice_result_flow_tests
        tests/practice_result_flow_tests.cpp
        src/ScoreProvenance.cpp
        src/bms_parser.cpp
        src/practice/PracticeAnalytics.cpp
        src/practice/PracticeConfiguration.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/practice/PracticeResultFlow.cpp
        src/practice/PracticeResultModel.cpp
    )
    target_include_directories(practice_result_flow_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(practice_result_flow_tests PRIVATE cxx_std_23)

    add_executable(practice_launch_tests
        tests/practice_launch_tests.cpp
        src/ScoreProvenance.cpp
        src/practice/PracticeConfiguration.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/practice/PracticeLaunchRequest.cpp
    )
    target_include_directories(practice_launch_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(practice_launch_tests PRIVATE cxx_std_23)

    add_executable(practice_analytics_presentation_tests
        tests/practice_analytics_presentation_tests.cpp
        src/scene/PracticeAnalyticsPresentation.cpp
    )
    target_include_directories(practice_analytics_presentation_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(practice_analytics_presentation_tests PRIVATE cxx_std_23)

    add_executable(result_layout_geometry_tests
        tests/result_layout_geometry_tests.cpp
    )
    target_include_directories(result_layout_geometry_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(result_layout_geometry_tests PRIVATE cxx_std_23)

    add_executable(result_gauge_history_tests
        tests/result_gauge_history_tests.cpp
        src/scene/ResultGaugeHistory.cpp
    )
    target_include_directories(result_gauge_history_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(result_gauge_history_tests PRIVATE cxx_std_23)

    add_executable(chart_practice_range_tests
        tests/chart_practice_range_tests.cpp
        src/practice/PracticeConfiguration.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(chart_practice_range_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(chart_practice_range_tests PRIVATE cxx_std_23)

    add_executable(practice_preset_store_tests
        tests/practice_preset_store_tests.cpp
        src/AtomicFile.cpp
        src/VersionedJson.cpp
        src/practice/PracticeConfiguration.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/practice/PracticePresetStore.cpp
    )
    target_include_directories(practice_preset_store_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(practice_preset_store_tests PRIVATE cxx_std_23)
    if(WIN32)
        target_link_libraries(app_settings_store_tests PRIVATE shell32 ole32)
    endif()

    add_executable(player_profile_manager_tests
        tests/player_profile_manager_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/PlayerProfileManager.cpp
        src/Uuid.cpp
        src/ProfileDatabaseTools.cpp
        src/AppSettingsStore.cpp
        src/AtomicFile.cpp
        src/VersionedJson.cpp
        src/practice/PracticeConfiguration.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/practice/PracticePresetStore.cpp
        src/AppSettings.cpp
        src/ir/IrProfileSettings.cpp
        src/settings/AudioVideoSettings.cpp
        src/input/GyroscopeTurntable.cpp
        src/input/InputProfile.cpp
        src/input/InputDefaults.cpp
        src/input/InputProfileStore.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/ir/IrOutboxModels.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(player_profile_manager_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(player_profile_manager_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(player_profile_manager_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(player_profile_manager_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(player_profile_manager_tests PRIVATE SDL2)
    endif()
    if(WIN32)
        target_link_libraries(player_profile_manager_tests PRIVATE shell32 ole32)
    endif()

    add_executable(profile_switch_tests
        tests/profile_switch_tests.cpp
        src/DifficultyTableImporter.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ProfileSessionCoordinator.cpp
        src/PlayerProfileManager.cpp
        src/Uuid.cpp
        src/ProfileDatabaseTools.cpp
        src/AppSettingsStore.cpp
        src/AtomicFile.cpp
        src/VersionedJson.cpp
        src/practice/PracticeConfiguration.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/practice/PracticePresetStore.cpp
        src/AppSettings.cpp
        src/ir/IrProfileSettings.cpp
        src/settings/AudioVideoSettings.cpp
        src/input/GyroscopeTurntable.cpp
        src/input/InputProfile.cpp
        src/input/InputDefaults.cpp
        src/input/InputProfileStore.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/bms_parser.cpp
        src/repositories/ChartRepository.cpp
        src/repositories/ChartScanStore.cpp
        src/repositories/ScoreRepositoryChartSession.cpp
        src/repositories/ChartRepositoryDifficulty.cpp
        src/repositories/ChartRepositoryQueries.cpp
        src/repositories/ChartStorageIdentity.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/ir/IrOutboxModels.cpp
        src/ResultPersistenceCoordinator.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(profile_switch_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(profile_switch_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(profile_switch_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(profile_switch_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(profile_switch_tests PRIVATE SDL2)
    endif()
    target_link_libraries(profile_switch_tests PRIVATE ${COMMON_LIBS})
    if(APPLE)
        target_link_libraries(profile_switch_tests PRIVATE iconv)
    endif()
    if(WIN32)
        target_link_libraries(profile_switch_tests PRIVATE shell32 ole32)
    endif()

    add_executable(profile_archive_tests
        tests/profile_archive_tests.cpp
        src/CourseIdentity.cpp
        src/ProfileArchive.cpp
        src/FileChecksum.cpp
        src/PlayerProfileManager.cpp
        src/Uuid.cpp
        src/ProfileDatabaseTools.cpp
        src/AppSettingsStore.cpp
        src/AtomicFile.cpp
        src/VersionedJson.cpp
        src/practice/PracticeConfiguration.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/practice/PracticePresetStore.cpp
        src/AppSettings.cpp
        src/ir/IrProfileSettings.cpp
        src/settings/AudioVideoSettings.cpp
        src/input/GyroscopeTurntable.cpp
        src/input/InputProfile.cpp
        src/input/InputDefaults.cpp
        src/input/InputProfileStore.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/ir/IrOutboxModels.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(profile_archive_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(profile_archive_tests PRIVATE cxx_std_23)
    target_link_libraries(profile_archive_tests PRIVATE ${COMMON_LIBS})
    if(WIN32)
        target_link_libraries(profile_archive_tests PRIVATE shell32 ole32 advapi32)
    endif()

    add_executable(profile_settings_controller_tests
        tests/profile_settings_controller_tests.cpp
        src/scene/ProfileSettingsController.cpp
        src/scene/ProfileRuntimeReapply.cpp
    )
    target_include_directories(profile_settings_controller_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(profile_settings_controller_tests PRIVATE cxx_std_23)

    add_executable(profile_runtime_reapply_tests
        tests/profile_runtime_reapply_tests.cpp
        src/scene/ProfileRuntimeReapply.cpp
    )
    target_include_directories(profile_runtime_reapply_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(profile_runtime_reapply_tests PRIVATE cxx_std_23)

    add_executable(input_device_registry_tests
        tests/input_device_registry_tests.cpp
        src/input/InputDeviceIdentity.cpp
        src/input/InputDeviceRegistry.cpp
        src/input/SDLInputBackend.cpp
    )
    target_include_directories(input_device_registry_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(input_device_registry_tests PRIVATE cxx_std_23)
    asobmashow_add_midi_backend(input_device_registry_tests)
    asobmashow_add_gyroscope_backend(input_device_registry_tests)
    asobmashow_add_windows_realtime_input_backend(input_device_registry_tests)
    if(TARGET SDL2::SDL2)
        target_link_libraries(input_device_registry_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(input_device_registry_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(input_device_registry_tests PRIVATE SDL2)
    endif()

    add_executable(logical_gameplay_input_tests
        tests/logical_gameplay_input_tests.cpp
        src/input/LogicalGameplayInputAdapter.cpp
        src/input/InputBindingResolver.cpp
        src/input/InputDefaults.cpp
        src/input/InputNormalizer.cpp
        src/input/RealtimePhysicalInputRouter.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(logical_gameplay_input_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(logical_gameplay_input_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(logical_gameplay_input_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(logical_gameplay_input_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(logical_gameplay_input_tests PRIVATE SDL2)
    endif()

    add_executable(display_settings_manager_tests
        tests/display_settings_manager_tests.cpp
        src/video/DisplaySettingsManager.cpp
        src/video/FramePacer.cpp
    )
    target_include_directories(display_settings_manager_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(display_settings_manager_tests PRIVATE cxx_std_23)

    add_executable(frame_pacer_tests
        tests/frame_pacer_tests.cpp
        src/video/FramePacer.cpp
    )
    target_include_directories(frame_pacer_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(frame_pacer_tests PRIVATE cxx_std_23)

    add_executable(sdl_display_backend_tests
        tests/sdl_display_backend_tests.cpp
        src/video/DisplaySettingsManager.cpp
        src/video/FramePacer.cpp
        src/video/RendererAccessCoordinator.cpp
        src/video/SDLDisplayBackend.cpp
    )
    target_include_directories(sdl_display_backend_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(sdl_display_backend_tests PRIVATE cxx_std_23)
    target_link_libraries(sdl_display_backend_tests PRIVATE bgfx)
    if(TARGET SDL2::SDL2)
        target_link_libraries(sdl_display_backend_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(sdl_display_backend_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(sdl_display_backend_tests PRIVATE SDL2)
    endif()

    add_executable(midi_input_tests
        tests/midi_input_tests.cpp
        src/input/LiveMidiDeviceIdAllocator.cpp
        src/input/MidiMessageParser.cpp
        src/input/NativeCallbackLifetime.cpp
        src/input/QueuedMidiInputBackend.cpp
        src/input/Utf16ToUtf8.cpp
    )
    target_include_directories(midi_input_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(midi_input_tests PRIVATE cxx_std_23)

    function(asobmashow_register_test target_name)
        set(test_name ${target_name})
        if(ARGC GREATER 1)
            set(test_name ${ARGV1})
        endif()
        add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${target_name}>)
        set_tests_properties(${test_name} PROPERTIES
            WORKING_DIRECTORY $<TARGET_FILE_DIR:${target_name}>
        )
    endfunction()

    asobmashow_register_test(repository_boundary_tests)

    add_test(NAME foundation_av_settings
        COMMAND $<TARGET_FILE:audio_video_settings_tests>)
    set_tests_properties(foundation_av_settings PROPERTIES
        WORKING_DIRECTORY $<TARGET_FILE_DIR:audio_video_settings_tests>
    )

    add_test(NAME foundation_av_audio_mix
        COMMAND $<TARGET_FILE:audio_mix_tests>)
    set_tests_properties(foundation_av_audio_mix PROPERTIES
        WORKING_DIRECTORY $<TARGET_FILE_DIR:audio_mix_tests>
    )

    add_test(NAME foundation_av_audio_wrapper_lifecycle
        COMMAND $<TARGET_FILE:audio_wrapper_lifecycle_tests>)
    set_tests_properties(foundation_av_audio_wrapper_lifecycle PROPERTIES
        WORKING_DIRECTORY $<TARGET_FILE_DIR:audio_wrapper_lifecycle_tests>
        TIMEOUT 15
    )

    add_test(NAME foundation_av_audio_device
        COMMAND $<TARGET_FILE:audio_device_manager_tests>)
    set_tests_properties(foundation_av_audio_device PROPERTIES
        WORKING_DIRECTORY $<TARGET_FILE_DIR:audio_device_manager_tests>
    )

    add_test(NAME foundation_av_jukebox_restore
        COMMAND $<TARGET_FILE:jukebox_restore_tests>)
    set_tests_properties(foundation_av_jukebox_restore PROPERTIES
        WORKING_DIRECTORY $<TARGET_FILE_DIR:jukebox_restore_tests>
        TIMEOUT 30
    )

    foreach(test_target IN ITEMS
        prep_metronome_tests
        preparation_plan_tests
        start_lane_indicator_geometry_tests
        lane_cover_number_geometry_tests
        chart_viewer_note_geometry_tests
        gameplay_scroll_geometry_tests
        gameplay_chart_entity_render_budget_tests
        club_beat_tests
        gbattle_tests
        replay_summary_list_tests
        replay_record_filters_tests
        course_identity_tests
        ir_driver_tests
        tachi_batch_manual_tests
        tachi_driver_tests
        bokutachi_cache_store_tests
        tachi_ranking_parser_tests
        ir_submission_service_tests
        ir_ranking_service_tests
        ir_settings_presentation_tests
        ir_result_presentation_tests
        ir_ranking_modal_tests
        replay_repository_tests
        music_playlist_repository_tests
        chart_repository_tests
        chart_library_scanner_tests
        difficulty_table_importer_tests
        result_persistence_model_tests
        result_recall_builder_tests
        result_persistence_coordinator_tests
        result_persistence_integration_tests
        chart_record_filters_tests
        main_menu_library_tests
        chart_meta_index_order_tests
        score_cache_query_tests
        app_database_initializer_tests
        application_startup_tests
        application_result_recovery_tests
        chart_filter_sort_panel_view_tests
        dropdown_view_tests
        view_layout_tests
        button_enabled_tests
    )
        asobmashow_register_test(${test_target})
    endforeach()
    if (TARGET ir_http_client_tests)
        asobmashow_register_test(ir_http_client_tests)
    endif()
    set_tests_properties(application_result_recovery_tests PROPERTIES
        TIMEOUT 10
    )
    set_tests_properties(result_persistence_integration_tests PROPERTIES
        TIMEOUT 15
    )
    add_test(NAME foundation_provenance_contract
        COMMAND $<TARGET_FILE:score_provenance_tests>)
    set_tests_properties(foundation_provenance_contract PROPERTIES
        WORKING_DIRECTORY $<TARGET_FILE_DIR:score_provenance_tests>
    )
    asobmashow_register_test(gameplay_ruleset_tests)
    asobmashow_register_test(play_options_panel_view_tests)
    asobmashow_register_test(gameplay_judge_rules_tests)
    asobmashow_register_test(gameplay_gauge_rules_tests)
    asobmashow_register_test(gameplay_ruleset_policy_tests)
    asobmashow_register_test(input_profile_tests foundation_input_profile)
    if (NOT IOS AND NOT PLATFORM_ANDROID)
        asobmashow_register_test(platform_document_handoff_tests foundation_platform_document_handoff)
        asobmashow_register_test(profile_export_staging_tests foundation_profile_export_staging)
    endif()
    asobmashow_register_test(input_binding_resolver_tests foundation_input_resolver)
    asobmashow_register_test(gyroscope_turntable_tests foundation_input_gyroscope)
    asobmashow_register_test(ios_gyroscope_motion_adapter_tests foundation_input_gyroscope_ios_adapter)
    asobmashow_register_test(gyroscope_input_backend_core_tests foundation_input_gyroscope_backend)
    asobmashow_register_test(input_capture_controller_tests foundation_input_capture)
    set_tests_properties(foundation_input_capture PROPERTIES TIMEOUT 15)
    asobmashow_register_test(app_settings_store_tests foundation_profile_settings)
    asobmashow_register_test(ir_credential_store_tests ir_credential_store_tests)
    asobmashow_register_test(practice_configuration_tests)
    asobmashow_register_test(practice_rule_override_tests)
    asobmashow_register_test(gameplay_score_state_tests)
    asobmashow_register_test(practice_session_tests)
    asobmashow_register_test(gameplay_practice_boundary_tests)
    asobmashow_register_test(gameplay_playback_startup_tests)
    asobmashow_register_test(gameplay_practice_input_boundary_tests)
    asobmashow_register_test(gameplay_simulation_tests)
    asobmashow_register_test(gameplay_automatic_authority_tests)
    asobmashow_register_test(realtime_gameplay_worker_tests)
    asobmashow_register_test(realtime_gameplay_authority_policy_tests)
    asobmashow_register_test(realtime_touch_input_router_tests)
    asobmashow_register_test(replay_keysound_schedule_tests)
    asobmashow_register_test(practice_analytics_tests)
    asobmashow_register_test(practice_result_model_tests)
    asobmashow_register_test(practice_result_flow_tests)
    asobmashow_register_test(practice_launch_tests)
    asobmashow_register_test(practice_analytics_presentation_tests)
    asobmashow_register_test(result_layout_geometry_tests)
    asobmashow_register_test(result_gauge_history_tests)
    asobmashow_register_test(chart_practice_range_tests)
    asobmashow_register_test(practice_preset_store_tests)
    asobmashow_register_test(player_profile_manager_tests foundation_profile_manager)
    asobmashow_register_test(profile_switch_tests foundation_profile_switch)
    set_tests_properties(foundation_profile_switch PROPERTIES TIMEOUT 30)
    asobmashow_register_test(profile_archive_tests foundation_profile_archive)
    asobmashow_register_test(profile_settings_controller_tests foundation_profile_controller)
    asobmashow_register_test(profile_runtime_reapply_tests foundation_profile_runtime)
    asobmashow_register_test(input_device_registry_tests foundation_input_registry)
    asobmashow_register_test(logical_gameplay_input_tests foundation_input_gameplay)
    asobmashow_register_test(input_timestamp_tests foundation_input_timestamp)
    asobmashow_register_test(windows_realtime_input_mapping_tests foundation_input_windows_realtime_mapping)
    asobmashow_register_test(settings_audio_video_model_tests foundation_av_settings_ui)
    asobmashow_register_test(display_settings_manager_tests foundation_av_display)
    asobmashow_register_test(frame_pacer_tests foundation_av_frame_pacer)
    asobmashow_register_test(sdl_display_backend_tests foundation_av_sdl_display)
    add_test(NAME foundation_provenance_db
        COMMAND $<TARGET_FILE:score_provenance_db_tests>)
    set_tests_properties(foundation_provenance_db PROPERTIES
        WORKING_DIRECTORY $<TARGET_FILE_DIR:score_provenance_db_tests>
    )
    asobmashow_register_test(midi_input_tests foundation_input_midi)
endif()
