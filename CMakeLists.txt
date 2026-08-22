cmake_minimum_required(VERSION 3.22)

include(ExternalProject)
project(AsoBMaShow VERSION 0.0.1 LANGUAGES C CXX)
set(ASOBMASHOW_APPLICATION_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Application version exposed to native runtime and gameplay skins")
add_compile_definitions(
    ASOBMASHOW_APPLICATION_VERSION="${ASOBMASHOW_APPLICATION_VERSION}")
include(CTest)
option(ASOBMASHOW_BUILD_TESTS "Build lightweight unit tests" ON)
option(ASOBMASHOW_ENABLE_PERF_TELEMETRY
       "Log periodic frame, audio, and input performance telemetry" OFF)
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
if (ANDROID)
    set(ASOBMASHOW_LUA_GAMEPLAY_SKINS_DEFAULT OFF)
else()
    set(ASOBMASHOW_LUA_GAMEPLAY_SKINS_DEFAULT ON)
endif()
option(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
       "Enable Beatoraja Lua gameplay skins"
       ${ASOBMASHOW_LUA_GAMEPLAY_SKINS_DEFAULT})
# Option to build macOS app bundle
if (PLATFORM_MACOS)
    option(BUILD_MACOS_BUNDLE "Build macOS app bundle" OFF)
    message(STATUS "Build macOS bundle: ${BUILD_MACOS_BUNDLE}")

    set(MACOS_BUNDLE_IDENTIFIER "com.SNURhythm.AsoBMaShow")
    set(MACOS_BUNDLE_NAME "AsoBMaShow")
    set(MACOS_BUNDLE_ICON_FILE "AsoBMaShow.icns")
    set(MACOS_BUNDLE_ICON_SOURCE
        "${CMAKE_SOURCE_DIR}/assets/platform/${MACOS_BUNDLE_ICON_FILE}")
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
    # Let cl.exe compile the sources from each large target concurrently. /FS
    # keeps those compiler workers' shared PDB writes serialized and avoids
    # C1041 when /MP is active.
    add_compile_options(/Zc:__cplusplus /Zc:preprocessor /wd4819 /utf-8 /bigobj /MP /FS)
    add_definitions(-DSTBI_WINDOWS_UTF8)
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_definitions(-DAMD64)
    else ()
        add_definitions(-DIA32)
    endif ()
else ()
    add_compile_options(-Wno-strict-prototypes -Wno-unused-but-set-variable -pedantic -Wno-ignored-attributes -Wstrict-aliasing -Wstrict-overflow)
endif ()


# ------------------------------------------------------------------------------
# Dependency Management
# ------------------------------------------------------------------------------

# 1. SndFile (Common)
find_package(SndFile CONFIG REQUIRED)
set(COMMON_LIBS SndFile::sndfile)

if (NOT IOS)
    find_package(utf8proc CONFIG REQUIRED)
    list(APPEND COMMON_LIBS utf8proc::utf8proc)
endif()

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
    find_library(SECURITY Security)
    list(APPEND COMMON_LIBS ${UIKIT} ${FOUNDATION} ${CORE_GRAPHICS}
        ${CORE_TEXT} ${PHOTOS} ${SECURITY} ${SDL2_LIB} ${SDL2_TFF_LIB})

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
            include_directories(BEFORE ${LUAJIT_INCLUDE_DIR})
            list(APPEND COMMON_LIBS ${LUAJIT_LIB})
            message(STATUS "Found LuaJIT: ${LUAJIT_LIB}")
        else()
            message(FATAL_ERROR "LuaJIT not found. LUAJIT_LIB=${LUAJIT_LIB}, LUAJIT_INCLUDE_DIR=${LUAJIT_INCLUDE_DIR}")
        endif()
    else()
        # On Unix-like systems, use pkg-config
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LUAJIT REQUIRED luajit)
        include_directories(BEFORE ${LUAJIT_INCLUDE_DIRS})
        link_directories(${LUAJIT_LIBRARY_DIRS})
        list(APPEND COMMON_LIBS ${LUAJIT_LIBRARIES})
        # unarr gives direct offset-based reads for non-solid RAR4 archives.
        find_package(unarr CONFIG REQUIRED)
        list(APPEND COMMON_LIBS unarr::unarr)
    endif()

endif()

# ------------------------------------------------------------------------------
# Targets
# ------------------------------------------------------------------------------

# Prefer the selected toolchain/vcpkg headers over stale host-wide installs.
# This is portable CMake behavior and avoids architecture-mismatched SDL
# headers on Apple Silicon without injecting compiler-specific -I flags.
set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)

# Lua skin file_list mirrors java.util.regex.Pattern.  Keep the desktop and
# Android regex engine pinned in the repository instead of relying on a host
# package; iOS uses Foundation's system ICU implementation through the same
# wrapper API.
if(NOT IOS)
    set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "" FORCE)
    set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "" FORCE)
    set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
    set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
    set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PCRE2_SUPPORT_JIT OFF CACHE BOOL "" FORCE)
    set(PCRE2_SHOW_REPORT OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/pcre2 EXCLUDE_FROM_ALL)
endif()

function(asobmashow_enable_lua_skin_java_pattern target_name)
    if(IOS)
        target_sources(${target_name} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/skin/beatoraja/LuaSkinJavaPatternFoundation.mm)
        target_link_libraries(${target_name} PRIVATE "-framework Foundation")
    else()
        target_sources(${target_name} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/skin/beatoraja/LuaSkinJavaPatternPcre2.cpp)
        target_link_libraries(${target_name} PRIVATE pcre2-8-static)
    endif()
endfunction()

add_library(TinyFileDialogs STATIC ${SRC_DIR}/tinyfiledialogs.c)
add_library(SQLite3 STATIC ${SRC_DIR}/sqlite3.c)

if (ANDROID)
    add_library(main SHARED)
elseif (PLATFORM_MACOS AND BUILD_MACOS_BUNDLE)
    add_executable(main MACOSX_BUNDLE)
    target_sources(main PRIVATE "${MACOS_BUNDLE_ICON_SOURCE}")
    set_source_files_properties("${MACOS_BUNDLE_ICON_SOURCE}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    set_target_properties(main PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${MACOS_INFO_PLIST}
        MACOSX_BUNDLE_GUI_IDENTIFIER "${MACOS_BUNDLE_IDENTIFIER}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_NAME "${MACOS_BUNDLE_NAME}"
        MACOSX_BUNDLE_ICON_FILE "${MACOS_BUNDLE_ICON_FILE}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright © SNURhythm"
        OUTPUT_NAME "${MACOS_BUNDLE_NAME}"
    )
else()
    add_executable(main)
endif()
if (WIN32)
    set_target_properties(main PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Refresh the source identity before every build. The refresh script only
# replaces its generated header when Git identity changes, preserving normal
# incremental compilation while keeping acceptance evidence honest.
include("${CMAKE_SOURCE_DIR}/cmake/BuildIdentity.cmake")
add_library(asobmashow_build_identity OBJECT "${SRC_DIR}/BuildIdentity.cpp")
target_compile_features(asobmashow_build_identity PRIVATE cxx_std_23)
asobmashow_configure_build_identity_refresh(asobmashow_build_identity
    "${CMAKE_SOURCE_DIR}")
target_sources(main PRIVATE $<TARGET_OBJECTS:asobmashow_build_identity>)
if (ANDROID)
    # Android NDK libc++ keeps C++20 stop_token/jthread behind this opt-in.
    # The application uses both for cancellable background work.
    target_compile_definitions(main PRIVATE
        PLATFORM_ANDROID=1
        _LIBCPP_ENABLE_EXPERIMENTAL
    )
endif()
target_compile_definitions(main PRIVATE
    ASOBMASHOW_ENABLE_PERF_TELEMETRY=$<BOOL:${ASOBMASHOW_ENABLE_PERF_TELEMETRY}>
    ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=$<BOOL:${ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS}>
)

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
    find_package(ZLIB REQUIRED)
    # The release bundle triplet is fully static, so a checkout-local runtime
    # search path is unnecessary and would make the package machine-specific.
    # Keep normal build rpaths for developer builds that use dynamic vcpkg
    # triplets.
    if (BUILD_MACOS_BUNDLE
        AND CMAKE_BUILD_TYPE STREQUAL "Release"
        AND VCPKG_TARGET_TRIPLET STREQUAL "arm64-osx-asobmashow")
        set_target_properties(main PROPERTIES SKIP_BUILD_RPATH TRUE)
    endif()
    include(CheckLinkerFlag)
    check_linker_flag(CXX "LINKER:-no_warn_duplicate_libraries"
        ASOBMASHOW_HAS_NO_WARN_DUPLICATE_LIBRARIES)
    if (ASOBMASHOW_HAS_NO_WARN_DUPLICATE_LIBRARIES)
        target_link_options(main PRIVATE "LINKER:-no_warn_duplicate_libraries")
    endif()
    # A static dependency built for a newer macOS can silently raise the
    # bundle's real deployment floor while leaving LC_BUILD_VERSION unchanged.
    # Make that (and every other linker warning) fatal for release artifacts.
    target_link_options(main PRIVATE
        $<$<CONFIG:Release>:LINKER:-fatal_warnings>
    )
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

function(asobmashow_stage_sevenzip_runtime target_name)
    if(WIN32)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:7zip::7zip>"
                "$<TARGET_FILE_DIR:${target_name}>"
            COMMENT "Staging the 7-Zip runtime for ${target_name}"
            VERBATIM
        )
    endif()
endfunction()
asobmashow_stage_sevenzip_runtime(main)

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
        bz2 ZLIB::ZLIB iconv
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

if (WIN32 AND ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS)
    set(WINDOWS_GAMEPLAY_SKIN_SHADERS
        shaders/dx11/vs_skin_quad.bin
        shaders/dx11/fs_skin_quad.bin
        shaders/dx11/fs_skin_distance_field.bin
        shaders/dx11/vs_skin_yuvrgb.bin
        shaders/dx11/fs_skin_yuvrgb.bin
    )
    foreach(shader IN LISTS WINDOWS_GAMEPLAY_SKIN_SHADERS)
        if (NOT EXISTS "${CMAKE_SOURCE_DIR}/${shader}")
            message(FATAL_ERROR
                "Missing required DX11 gameplay skin shader: ${shader}. "
                "Run shader_src/make.py on Windows before building.")
        endif()
    endforeach()
endif()

if (ANDROID)
    # Android reads packaged shader assets with SDL_RWFromFile.
elseif (PLATFORM_MACOS)
     # macOS (Metal)
    file(GLOB_RECURSE METAL_SHADER_FILES "${CMAKE_SOURCE_DIR}/shaders/metal/*.*")
    if (BUILD_MACOS_BUNDLE)
        add_custom_target(copy_macos_bundle_shaders
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_DIR:main>/Contents/Resources/shaders
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/shaders/metal $<TARGET_BUNDLE_DIR:main>/Contents/Resources/shaders/metal
            DEPENDS ${METAL_SHADER_FILES}
        )
        add_dependencies(main copy_macos_bundle_shaders)
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
    find_package(BZip2 REQUIRED)
    find_package(LibLZMA REQUIRED)
    find_package(ZLIB REQUIRED)

    if(APPLE AND NOT IOS)
        add_executable(builtin_renderer_characterization_tests
            tests/builtin_renderer_characterization_tests.cpp
            src/ArchiveFile.cpp
            src/MinizBridge.c
            src/audio/GameplayBgaMissStateTracker.cpp
            src/bms_parser.cpp
            src/path.cpp
            src/rendering/Camera.cpp
            src/rendering/Color.cpp
            src/rendering/common.cpp
            src/rendering/RenderPlan.cpp
            src/rendering/SimpleBatchRenderer.cpp
            src/rendering/TexBatchRenderer.cpp
            src/rendering/UniformCache.cpp
            src/scene/play/BMSRenderer.cpp
            src/scene/play/BeatorajaHiSpeedChart.cpp
            src/scene/play/BuiltInPlayfieldPresentation.cpp
            src/scene/play/GameplayGaugeRules.cpp
            src/scene/play/Judge.cpp
            src/scene/play/JudgementIndicatorRenderer.cpp
            src/scene/play/PlayfieldChartVisualModel.cpp
            src/scene/play/PlayfieldProjection.cpp
            src/scene/play/PlayfieldVisualState.cpp
            src/scene/play/SkinGameplayGraphState.cpp
            src/utils/SpriteLoader.cpp
            src/view/DecodedImageCache.cpp
            src/view/ImageFileDecoder.cpp
            src/view/SdlTtfRuntime.cpp
            src/view/TextView.cpp
            src/view/View.cpp
            bgfx/bimg/3rdparty/lodepng/lodepng.cpp
        )
        target_include_directories(builtin_renderer_characterization_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
            ${CMAKE_SOURCE_DIR}/bgfx/bimg/3rdparty/lodepng
        )
        target_compile_features(builtin_renderer_characterization_tests PRIVATE cxx_std_23)
        target_compile_definitions(builtin_renderer_characterization_tests PRIVATE
            ASOBMASHOW_BMS_RENDERER_CHARACTERIZATION=1
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(builtin_renderer_characterization_tests PRIVATE
            ${COMMON_LIBS}
            bgfx
            yogacore
            iconv
        )
    endif()

    add_executable(skin_path_policy_tests
        tests/skin_path_policy_tests.cpp
        src/skin/package/SkinPathPolicy.cpp
    )
    target_include_directories(skin_path_policy_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(skin_path_policy_tests PRIVATE cxx_std_23)
    target_link_libraries(skin_path_policy_tests PRIVATE utf8proc::utf8proc)

    add_executable(gameplay_skin_source_format_tests
        tests/gameplay_skin_source_format_tests.cpp
        src/skin/beatoraja/GameplaySkinSourceFormat.cpp
    )
    target_include_directories(gameplay_skin_source_format_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_skin_source_format_tests PRIVATE cxx_std_23)

    if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS)
        add_executable(play_skin_viewport_tests
            tests/play_skin_viewport_tests.cpp
            src/skin/beatoraja/PlaySkinViewport.cpp
            src/skin/beatoraja/SkinDestinationEvaluator.cpp
        )
        add_executable(skin_destination_evaluator_tests
            tests/skin_destination_evaluator_tests.cpp
            src/skin/beatoraja/PlaySkinViewport.cpp
            src/skin/beatoraja/SkinDestinationEvaluator.cpp
        )
        foreach(target IN ITEMS play_skin_viewport_tests skin_destination_evaluator_tests)
            target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
            set_target_properties(${target} PROPERTIES
                CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
        endforeach()

        add_executable(skin_draw_command_tests
            tests/skin_draw_command_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/PlaySkinViewport.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinDestinationEvaluator.cpp
            src/skin/beatoraja/Skin2DRenderer.cpp
            src/skin/beatoraja/SkinBpmGraphRenderer.cpp
            src/skin/beatoraja/SkinGaugeGraphRenderer.cpp
            src/skin/beatoraja/SkinNoteDistributionGraphRenderer.cpp
            src/skin/beatoraja/SkinHitErrorVisualizerRenderer.cpp
            src/skin/beatoraja/SkinTimingVisualizerRenderer.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(skin_draw_command_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(skin_draw_command_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(skin_draw_command_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_SKIN_RENDERER_TESTING=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(skin_draw_command_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(skin_draw_command_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(skin_draw_command_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()

        add_executable(play_skin_touch_geometry_tests
            tests/play_skin_touch_geometry_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/PlaySkinViewport.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinDestinationEvaluator.cpp
            src/skin/beatoraja/Skin2DRenderer.cpp
            src/skin/beatoraja/SkinBpmGraphRenderer.cpp
            src/skin/beatoraja/SkinGaugeGraphRenderer.cpp
            src/skin/beatoraja/SkinNoteDistributionGraphRenderer.cpp
            src/skin/beatoraja/SkinHitErrorVisualizerRenderer.cpp
            src/skin/beatoraja/SkinTimingVisualizerRenderer.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(play_skin_touch_geometry_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(play_skin_touch_geometry_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(play_skin_touch_geometry_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_SKIN_RENDERER_TESTING=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(play_skin_touch_geometry_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(play_skin_touch_geometry_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(play_skin_touch_geometry_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()

        if(NOT IOS)
            add_executable(skin_quad_batch_renderer_tests
                tests/skin_quad_batch_renderer_tests.cpp
                src/rendering/SkinQuadBatchRenderer.cpp
                src/rendering/UniformCache.cpp
                src/skin/beatoraja/SkinMovieCatalogFrame.cpp
            )
            target_include_directories(skin_quad_batch_renderer_tests PRIVATE
                ${CMAKE_SOURCE_DIR}/src
            )
            target_compile_features(skin_quad_batch_renderer_tests PRIVATE cxx_std_23)
            target_compile_definitions(skin_quad_batch_renderer_tests PRIVATE
                ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            )
            target_link_libraries(skin_quad_batch_renderer_tests PRIVATE
                ${COMMON_LIBS}
                bgfx
            )

            # The committed pixels are produced by an actual headless Metal
            # device. Other desktop backends keep the backend-state suite but
            # do not silently replace this acceptance gate with Noop/CPU output.
            if(APPLE)
                add_executable(skin_renderer_golden_tests
                    tests/skin_renderer_golden_tests.cpp
                    src/rendering/SkinQuadBatchRenderer.cpp
                    src/rendering/UniformCache.cpp
                    src/skin/beatoraja/PlaySkinViewport.cpp
                    src/skin/beatoraja/SkinBpmGraphRenderer.cpp
                    src/skin/beatoraja/SkinDestinationEvaluator.cpp
                    src/skin/beatoraja/SkinGaugeGraphRenderer.cpp
                    src/skin/beatoraja/SkinHitErrorVisualizerRenderer.cpp
                    src/skin/beatoraja/SkinNoteDistributionGraphRenderer.cpp
                    src/skin/beatoraja/SkinTimingVisualizerRenderer.cpp
                    bgfx/bimg/3rdparty/lodepng/lodepng.cpp
                )
                target_include_directories(skin_renderer_golden_tests PRIVATE
                    ${CMAKE_SOURCE_DIR}/src
                    ${CMAKE_SOURCE_DIR}/bgfx/bimg/3rdparty/lodepng
                )
                target_compile_features(skin_renderer_golden_tests PRIVATE cxx_std_23)
                target_compile_definitions(skin_renderer_golden_tests PRIVATE
                    ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
                    ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                )
                target_link_libraries(skin_renderer_golden_tests PRIVATE
                    ${COMMON_LIBS}
                    bgfx
                )
            endif()
        endif()
        if(APPLE)
            target_sources(skin_draw_command_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_sources(play_skin_touch_geometry_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(skin_draw_command_tests PRIVATE
                "-framework Foundation"
            )
            target_link_libraries(play_skin_touch_geometry_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(skin_gauge_node_expansion_tests
            tests/skin_gauge_node_expansion_tests.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
        )
        target_include_directories(skin_gauge_node_expansion_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(skin_gauge_node_expansion_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )

        add_executable(skin_judge_number_normalization_tests
            tests/skin_judge_number_normalization_tests.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
        )
        target_include_directories(skin_judge_number_normalization_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(skin_judge_number_normalization_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )

        add_executable(skin_judge_normalization_tests
            tests/skin_judge_normalization_tests.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
        )
        target_include_directories(skin_judge_normalization_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(skin_judge_normalization_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(skin_judge_normalization_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
        )

        add_executable(skin_note_normalization_tests
            tests/skin_note_normalization_tests.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
        )
        target_include_directories(skin_note_normalization_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(skin_note_normalization_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )

        add_executable(skin_note_lane_geometry_normalization_tests
            tests/skin_note_lane_geometry_normalization_tests.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
        )
        target_include_directories(skin_note_lane_geometry_normalization_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(skin_note_lane_geometry_normalization_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )

        add_executable(skin_note_line_normalization_tests
            tests/skin_note_line_normalization_tests.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
        )
        target_include_directories(skin_note_line_normalization_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(skin_note_line_normalization_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )

        add_executable(skin_text_graph_normalization_tests
            tests/skin_text_graph_normalization_tests.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
        )
        target_include_directories(skin_text_graph_normalization_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(skin_text_graph_normalization_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )

        add_executable(skin_cover_normalization_tests
            tests/skin_cover_normalization_tests.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
        )
        target_include_directories(skin_cover_normalization_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(skin_cover_normalization_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )

        add_executable(skin_object_resolution_precedence_tests
            tests/skin_object_resolution_precedence_tests.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
        )
        target_include_directories(skin_object_resolution_precedence_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(skin_object_resolution_precedence_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )

        add_executable(numeric_glyph_atlas_tests
            tests/numeric_glyph_atlas_tests.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
        )
        target_include_directories(numeric_glyph_atlas_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(numeric_glyph_atlas_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )

        add_executable(numeric_glyph_model_validator_tests
            tests/numeric_glyph_model_validator_tests.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
        )
        target_include_directories(numeric_glyph_model_validator_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(numeric_glyph_model_validator_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(numeric_glyph_model_validator_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
        )
    endif()

    if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS AND NOT IOS)
        add_executable(lua_skin_runtime_tests
            tests/lua_skin_runtime_tests.cpp
            src/FileChecksum.cpp
            src/Utils.cpp
            src/path.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(lua_skin_runtime_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(lua_skin_runtime_tests PRIVATE cxx_std_23)
        target_compile_definitions(lua_skin_runtime_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(lua_skin_runtime_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(lua_skin_runtime_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(lua_skin_runtime_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(lua_skin_runtime_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(lua_skin_runtime_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(lua_skin_binding_decoder_tests
            tests/lua_skin_binding_decoder_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(lua_skin_binding_decoder_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(lua_skin_binding_decoder_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(lua_skin_binding_decoder_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(lua_skin_binding_decoder_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(lua_skin_binding_decoder_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(lua_skin_binding_decoder_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(lua_skin_binding_decoder_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(lua_skin_binding_decoder_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(lua_skin_table_decoder_tests
            tests/lua_skin_table_decoder_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/LuaSkinTableDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(lua_skin_table_decoder_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(lua_skin_table_decoder_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(lua_skin_table_decoder_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(lua_skin_table_decoder_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(lua_skin_table_decoder_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(lua_skin_table_decoder_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(lua_skin_table_decoder_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(lua_skin_table_decoder_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(lua_skin_gauge_live_integration_tests
            tests/lua_skin_gauge_live_integration_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/LuaSkinTableDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(lua_skin_gauge_live_integration_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(lua_skin_gauge_live_integration_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(lua_skin_gauge_live_integration_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(lua_skin_gauge_live_integration_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(lua_skin_gauge_live_integration_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(lua_skin_gauge_live_integration_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(lua_skin_gauge_live_integration_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(lua_skin_gauge_live_integration_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(lua_skin_text_graph_live_integration_tests
            tests/lua_skin_text_graph_live_integration_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/LuaSkinTableDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(lua_skin_text_graph_live_integration_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(lua_skin_text_graph_live_integration_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(lua_skin_text_graph_live_integration_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(lua_skin_text_graph_live_integration_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(lua_skin_text_graph_live_integration_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(lua_skin_text_graph_live_integration_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(lua_skin_text_graph_live_integration_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(lua_skin_text_graph_live_integration_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(lua_skin_binding_live_integration_tests
            tests/lua_skin_binding_live_integration_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/LuaSkinTableDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(lua_skin_binding_live_integration_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(lua_skin_binding_live_integration_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(lua_skin_binding_live_integration_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(lua_skin_binding_live_integration_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(lua_skin_binding_live_integration_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(lua_skin_binding_live_integration_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(lua_skin_binding_live_integration_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(lua_skin_binding_live_integration_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(lua_skin_note_live_integration_tests
            tests/lua_skin_note_live_integration_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/LuaSkinTableDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(lua_skin_note_live_integration_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(lua_skin_note_live_integration_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(lua_skin_note_live_integration_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(lua_skin_note_live_integration_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(lua_skin_note_live_integration_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(lua_skin_note_live_integration_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(lua_skin_note_live_integration_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(lua_skin_note_live_integration_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(lua_skin_host_modules_tests
            tests/lua_skin_host_modules_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(lua_skin_host_modules_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(lua_skin_host_modules_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(lua_skin_host_modules_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(lua_skin_host_modules_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(lua_skin_host_modules_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(lua_skin_host_modules_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()

        add_executable(lua_skin_http_transport_tests
            tests/lua_skin_http_transport_tests.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinCurlHttpTransport.cpp
        )
        target_include_directories(lua_skin_http_transport_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(lua_skin_http_transport_tests PRIVATE cxx_std_23)
        target_link_libraries(lua_skin_http_transport_tests PRIVATE
            CURL::libcurl
        )
        if(WIN32)
            target_link_libraries(lua_skin_http_transport_tests PRIVATE ws2_32)
        endif()
        if(APPLE)
            target_sources(lua_skin_host_modules_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(lua_skin_host_modules_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        set(beatoraja_skin_model_test_sources
            tests/beatoraja_skin_model_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/GameplaySkinBuiltinCatalog.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/LuaSkinTableDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        add_executable(beatoraja_skin_model_tests
            ${beatoraja_skin_model_test_sources}
        )
        target_include_directories(beatoraja_skin_model_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(beatoraja_skin_model_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(beatoraja_skin_model_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_LUA_RUNTIME_TEST_HOOKS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(beatoraja_skin_model_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(beatoraja_skin_model_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(beatoraja_skin_model_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(beatoraja_skin_model_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(beatoraja_skin_model_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(json_gameplay_skin_decoder_tests
            tests/json_gameplay_skin_decoder_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/beatoraja/GameplaySkinBuiltinCatalog.cpp
            src/skin/beatoraja/JsonGameplaySkinDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
        )
        target_include_directories(json_gameplay_skin_decoder_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(json_gameplay_skin_decoder_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(json_gameplay_skin_decoder_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(json_gameplay_skin_decoder_tests PRIVATE
            utf8proc::utf8proc
        )

        add_executable(lr2_skin_csv_parser_tests
            tests/lr2_skin_csv_parser_tests.cpp
            src/FileChecksum.cpp
            src/Utils.cpp
            src/path.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/Lr2SkinCsvParser.cpp
            src/skin/beatoraja/Lr2SkinHeaderDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(lr2_skin_csv_parser_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(lr2_skin_csv_parser_tests PRIVATE cxx_std_23)
        target_compile_definitions(lr2_skin_csv_parser_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(lr2_skin_csv_parser_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(lr2_skin_csv_parser_tests PRIVATE advapi32)
        endif()
        if(APPLE)
            target_sources(lr2_skin_csv_parser_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(lr2_skin_csv_parser_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(lr2_gameplay_skin_decoder_tests
            tests/lr2_gameplay_skin_decoder_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/beatoraja/GameplaySkinBuiltinCatalog.cpp
            src/skin/beatoraja/Lr2GameplaySkinDecoder.cpp
            src/skin/beatoraja/Lr2SkinHeaderDecoder.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
        )
        target_include_directories(lr2_gameplay_skin_decoder_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(lr2_gameplay_skin_decoder_tests PRIVATE
            cxx_std_23
        )
        target_compile_definitions(lr2_gameplay_skin_decoder_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(lr2_gameplay_skin_decoder_tests PRIVATE
            utf8proc::utf8proc
        )

        add_executable(beatoraja_gameplay_cross_format_tests
            tests/beatoraja_gameplay_cross_format_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/GameplaySkinBuiltinCatalog.cpp
            src/skin/beatoraja/JsonGameplaySkinDecoder.cpp
            src/skin/beatoraja/Lr2GameplaySkinDecoder.cpp
            src/skin/beatoraja/Lr2SkinHeaderDecoder.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/LuaSkinTableDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/PlaySkinViewport.cpp
            src/skin/beatoraja/Skin2DRenderer.cpp
            src/skin/beatoraja/SkinBpmGraphRenderer.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinDestinationEvaluator.cpp
            src/skin/beatoraja/SkinGaugeGraphRenderer.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinHitErrorVisualizerRenderer.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinNoteDistributionGraphRenderer.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
            src/skin/beatoraja/SkinTimingVisualizerRenderer.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(beatoraja_gameplay_cross_format_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        set_target_properties(beatoraja_gameplay_cross_format_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
        )
        target_compile_definitions(beatoraja_gameplay_cross_format_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(beatoraja_gameplay_cross_format_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(beatoraja_gameplay_cross_format_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(beatoraja_gameplay_cross_format_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(beatoraja_gameplay_cross_format_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(beatoraja_gameplay_cross_format_tests PRIVATE
                "-framework Foundation"
            )
        endif()
    endif()

    add_test(NAME beatoraja_skin_reference_contract
        COMMAND ${Python3_EXECUTABLE}
                -m unittest tests/beatoraja_skin_reference_tests.py -v)
    set_tests_properties(beatoraja_skin_reference_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME beatoraja_gameplay_skin_ledger
        COMMAND ${Python3_EXECUTABLE}
                tests/beatoraja_gameplay_skin_ledger_tests.py
                --require-complete)
    set_tests_properties(beatoraja_gameplay_skin_ledger PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME gameplay_skin_loading_benchmark_script_contract
        COMMAND ${Python3_EXECUTABLE}
                -m unittest
                tests/gameplay_skin_loading_benchmark_script_tests.py -v)
    set_tests_properties(gameplay_skin_loading_benchmark_script_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    if (TARGET beatoraja_gameplay_cross_format_tests)
        add_test(NAME beatoraja_gameplay_oracle
            COMMAND ${Python3_EXECUTABLE}
                    tests/beatoraja_gameplay_oracle_tests.py)
        set_tests_properties(beatoraja_gameplay_oracle PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            ENVIRONMENT
                "ASOBMASHOW_GAMEPLAY_ORACLE_EXECUTABLE=$<TARGET_FILE:beatoraja_gameplay_cross_format_tests>"
        )
    endif()
    add_test(NAME build_identity_refresh_contract
        COMMAND ${Python3_EXECUTABLE}
                -m unittest tests/build_identity_refresh_tests.py -v)
    set_tests_properties(build_identity_refresh_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME skin_tree_snapshotter_windows_contract
        COMMAND ${Python3_EXECUTABLE}
                -m unittest
                tests/skin_tree_snapshotter_windows_contract_tests.py -v)
    set_tests_properties(skin_tree_snapshotter_windows_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME skin_archive_importer_windows_contract
        COMMAND ${Python3_EXECUTABLE}
                -m unittest
                tests/skin_archive_importer_windows_contract_tests.py -v)
    set_tests_properties(skin_archive_importer_windows_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME skin_package_store_windows_contract
        COMMAND ${Python3_EXECUTABLE}
                -m unittest
                tests/skin_package_store_windows_contract_tests.py -v)
    set_tests_properties(skin_package_store_windows_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME lua_skin_file_system_windows_contract
        COMMAND ${Python3_EXECUTABLE}
                -m unittest
                tests/lua_skin_file_system_windows_contract_tests.py -v)
    set_tests_properties(lua_skin_file_system_windows_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME settings_virtual_controller_ui_contract
        COMMAND ${Python3_EXECUTABLE}
                -m unittest
                tests/settings_virtual_controller_ui_contract_tests.py -v)
    set_tests_properties(settings_virtual_controller_ui_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME settings_gameplay_skin_initialization_contract
        COMMAND ${Python3_EXECUTABLE}
                -m unittest
                tests/settings_gameplay_skin_initialization_contract_tests.py -v)
    set_tests_properties(settings_gameplay_skin_initialization_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME replay_video_ui_batch_contract
        COMMAND ${Python3_EXECUTABLE}
                -m unittest
                tests/replay_video_ui_batch_contract_tests.py -v)
    set_tests_properties(replay_video_ui_batch_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME text_view_transient_buffer_contract
        COMMAND ${Python3_EXECUTABLE}
                -m unittest
                tests/text_view_transient_buffer_contract_tests.py -v)
    set_tests_properties(text_view_transient_buffer_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_library(ir_submission_eligibility_test_support STATIC
        src/ir/tachi/TachiEligibility.cpp
        src/ir/IrOutboxModels.cpp
        src/ScoreProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/Uuid.cpp
    )
    target_include_directories(ir_submission_eligibility_test_support PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_submission_eligibility_test_support PRIVATE
        cxx_std_23
    )
    add_test(NAME find_bms_archive_flow_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_find_bms_archive_flow.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(find_bms_archive_flow_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME judgement_indicator_range_flow_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_judgement_indicator_range_flow.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(judgement_indicator_range_flow_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
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
    add_test(NAME ir_uploads_flow_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_ir_uploads_flow.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(ir_uploads_flow_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME main_menu_chart_list_lifecycle_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_main_menu_chart_list_lifecycle.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(main_menu_chart_list_lifecycle_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME ir_ranking_detail_flow_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_ir_ranking_detail_flow.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(ir_ranking_detail_flow_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME ios_native_text_editor_sync_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_ios_native_text_editor_sync.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(ios_native_text_editor_sync_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME image_fade_shader_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_image_fade_shader.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(image_fade_shader_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME shader_compile_workflow_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_shader_compile_workflow.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(shader_compile_workflow_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME windows_skin_shader_artifact_contract
        COMMAND ${Python3_EXECUTABLE}
                -B
                -m unittest tests/windows_skin_shader_artifact_tests.py -v)
    set_tests_properties(windows_skin_shader_artifact_contract PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME records_ir_marker_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_records_ir_marker.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(records_ir_marker_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME saved_course_result_browse_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_saved_course_result_browse.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(saved_course_result_browse_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    add_test(NAME records_result_recall_flow_audit
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/scripts/check_records_result_recall_flow.py
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(records_result_recall_flow_audit PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
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
        src/ResultRecordSummary.cpp
        src/replay/ReplayCapabilities.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/bms_parser.cpp
    )
    target_include_directories(replay_record_filters_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(replay_record_filters_tests PRIVATE cxx_std_23)

    add_executable(replay_capabilities_tests
        tests/replay_capabilities_tests.cpp
        src/replay/ReplayCapabilities.cpp
    )
    target_include_directories(replay_capabilities_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_capabilities_tests PRIVATE cxx_std_23)

    add_executable(replay_limits_tests
        tests/replay_limits_tests.cpp
    )
    target_include_directories(replay_limits_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_limits_tests PRIVATE cxx_std_23)

    add_executable(durable_payload_contract_tests
        tests/durable_payload_contract_tests.cpp
    )
    target_include_directories(durable_payload_contract_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(durable_payload_contract_tests PRIVATE cxx_std_23)

    add_executable(replay_format_authority_tests
        tests/replay_format_authority_tests.cpp
    )
    target_include_directories(replay_format_authority_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_format_authority_tests PRIVATE cxx_std_23)

    add_executable(replay_setup_tests
        tests/replay_setup_tests.cpp
        src/ScoreProvenance.cpp
        src/replay/ReplaySetup.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(replay_setup_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_setup_tests PRIVATE cxx_std_20)
    set_property(TARGET replay_setup_tests PROPERTY CXX_STANDARD 20)

    add_executable(beatoraja_replay_path_tests
        tests/beatoraja_replay_path_tests.cpp
        src/replay/BeatorajaReplayPath.cpp
    )
    target_include_directories(beatoraja_replay_path_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(beatoraja_replay_path_tests PRIVATE cxx_std_20)
    set_property(TARGET beatoraja_replay_path_tests PROPERTY CXX_STANDARD 20)

    add_executable(replay_codec_primitive_tests
        tests/replay_codec_primitive_tests.cpp
        src/FileChecksum.cpp
        src/MinizBridge.c
        src/replay/Base64Url.cpp
        src/replay/GzipCodec.cpp
    )
    target_include_directories(replay_codec_primitive_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_definitions(replay_codec_primitive_tests PRIVATE
        ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_compile_features(replay_codec_primitive_tests PRIVATE cxx_std_23)

    add_executable(beatoraja_replay_codec_tests
        tests/beatoraja_replay_codec_tests.cpp
        src/MinizBridge.c
        src/ScoreProvenance.cpp
        src/replay/Base64Url.cpp
        src/replay/GzipCodec.cpp
        src/replay/BeatorajaReplayCodec.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplaySetup.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/bms_parser.cpp
    )
    target_include_directories(beatoraja_replay_codec_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_definitions(beatoraja_replay_codec_tests PRIVATE
        ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_compile_features(beatoraja_replay_codec_tests PRIVATE cxx_std_23)

    add_executable(replay_file_lifecycle_tests
        tests/replay_file_lifecycle_tests.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ReplayFileLifecycle.cpp
    )
    target_include_directories(replay_file_lifecycle_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_file_lifecycle_tests PRIVATE cxx_std_23)

    add_executable(replay_file_store_tests
        tests/replay_file_store_tests.cpp
        src/AtomicFile.cpp
        src/FileChecksum.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayFileStore.cpp
    )
    target_include_directories(replay_file_store_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_file_store_tests PRIVATE cxx_std_23)

    add_executable(replay_file_action_service_tests
        tests/replay_file_action_service_tests.cpp
        src/AtomicFile.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ModernResult.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ReplayFileAssociationCoordinator.cpp
        src/replay/ReplayFileActionService.cpp
        src/replay/ReplayFileReconciler.cpp
        src/replay/ReplayReferenceAgreement.cpp
        src/replay/ReplayProfileInventory.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayFileStore.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryIrRemoteScores.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrScoreReconciliation.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/IrSettingsPresentation.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/sqlite3.c
    )
    target_include_directories(replay_file_action_service_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_file_action_service_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(replay_file_action_service_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(replay_file_action_service_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(replay_file_action_service_tests PRIVATE SDL2)
    endif()

    add_executable(replay_file_reconciler_tests
        tests/replay_file_reconciler_tests.cpp
        src/replay/ReplayFileReconciler.cpp
    )
    target_include_directories(replay_file_reconciler_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_file_reconciler_tests PRIVATE cxx_std_23)

    add_executable(replay_profile_transfer_tests
        tests/replay_profile_transfer_tests.cpp
        src/AtomicFile.cpp
        src/FileChecksum.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayFileStore.cpp
        src/replay/ReplayProfileTransfer.cpp
    )
    target_include_directories(replay_profile_transfer_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_profile_transfer_tests PRIVATE cxx_std_23)

    add_executable(replay_codec_store_contract_tests
        tests/replay_codec_store_contract_tests.cpp
        src/AtomicFile.cpp
        src/FileChecksum.cpp
        src/MinizBridge.c
        src/ScoreProvenance.cpp
        src/bms_parser.cpp
        src/replay/Base64Url.cpp
        src/replay/BeatorajaReplayCodec.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/GzipCodec.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayFileStore.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplaySetup.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(replay_codec_store_contract_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_codec_store_contract_tests PRIVATE
        cxx_std_23
    )

    add_executable(replay_playback_tests
        tests/replay_playback_tests.cpp
        src/ScoreProvenance.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplaySetup.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(replay_playback_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_playback_tests PRIVATE cxx_std_23)

    add_executable(replay_input_recorder_tests
        tests/replay_input_recorder_tests.cpp
        src/replay/ReplayInputRecorder.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplaySetup.cpp
    )
    target_include_directories(replay_input_recorder_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_input_recorder_tests PRIVATE cxx_std_23)

    add_executable(course_continuation_tests
        tests/course_continuation_tests.cpp
        src/bms_parser.cpp
        src/ScoreProvenance.cpp
        src/replay/CourseContinuation.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ReplaySetup.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(course_continuation_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(course_continuation_tests PRIVATE cxx_std_23)

    add_executable(course_replay_capture_tests
        tests/course_replay_capture_tests.cpp
        src/FileChecksum.cpp
        src/ModernResult.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/bms_parser.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/CourseReplayAgreement.cpp
        src/replay/ReplayReferenceAgreement.cpp
        src/replay/CourseReplayCapture.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplaySetup.cpp
        src/replay/ReplaySetupProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(course_replay_capture_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(course_replay_capture_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(course_replay_capture_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(course_replay_capture_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(course_replay_capture_tests PRIVATE SDL2)
    endif()

    add_executable(result_record_summary_tests
        tests/result_record_summary_tests.cpp
        src/ResultRecordSummary.cpp
        src/ScoreProvenance.cpp
        src/replay/ReplayCapabilities.cpp
        src/replay/ReplayFileActionSelection.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/bms_parser.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(result_record_summary_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(result_record_summary_tests PRIVATE cxx_std_23)

    add_executable(course_identity_tests
        tests/course_identity_tests.cpp
        src/bms_parser.cpp
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
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrRankingModels.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ModernResult.cpp
        src/FileChecksum.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/bms_parser.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(ir_driver_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(ir_driver_tests PRIVATE cxx_std_23)

    add_executable(ir_account_lookup_service_tests
        tests/ir_account_lookup_service_tests.cpp
    )
    target_include_directories(ir_account_lookup_service_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_account_lookup_service_tests PRIVATE cxx_std_23)

    add_executable(ir_receipt_models_tests
        tests/ir_receipt_models_tests.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrProfileSettings.cpp
        src/Uuid.cpp
    )
    target_include_directories(ir_receipt_models_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_receipt_models_tests PRIVATE cxx_std_23)

    add_executable(ir_upload_candidates_tests
        tests/ir_upload_candidates_tests.cpp
        src/ir/IrReplayRecordState.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ir/tachi/TachiBatchManual.cpp
        src/FileChecksum.cpp
        src/ModernResult.cpp
        src/ScoreProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/ArchiveFile.cpp
        src/bms_parser.cpp
        src/MinizBridge.c
        src/repositories/ChartStorageIdentity.cpp
        src/path.cpp
    )
    target_include_directories(ir_upload_candidates_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_upload_candidates_tests PRIVATE cxx_std_23)
    target_link_libraries(ir_upload_candidates_tests PRIVATE ${COMMON_LIBS})
    if(APPLE)
        target_link_libraries(ir_upload_candidates_tests PRIVATE iconv)
    endif()

    add_executable(ir_remote_score_models_tests
        tests/ir_remote_score_models_tests.cpp
        src/ir/IrRemoteScoreModels.cpp
    )
    target_include_directories(ir_remote_score_models_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_remote_score_models_tests PRIVATE cxx_std_23)

    add_executable(ir_score_reconciliation_tests
        tests/ir_score_reconciliation_tests.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrScoreReconciliation.cpp
        src/Uuid.cpp
    )
    target_include_directories(ir_score_reconciliation_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_score_reconciliation_tests PRIVATE cxx_std_23)

    add_executable(ir_score_import_projection_tests
        tests/ir_score_import_projection_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryIrImport.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(ir_score_import_projection_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_score_import_projection_tests PRIVATE
        cxx_std_23
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(ir_score_import_projection_tests PRIVATE
            SDL2::SDL2
        )
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(ir_score_import_projection_tests PRIVATE
            SDL2::SDL2-static
        )
    elseif(TARGET SDL2)
        target_link_libraries(ir_score_import_projection_tests PRIVATE SDL2)
    endif()

    add_executable(tachi_user_score_parser_tests
        tests/tachi_user_score_parser_tests.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/tachi/TachiUserScoreParser.cpp
    )
    target_include_directories(tachi_user_score_parser_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(tachi_user_score_parser_tests PRIVATE cxx_std_23)

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
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrRankingModels.cpp
        src/ir/IrSubmission.cpp
        src/ir/tachi/BokutachiCacheStore.cpp
        src/ir/tachi/TachiBatchManual.cpp
        src/ir/tachi/TachiDriver.cpp
        src/ir/tachi/TachiRankingParser.cpp
        src/ir/tachi/TachiResponseParser.cpp
        src/ir/tachi/TachiUserScoreParser.cpp
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
        src/AtomicFile.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/VersionedJson.cpp
        src/ir/IrDriver.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrScoreReconciliation.cpp
        src/ir/IrRankingModels.cpp
        src/ir/IrSettingsPresentation.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ir/IrSubmissionService.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/tachi/BokutachiCacheStore.cpp
        src/ir/tachi/TachiBatchManual.cpp
        src/ir/tachi/TachiDriver.cpp
        src/ir/tachi/TachiRankingParser.cpp
        src/ir/tachi/TachiResponseParser.cpp
        src/ir/tachi/TachiUserScoreParser.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryIrRemoteScores.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/ScoreProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
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

    add_executable(ir_saved_result_upload_tests
        tests/ir_saved_result_upload_tests.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrSavedResultUpload.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
    )
    target_include_directories(ir_saved_result_upload_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_saved_result_upload_tests PRIVATE cxx_std_23)

    add_executable(ir_saved_result_batch_upload_tests
        tests/ir_saved_result_batch_upload_tests.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrSavedResultBatchUpload.cpp
        src/ir/IrSubmission.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
    )
    target_include_directories(ir_saved_result_batch_upload_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_saved_result_batch_upload_tests PRIVATE
        cxx_std_23
    )

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

    add_executable(replay_legacy_migration_tests
        tests/replay_legacy_migration_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryIrRemoteScores.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrScoreReconciliation.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/IrSettingsPresentation.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(replay_legacy_migration_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_legacy_migration_tests PRIVATE cxx_std_23)
    target_compile_definitions(replay_legacy_migration_tests PRIVATE
        ASOBMASHOW_ENABLE_REPLAY_MIGRATION_TEST_ACCESS=1
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(replay_legacy_migration_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(replay_legacy_migration_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(replay_legacy_migration_tests PRIVATE SDL2)
    endif()

    add_executable(legacy_result_summary_tests
        tests/legacy_result_summary_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryIrRemoteScores.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/repositories/ReplayRepositoryLegacySummaries.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrScoreReconciliation.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/IrSettingsPresentation.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(legacy_result_summary_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(legacy_result_summary_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(legacy_result_summary_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(legacy_result_summary_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(legacy_result_summary_tests PRIVATE SDL2)
    endif()

    add_executable(replay_repository_modern_chart_tests
        tests/replay_repository_modern_chart_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryIrRemoteScores.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrScoreReconciliation.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/IrSettingsPresentation.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(replay_repository_modern_chart_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_repository_modern_chart_tests PRIVATE
        cxx_std_23
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(replay_repository_modern_chart_tests PRIVATE
            SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(replay_repository_modern_chart_tests PRIVATE
            SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(replay_repository_modern_chart_tests PRIVATE
            SDL2)
    endif()

    add_executable(replay_repository_modern_course_tests
        tests/replay_repository_modern_course_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryIrRemoteScores.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrScoreReconciliation.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/IrSettingsPresentation.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ReplayProfileInventory.cpp
        src/replay/ReplayReferenceAgreement.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(replay_repository_modern_course_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_repository_modern_course_tests PRIVATE
        cxx_std_23
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(replay_repository_modern_course_tests PRIVATE
            SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(replay_repository_modern_course_tests PRIVATE
            SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(replay_repository_modern_course_tests PRIVATE
            SDL2)
    endif()

    add_executable(chart_replay_persistence_tests
        tests/chart_replay_persistence_tests.cpp
        src/AtomicFile.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/MinizBridge.c
        src/ModernResult.cpp
        src/ResultPersistenceCoordinator.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/replay/Base64Url.cpp
        src/replay/BeatorajaReplayCodec.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ChartReplayCapture.cpp
        src/replay/ChartReplayAgreement.cpp
        src/replay/ReplaySetupProvenance.cpp
        src/replay/ChartReplayPersistence.cpp
        src/replay/GzipCodec.cpp
        src/replay/ReplayFileAssociationCoordinator.cpp
        src/replay/ReplayFileActionService.cpp
        src/replay/ReplayCapabilities.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayReferenceAgreement.cpp
        src/replay/ReplayFileStore.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplaySetup.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryIrRemoteScores.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrScoreReconciliation.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/IrSettingsPresentation.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/sqlite3.c
    )
    target_include_directories(chart_replay_persistence_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(chart_replay_persistence_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(chart_replay_persistence_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(chart_replay_persistence_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(chart_replay_persistence_tests PRIVATE SDL2)
    endif()

    add_executable(replay_file_association_coordinator_tests
        tests/replay_file_association_coordinator_tests.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ReplayFileAssociationCoordinator.cpp
    )
    target_include_directories(replay_file_association_coordinator_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_file_association_coordinator_tests PRIVATE
        cxx_std_23
    )

    add_executable(course_replay_persistence_tests
        tests/course_replay_persistence_tests.cpp
        src/AtomicFile.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/MinizBridge.c
        src/ModernResult.cpp
        src/ResultPersistenceCoordinator.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/replay/Base64Url.cpp
        src/replay/BeatorajaReplayCodec.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/CourseReplayAgreement.cpp
        src/replay/ReplayReferenceAgreement.cpp
        src/replay/CourseReplayCapture.cpp
        src/replay/CourseReplayPersistence.cpp
        src/replay/CourseResultPersistence.cpp
        src/replay/GzipCodec.cpp
        src/replay/ReplayFileAssociationCoordinator.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayFileStore.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplaySetup.cpp
        src/replay/ReplaySetupProvenance.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryIrRemoteScores.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrScoreReconciliation.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/IrSettingsPresentation.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/sqlite3.c
    )
    target_include_directories(course_replay_persistence_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(course_replay_persistence_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(course_replay_persistence_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(course_replay_persistence_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(course_replay_persistence_tests PRIVATE SDL2)
    endif()

    add_executable(chart_replay_context_tests
        tests/chart_replay_context_tests.cpp
        src/AtomicFile.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/MinizBridge.c
        src/ModernResult.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/replay/Base64Url.cpp
        src/replay/BeatorajaReplayCodec.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ChartReplayAgreement.cpp
        src/replay/ReplaySetupProvenance.cpp
        src/replay/ChartReplayContext.cpp
        src/replay/ReplayReferenceAgreement.cpp
        src/replay/GzipCodec.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayFileStore.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplaySetup.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryIrRemoteScores.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrScoreReconciliation.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/IrSettingsPresentation.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/sqlite3.c
    )
    target_include_directories(chart_replay_context_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(chart_replay_context_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(chart_replay_context_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(chart_replay_context_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(chart_replay_context_tests PRIVATE SDL2)
    endif()

    add_executable(course_replay_context_tests
        tests/course_replay_context_tests.cpp
        src/AtomicFile.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/MinizBridge.c
        src/ModernResult.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/replay/Base64Url.cpp
        src/replay/BeatorajaReplayCodec.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/CourseReplayAgreement.cpp
        src/replay/CourseReplayContext.cpp
        src/replay/ReplayReferenceAgreement.cpp
        src/replay/GzipCodec.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayFileStore.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplaySetup.cpp
        src/replay/ReplaySetupProvenance.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryIrRemoteScores.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/ir/IrScoreReconciliation.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/IrSettingsPresentation.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/sqlite3.c
    )
    target_include_directories(course_replay_context_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(course_replay_context_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(course_replay_context_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(course_replay_context_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(course_replay_context_tests PRIVATE SDL2)
    endif()

    add_executable(course_replay_consumer_tests
        tests/course_replay_consumer_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ModernResult.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/replay/CourseContinuation.cpp
        src/replay/CourseReplayConsumer.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplayPlaybackDriver.cpp
        src/replay/ReplayPlaybackMaterializer.cpp
        src/replay/ReplaySetup.cpp
        src/replay/ReplaySetupAdapter.cpp
        src/bms_parser.cpp
        src/scene/play/CompiledGameplayJudge.cpp
        src/scene/play/GameplayCandidateRules.cpp
        src/scene/play/GameplayDefinition.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/GameplayNoteJudgeRole.cpp
        src/scene/play/GameplayRulesetPolicy.cpp
        src/scene/play/GameplaySimulation.cpp
        src/scene/play/SkinGameplayGraphState.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(course_replay_consumer_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(course_replay_consumer_tests PRIVATE cxx_std_23)

    add_executable(replay_playback_driver_tests
        tests/replay_playback_driver_tests.cpp
        src/FileChecksum.cpp
        src/ModernResult.cpp
        src/ResultPersistenceModel.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/replay/ChartReplayConsumer.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplayPlaybackDriver.cpp
        src/replay/ReplayPlaybackMaterializer.cpp
        src/replay/ReplaySetup.cpp
        src/replay/ReplaySetupAdapter.cpp
        src/bms_parser.cpp
        src/scene/play/CompiledGameplayJudge.cpp
        src/scene/play/GameplayCandidateRules.cpp
        src/scene/play/GameplayDefinition.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/GameplayNoteJudgeRole.cpp
        src/scene/play/GameplayRulesetPolicy.cpp
        src/scene/play/GameplaySimulation.cpp
        src/scene/play/SkinGameplayGraphState.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(replay_playback_driver_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_playback_driver_tests PRIVATE cxx_std_23)

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
        src/ir/IrOutboxModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/repositories/ChartRepository.cpp
        src/repositories/ChartScanStore.cpp
        src/repositories/ScoreRepositoryChartSession.cpp
        src/repositories/ChartRepositoryDifficulty.cpp
        src/repositories/ChartRepositoryQueries.cpp
        src/repositories/ChartStorageIdentity.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryIrImport.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
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

    add_executable(chart_scan_work_scheduler_tests
        tests/chart_scan_work_scheduler_tests.cpp
        src/ChartScanWorkScheduler.cpp
    )
    target_include_directories(chart_scan_work_scheduler_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(chart_scan_work_scheduler_tests PRIVATE cxx_std_23)

    add_executable(archive_file_concurrency_tests
        tests/archive_file_concurrency_tests.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/bms_parser.cpp
        src/path.cpp
    )
    target_include_directories(archive_file_concurrency_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(archive_file_concurrency_tests PRIVATE cxx_std_23)
    target_link_libraries(archive_file_concurrency_tests PRIVATE ${COMMON_LIBS})
    if(NOT WIN32)
        set(SEVENZIP_TEST_SUPPORT_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/7zip-test-support")
        set(SEVENZIP_TEST_LINK_OBJECTS
            "${SEVENZIP_TEST_SUPPORT_DIR}/7zRegister.cpp.o"
            "${SEVENZIP_TEST_SUPPORT_DIR}/LzmaRegister.cpp.o"
            "${SEVENZIP_TEST_SUPPORT_DIR}/Lzma2Register.cpp.o"
            "${SEVENZIP_TEST_SUPPORT_DIR}/DeltaFilter.cpp.o"
        )
        add_custom_command(
            OUTPUT ${SEVENZIP_TEST_LINK_OBJECTS}
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${SEVENZIP_TEST_SUPPORT_DIR}"
            COMMAND ${CMAKE_COMMAND} -E rm -f
                ${SEVENZIP_TEST_LINK_OBJECTS}
            COMMAND ${CMAKE_AR} -x "$<TARGET_FILE:7zip::7zip>"
                7zRegister.cpp.o
                LzmaRegister.cpp.o
                Lzma2Register.cpp.o
                DeltaFilter.cpp.o
            WORKING_DIRECTORY "${SEVENZIP_TEST_SUPPORT_DIR}"
            COMMENT "Extracting 7-Zip registrations for concurrency test"
            VERBATIM
        )
        set_source_files_properties(${SEVENZIP_TEST_LINK_OBJECTS}
            PROPERTIES
            EXTERNAL_OBJECT TRUE
            GENERATED TRUE
        )
        target_sources(archive_file_concurrency_tests PRIVATE
            ${SEVENZIP_TEST_LINK_OBJECTS})
    endif()
    if(APPLE)
        target_link_libraries(archive_file_concurrency_tests PRIVATE iconv)
    endif()
    if(WIN32)
        target_link_libraries(archive_file_concurrency_tests PRIVATE shell32 ole32)
    endif()

    add_executable(chart_library_scanner_tests
        tests/chart_library_scanner_tests.cpp
        src/ChartLibraryScanner.cpp
        src/ChartScanWorkScheduler.cpp
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
        src/ModernResult.cpp
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
    target_compile_definitions(chart_library_scanner_tests PRIVATE
        ASOBMASHOW_ARCHIVE_FILE_STREAMING_TEST_HOOKS=1
    )
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
        src/ModernResult.cpp
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
        src/ModernResult.cpp
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

    add_executable(modern_result_tests
        tests/modern_result_tests.cpp
        src/ModernResult.cpp
        src/FileChecksum.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/bms_parser.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(modern_result_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(modern_result_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(modern_result_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(modern_result_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(modern_result_tests PRIVATE SDL2)
    endif()

    add_executable(result_contract_tests
        tests/result_contract_tests.cpp
    )
    target_include_directories(result_contract_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(result_contract_tests PRIVATE cxx_std_23)

    add_executable(ir_submission_snapshot_tests
        tests/ir_submission_snapshot_tests.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ModernResult.cpp
        src/FileChecksum.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/bms_parser.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(ir_submission_snapshot_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_submission_snapshot_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(ir_submission_snapshot_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(ir_submission_snapshot_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(ir_submission_snapshot_tests PRIVATE SDL2)
    endif()

    add_executable(modern_result_recall_tests
        tests/modern_result_recall_tests.cpp
        src/ModernResultRecallBuilder.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/ModernResult.cpp
        src/FileChecksum.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/bms_parser.cpp
        src/repositories/ChartStorageIdentity.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(modern_result_recall_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(modern_result_recall_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(modern_result_recall_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(modern_result_recall_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(modern_result_recall_tests PRIVATE SDL2)
    endif()
    target_link_libraries(modern_result_recall_tests PRIVATE ${COMMON_LIBS})
    if(APPLE)
        target_link_libraries(modern_result_recall_tests PRIVATE iconv)
    endif()
    if(WIN32)
        target_link_libraries(modern_result_recall_tests PRIVATE shell32 ole32)
    endif()

    add_executable(score_provenance_db_tests
        tests/score_provenance_db_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/ScoreProvenance.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
        src/bms_parser.cpp
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

    add_executable(best_replay_resolver_tests
        tests/best_replay_resolver_tests.cpp
        src/ScoreProvenance.cpp
        src/replay/BestReplayResolver.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(best_replay_resolver_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(best_replay_resolver_tests PRIVATE cxx_std_23)

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
        src/FileChecksum.cpp
        src/ir/IrProfileSettings.cpp
        src/Utils.cpp
        src/path.cpp
        src/settings/AudioVideoSettings.cpp
        src/skin/SkinProfileSettings.cpp
        src/skin/package/SkinPathPolicy.cpp
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
    target_link_libraries(audio_video_settings_tests PRIVATE utf8proc::utf8proc)
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
        src/skin/beatoraja/LuaSkinApplicationAudioBackend.cpp
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
        src/audio/GameplayBgaMissStateTracker.cpp
        src/audio/Jukebox.cpp
        src/audio/PrepMetronomeSound.cpp
        src/audio/decoder.cpp
        src/bms_parser.cpp
        src/path.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/skin/beatoraja/SkinDestinationEvaluator.cpp
        src/utils/Stopwatch.cpp
        src/video/VideoDecodeState.cpp
        src/video/VideoFrameLayout.cpp
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

    add_executable(video_frame_layout_tests
        tests/video_frame_layout_tests.cpp
        src/video/VideoFrameLayout.cpp
    )
    target_include_directories(video_frame_layout_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(video_frame_layout_tests PRIVATE cxx_std_23)

    add_executable(video_decode_state_tests
        tests/video_decode_state_tests.cpp
        src/video/VideoDecodeState.cpp
    )
    target_include_directories(video_decode_state_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(video_decode_state_tests PRIVATE cxx_std_23)

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

    add_executable(image_fade_tests
        tests/image_fade_tests.cpp
        src/view/ImageFade.cpp
    )
    target_include_directories(image_fade_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(image_fade_tests PRIVATE cxx_std_23)

    add_executable(decoded_image_cache_tests
        tests/decoded_image_cache_tests.cpp
        src/view/DecodedImageCache.cpp
    )
    target_include_directories(decoded_image_cache_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(decoded_image_cache_tests PRIVATE cxx_std_23)

    add_executable(image_decode_coordinator_tests
        tests/image_decode_coordinator_tests.cpp
        src/view/DecodedImageCache.cpp
        src/view/ImageDecodeCoordinator.cpp
    )
    target_include_directories(image_decode_coordinator_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(image_decode_coordinator_tests PRIVATE cxx_std_23)

    add_executable(image_file_decoder_tests
        tests/image_file_decoder_tests.cpp
        src/MinizBridge.c
        src/path.cpp
        src/view/DecodedImageCache.cpp
        src/view/ImageFileDecoder.cpp
    )
    target_include_directories(image_file_decoder_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(image_file_decoder_tests PRIVATE cxx_std_23)
    target_compile_definitions(image_file_decoder_tests PRIVATE ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(image_file_decoder_tests PRIVATE ${FFMPEG_LIBRARIES})

    if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS AND NOT IOS)
        add_executable(gameplay_skin_validator_tests
            tests/gameplay_skin_validator_tests.cpp
            src/MinizBridge.c
            src/view/DecodedImageCache.cpp
            src/view/ImageDecodeCoordinator.cpp
            src/view/ImageFileDecoder.cpp
            src/view/SdlTtfRuntime.cpp
            src/FileChecksum.cpp
            src/Utils.cpp
            src/path.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
            src/skin/beatoraja/GameplaySkinBuiltinCatalog.cpp
            src/skin/beatoraja/GameplaySkinDocumentLoader.cpp
            src/skin/beatoraja/GameplaySkinSourceFormat.cpp
            src/skin/beatoraja/JsonGameplaySkinDecoder.cpp
            src/skin/beatoraja/Lr2GameplaySkinDecoder.cpp
            src/skin/beatoraja/Lr2SkinCsvParser.cpp
            src/skin/beatoraja/Lr2SkinHeaderDecoder.cpp
            src/skin/beatoraja/GameplaySkinValidator.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/LuaSkinTableDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinResourceCatalog.cpp
            src/skin/beatoraja/PomyuCharaResource.cpp
            src/skin/beatoraja/SkinBitmapFontParser.cpp
            src/skin/beatoraja/SkinTextAtlas.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
        )
        target_include_directories(gameplay_skin_validator_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(gameplay_skin_validator_tests PRIVATE cxx_std_23)
        target_compile_definitions(gameplay_skin_validator_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(gameplay_skin_validator_tests PRIVATE
            ${COMMON_LIBS}
            bgfx
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(gameplay_skin_validator_tests PRIVATE ${LUAJIT_LIB})
        else()
            target_link_libraries(gameplay_skin_validator_tests PRIVATE ${LUAJIT_LIBRARIES})
        endif()
        if(APPLE)
            target_sources(gameplay_skin_validator_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(gameplay_skin_validator_tests PRIVATE "-framework Foundation")
        endif()

        add_executable(skin_resource_catalog_tests
            tests/skin_resource_catalog_tests.cpp
            src/MinizBridge.c
            src/view/DecodedImageCache.cpp
            src/view/ImageDecodeCoordinator.cpp
            src/view/ImageFileDecoder.cpp
            src/view/SdlTtfRuntime.cpp
            src/FileChecksum.cpp
            src/Utils.cpp
            src/path.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/SkinMovieCatalog.cpp
            src/skin/beatoraja/SkinMovieCatalogFrame.cpp
            src/skin/beatoraja/SkinResourceCatalog.cpp
            src/skin/beatoraja/PomyuCharaResource.cpp
            src/skin/beatoraja/SkinBitmapFontParser.cpp
            src/skin/beatoraja/SkinTextAtlas.cpp
        )
        if(APPLE)
            target_sources(skin_resource_catalog_tests PRIVATE src/skin/package/SkinAliasDetectorApple.mm)
        endif()
        target_include_directories(skin_resource_catalog_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_compile_features(skin_resource_catalog_tests PRIVATE cxx_std_23)
        target_compile_definitions(skin_resource_catalog_tests PRIVATE ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}" ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1 ASOBMASHOW_SKIN_RESOURCE_TESTING=1)
        target_link_libraries(skin_resource_catalog_tests PRIVATE ${COMMON_LIBS} bgfx utf8proc::utf8proc)

        add_executable(skin_live_resource_counters_tests
            tests/skin_live_resource_counters_tests.cpp
        )
        target_include_directories(skin_live_resource_counters_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(skin_live_resource_counters_tests PRIVATE cxx_std_23)

        add_executable(skin_process_resident_memory_tests
            tests/skin_process_resident_memory_tests.cpp
            src/skin/beatoraja/SkinProcessResidentMemory.cpp
        )
        target_include_directories(skin_process_resident_memory_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(skin_process_resident_memory_tests PRIVATE cxx_std_23)
    endif()

    add_executable(image_view_fade_tests
        tests/image_view_fade_tests.cpp
        src/view/DecodedImageCache.cpp
        src/view/ImageDecodeCoordinator.cpp
        src/view/ImageFade.cpp
        src/view/ImageFileDecoder.cpp
        src/view/ImageView.cpp
        src/view/SdlTtfRuntime.cpp
        src/view/View.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/bms_parser.cpp
        src/path.cpp
    )
    target_include_directories(image_view_fade_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(image_view_fade_tests PRIVATE cxx_std_23)
    target_compile_definitions(image_view_fade_tests PRIVATE
        ASOBMASHOW_IMAGE_VIEW_TESTING=1
    )
    target_link_libraries(image_view_fade_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )
    if(APPLE)
        target_link_libraries(image_view_fade_tests PRIVATE iconv)
    endif()
    add_test(NAME image_view_decoder_ownership_tests
        COMMAND ${CMAKE_COMMAND}
            -DIMAGE_VIEW_SOURCE=${CMAKE_SOURCE_DIR}/src/view/ImageView.cpp
            -P ${CMAKE_SOURCE_DIR}/tests/assert_image_view_decoder_ownership.cmake
    )

    add_executable(chart_list_item_view_tests
        tests/chart_list_item_view_tests.cpp
        src/view/DecodedImageCache.cpp
        src/view/ImageDecodeCoordinator.cpp
        src/view/ChartListItemView.cpp
        src/view/ImageFade.cpp
        src/view/ImageFileDecoder.cpp
        src/view/ImageView.cpp
        src/view/SdlTtfRuntime.cpp
        src/view/Button.cpp
        src/view/TextView.cpp
        src/view/View.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/bms_parser.cpp
        src/path.cpp
    )
    target_include_directories(chart_list_item_view_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(chart_list_item_view_tests PRIVATE cxx_std_23)
    target_link_libraries(chart_list_item_view_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )
    if(APPLE)
        target_link_libraries(chart_list_item_view_tests PRIVATE iconv)
    endif()

    add_executable(dropdown_view_tests
        tests/dropdown_view_tests.cpp
        src/bms_parser.cpp
    )
    target_include_directories(dropdown_view_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(dropdown_view_tests PRIVATE cxx_std_23)

    add_executable(context_menu_view_tests
        tests/context_menu_view_tests.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/SimpleBatchRenderer.cpp
        src/rendering/UniformCache.cpp
        src/view/Button.cpp
        src/view/ContextMenuView.cpp
        src/view/ScrollView.cpp
        src/view/SdlTtfRuntime.cpp
        src/view/TextView.cpp
        src/view/View.cpp
    )
    target_include_directories(context_menu_view_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(context_menu_view_tests PRIVATE cxx_std_23)
    target_link_libraries(context_menu_view_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )

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

    add_executable(text_input_box_tests
        tests/text_input_box_tests.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/view/Button.cpp
        src/view/SdlTtfRuntime.cpp
        src/view/TextInputBox.cpp
        src/view/TextView.cpp
        src/view/View.cpp
    )
    target_include_directories(text_input_box_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(text_input_box_tests PRIVATE cxx_std_23)
    target_compile_definitions(text_input_box_tests PRIVATE
        SDL_ClearComposition=TextInputBoxTest_ClearComposition
    )
    target_link_libraries(text_input_box_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )

    add_executable(checkbox_button_content_tests
        tests/checkbox_button_content_tests.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/view/CheckboxButtonContent.cpp
        src/view/SdlTtfRuntime.cpp
        src/view/TextView.cpp
        src/view/View.cpp
    )
    target_include_directories(checkbox_button_content_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(checkbox_button_content_tests PRIVATE cxx_std_23)
    target_link_libraries(checkbox_button_content_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )

    add_executable(difficulty_table_url_completion_tests
        tests/difficulty_table_url_completion_tests.cpp
    )
    target_include_directories(difficulty_table_url_completion_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(difficulty_table_url_completion_tests PRIVATE
        cxx_std_23
    )

    add_executable(play_options_panel_view_tests
        tests/play_options_panel_view_tests.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/SimpleBatchRenderer.cpp
        src/rendering/UniformCache.cpp
        src/view/Button.cpp
        src/view/CheckboxButtonContent.cpp
        src/view/DropdownView.cpp
        src/view/PlayOptionSectionView.cpp
        src/view/PlayOptionsPanelView.cpp
        src/view/SdlTtfRuntime.cpp
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

    add_executable(replay_summary_list_view_tests
        tests/replay_summary_list_view_tests.cpp
        src/bms_parser.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/view/Button.cpp
        src/view/SdlTtfRuntime.cpp
        src/view/TextView.cpp
        src/view/View.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(replay_summary_list_view_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(replay_summary_list_view_tests PRIVATE cxx_std_23)
    target_link_libraries(replay_summary_list_view_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )

    add_executable(result_record_list_view_tests
        tests/result_record_list_view_tests.cpp
        src/ResultRecordSummary.cpp
        src/replay/ReplayCapabilities.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/bms_parser.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/view/Button.cpp
        src/view/SdlTtfRuntime.cpp
        src/view/TextView.cpp
        src/view/View.cpp
        src/scene/play/GameplayGaugeRules.cpp
    )
    target_include_directories(result_record_list_view_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(result_record_list_view_tests PRIVATE cxx_std_23)
    target_link_libraries(result_record_list_view_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )

    add_executable(ir_upload_candidate_list_view_tests
        tests/ir_upload_candidate_list_view_tests.cpp
        src/view/DecodedImageCache.cpp
        src/view/ImageDecodeCoordinator.cpp
        src/view/IrUploadCandidateListView.cpp
        src/view/ImageFade.cpp
        src/view/ImageFileDecoder.cpp
        src/view/ImageView.cpp
        src/view/SdlTtfRuntime.cpp
        src/view/Button.cpp
        src/view/CheckboxButtonContent.cpp
        src/view/TextView.cpp
        src/view/View.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/ir/IrReplayRecordState.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/IrOutboxModels.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ir/tachi/TachiBatchManual.cpp
        src/FileChecksum.cpp
        src/ModernResult.cpp
        src/ScoreProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/repositories/ChartStorageIdentity.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/bms_parser.cpp
        src/path.cpp
    )
    target_include_directories(ir_upload_candidate_list_view_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_upload_candidate_list_view_tests PRIVATE
        cxx_std_23
    )
    target_link_libraries(ir_upload_candidate_list_view_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )
    if(APPLE)
        target_link_libraries(ir_upload_candidate_list_view_tests PRIVATE iconv)
    endif()

    add_executable(ir_uploads_controller_tests
        tests/ir_uploads_controller_tests.cpp
        src/scene/IrUploadsController.cpp
        src/ir/IrOutboxModels.cpp
        src/ScoreProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/Uuid.cpp
    )
    target_include_directories(ir_uploads_controller_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_uploads_controller_tests PRIVATE cxx_std_23)

    add_executable(score_provenance_tests
        tests/score_provenance_tests.cpp
        src/bms_parser.cpp
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

    add_executable(judgement_indicator_range_tests
        tests/judgement_indicator_range_tests.cpp
    )
    target_include_directories(judgement_indicator_range_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(judgement_indicator_range_tests PRIVATE cxx_std_23)

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
        src/input/VirtualControllerConfig.cpp
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
            src/skin/package/SkinPathPolicy.cpp
        )
        target_include_directories(platform_document_handoff_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(platform_document_handoff_tests PRIVATE cxx_std_23)
        target_link_libraries(platform_document_handoff_tests PRIVATE TinyFileDialogs utf8proc::utf8proc)
        if (WIN32)
            target_link_libraries(platform_document_handoff_tests PRIVATE advapi32)
        endif()

        add_executable(profile_export_staging_tests
            tests/profile_export_staging_tests.cpp
            src/ProfileExportStaging.cpp
            src/PlatformDocumentHandoff.cpp
            src/skin/package/SkinPathPolicy.cpp
        )
        target_include_directories(profile_export_staging_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(profile_export_staging_tests PRIVATE cxx_std_23)
        target_link_libraries(profile_export_staging_tests PRIVATE TinyFileDialogs utf8proc::utf8proc)
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
        src/input/VirtualControllerConfig.cpp
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
        src/FileChecksum.cpp
        src/VersionedJson.cpp
        src/AppSettings.cpp
        src/skin/SkinProfileSettings.cpp
        src/skin/package/SkinPathPolicy.cpp
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
    target_link_libraries(app_settings_store_tests PRIVATE utf8proc::utf8proc)

    add_executable(profile_settings_persistence_tests
        tests/profile_settings_persistence_tests.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ProfileSettingsPersistenceCoordinator.cpp
        src/PlayerProfileManager.cpp
        src/Uuid.cpp
        src/ProfileDatabaseTools.cpp
        src/AppSettingsStore.cpp
        src/AtomicFile.cpp
        src/VersionedJson.cpp
        src/AppSettings.cpp
        src/skin/SkinProfileSettings.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/ir/IrProfileSettings.cpp
        src/settings/AudioVideoSettings.cpp
        src/input/GyroscopeTurntable.cpp
        src/input/InputProfile.cpp
        src/input/VirtualControllerConfig.cpp
        src/input/InputDefaults.cpp
        src/input/InputProfileStore.cpp
        src/practice/PracticeConfiguration.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/Judge.cpp
        src/practice/PracticePresetStore.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ir/IrUploadCandidates.cpp
        src/ir/tachi/TachiEligibility.cpp
        src/ir/IrOutboxModels.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
        src/ScoreProvenance.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayFileStore.cpp
        src/replay/ReplayProfileTransfer.cpp
        src/replay/ReplayProfileInventory.cpp
        src/replay/ReplayReferenceAgreement.cpp
        src/Utils.cpp
        src/path.cpp
        src/sqlite3.c
    )
    target_include_directories(profile_settings_persistence_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(profile_settings_persistence_tests PRIVATE cxx_std_23)
    target_link_libraries(profile_settings_persistence_tests PRIVATE ${COMMON_LIBS})

    add_executable(find_bms_download_tests
        tests/find_bms_download_tests.cpp
        src/bms_search/ArchiveDecision.cpp
        src/bms_search/DownloadedArchiveWorkflow.cpp
        src/bms_search/DownloadStaging.cpp
        src/bms_search/DownloadStorageIdentity.cpp
        src/bms_search/PackageDownloadCandidate.cpp
        src/bms_search/PendingArtifact.cpp
        src/scene/FindBmsDialogPolicy.cpp
        src/scene/FindBmsProgressPresentation.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/bms_parser.cpp
        src/Uuid.cpp
        src/Utils.cpp
        src/path.cpp
    )
    target_include_directories(find_bms_download_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(find_bms_download_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(find_bms_download_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(find_bms_download_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(find_bms_download_tests PRIVATE SDL2)
    endif()
    target_link_libraries(find_bms_download_tests PRIVATE ${COMMON_LIBS})
    if(APPLE)
        target_link_libraries(find_bms_download_tests PRIVATE iconv)
    endif()
    if(WIN32)
        target_link_libraries(find_bms_download_tests PRIVATE shell32 ole32)
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

    add_executable(ir_credential_migration_tests
        tests/ir_credential_migration_tests.cpp
        src/ir/IrCredentialBackend.cpp
        src/ir/IrCredentialMigration.cpp
        src/ir/IrCredentialStore.cpp
        src/AtomicFile.cpp
        src/VersionedJson.cpp
    )
    target_include_directories(ir_credential_migration_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ir_credential_migration_tests PRIVATE cxx_std_23)

    add_executable(pending_ir_credential_cleanup_tests
        tests/pending_ir_credential_cleanup_tests.cpp
        src/ir/PendingIrCredentialCleanup.cpp
        src/ir/IrCredentialBackend.cpp
        src/ir/IrCredentialStore.cpp
        src/AtomicFile.cpp
        src/VersionedJson.cpp
    )
    target_include_directories(pending_ir_credential_cleanup_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(pending_ir_credential_cleanup_tests PRIVATE
        cxx_std_23
    )

    if (IOS)
        add_executable(ios_keychain_credential_tests
            tests/ios_keychain_credential_tests.mm
            src/ir/IrCredentialBackend.cpp
            src/ir/IosKeychainCredentialBackend.mm
            src/ir/IrCredentialStore.cpp
            src/AtomicFile.cpp
            src/VersionedJson.cpp
        )
        target_include_directories(ios_keychain_credential_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(ios_keychain_credential_tests PRIVATE cxx_std_23)
        target_link_libraries(ios_keychain_credential_tests PRIVATE
            ${FOUNDATION} ${SECURITY}
        )
    endif()

    add_executable(practice_configuration_tests
        tests/practice_configuration_tests.cpp
        src/practice/PracticeConfiguration.cpp
        src/practice/PracticeSession.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/bms_parser.cpp
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

    add_executable(playfield_visual_state_tests
        tests/playfield_visual_state_tests.cpp
        src/bms_parser.cpp
        src/audio/GameplayBgaMissStateTracker.cpp
        src/scene/play/Judge.cpp
        src/scene/play/PlayfieldChartVisualModel.cpp
        src/scene/play/PlayfieldVisualState.cpp
        src/scene/play/SkinGameplayGraphState.cpp
    )
    target_include_directories(playfield_visual_state_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(playfield_visual_state_tests PRIVATE cxx_std_23)

    add_executable(gameplay_bga_target_tests
        tests/gameplay_bga_target_tests.cpp
        src/ArchiveFile.cpp
        src/MinizBridge.c
        src/Utils.cpp
        src/audio/AudioBackend.cpp
        src/audio/AudioDeviceManager.cpp
        src/audio/AudioMix.cpp
        src/audio/AudioWrapper.cpp
        src/audio/ClubBeat.cpp
        src/audio/GameplayBgaMissStateTracker.cpp
        src/audio/Jukebox.cpp
        src/audio/PrepMetronomeSound.cpp
        src/audio/decoder.cpp
        src/bms_parser.cpp
        src/path.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/utils/Stopwatch.cpp
        src/video/VideoDecodeState.cpp
        src/video/VideoFrameLayout.cpp
        src/video/VideoPlayer.cpp
    )
    target_include_directories(gameplay_bga_target_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_bga_target_tests PRIVATE cxx_std_23)
    target_compile_definitions(gameplay_bga_target_tests PRIVATE
        ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(gameplay_bga_target_tests PRIVATE ${COMMON_LIBS} bgfx)
    if(APPLE)
        target_link_libraries(gameplay_bga_target_tests PRIVATE iconv)
    endif()
    if(WIN32)
        target_link_libraries(gameplay_bga_target_tests PRIVATE shell32 ole32)
    endif()

    add_executable(bgfx_skin_texture_device_tests
        tests/bgfx_skin_texture_device_tests.cpp
        src/skin/beatoraja/BgfxSkinTextureDevice.cpp
    )
    target_include_directories(bgfx_skin_texture_device_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(bgfx_skin_texture_device_tests PRIVATE cxx_std_23)
    target_link_libraries(bgfx_skin_texture_device_tests PRIVATE ${COMMON_LIBS} bgfx)

    add_executable(playfield_chart_visual_model_tests
        tests/playfield_chart_visual_model_tests.cpp
        src/bms_parser.cpp
        src/scene/play/PlayfieldChartVisualModel.cpp
    )
    target_include_directories(playfield_chart_visual_model_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(playfield_chart_visual_model_tests PRIVATE cxx_std_23)

    add_executable(playfield_projection_tests
        tests/playfield_projection_tests.cpp
        src/bms_parser.cpp
        src/scene/play/PlayfieldChartVisualModel.cpp
        src/scene/play/PlayfieldProjection.cpp
    )
    target_include_directories(playfield_projection_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(playfield_projection_tests PRIVATE cxx_std_23)
    target_compile_definitions(playfield_projection_tests PRIVATE
        ASOBMASHOW_PLAYFIELD_PROJECTION_TESTING=1
    )

    if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS AND NOT IOS)
        add_executable(gameplay_skin_session_factory_tests
            tests/gameplay_skin_session_factory_tests.cpp
            src/AtomicFile.cpp
            src/FileChecksum.cpp
            src/MinizBridge.c
            src/Utils.cpp
            src/bms_parser.cpp
            src/path.cpp
            src/rendering/SkinQuadBatchRenderer.cpp
            src/rendering/UniformCache.cpp
            src/scene/play/GameplaySkinSessionFactory.cpp
            src/scene/play/PlayfieldChartVisualModel.cpp
            src/scene/play/PlayfieldProjection.cpp
            src/scene/play/SkinGameplayGraphState.cpp
            src/skin/SkinConfigurationWriteQueue.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/BgfxSkinTextureDevice.cpp
            src/skin/beatoraja/GameplaySkinBuiltinCatalog.cpp
            src/skin/beatoraja/GameplaySkinDocumentLoader.cpp
            src/skin/beatoraja/GameplaySkinSourceFormat.cpp
            src/skin/beatoraja/JsonGameplaySkinDecoder.cpp
            src/skin/beatoraja/Lr2GameplaySkinDecoder.cpp
            src/skin/beatoraja/Lr2SkinCsvParser.cpp
            src/skin/beatoraja/Lr2SkinHeaderDecoder.cpp
            src/skin/beatoraja/GameplaySkinValidator.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/LuaSkinTableDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/PlaySkinSession.cpp
            src/skin/beatoraja/PlaySkinStateBridge.cpp
            src/skin/beatoraja/PlaySkinViewport.cpp
            src/skin/beatoraja/Skin2DRenderer.cpp
            src/skin/beatoraja/SkinBpmGraphRenderer.cpp
            src/skin/beatoraja/SkinGaugeGraphRenderer.cpp
            src/skin/beatoraja/SkinNoteDistributionGraphRenderer.cpp
            src/skin/beatoraja/SkinHitErrorVisualizerRenderer.cpp
            src/skin/beatoraja/SkinTimingVisualizerRenderer.cpp
            src/skin/beatoraja/Skin2DRendererSubmit.cpp
            src/skin/beatoraja/SyntheticReplayGhostOverlay.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinDestinationEvaluator.cpp
            src/skin/beatoraja/SkinDiagnosticHistory.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinPerformanceTelemetry.cpp
            src/skin/beatoraja/SkinMovieCatalog.cpp
            src/skin/beatoraja/SkinMovieCatalogFrame.cpp
            src/skin/beatoraja/SkinResourceCatalog.cpp
            src/skin/beatoraja/PomyuCharaResource.cpp
            src/skin/beatoraja/SkinBitmapFontParser.cpp
            src/skin/beatoraja/SkinTextAtlas.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPackageCatalog.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
            src/view/DecodedImageCache.cpp
            src/view/ImageDecodeCoordinator.cpp
            src/view/ImageFileDecoder.cpp
            src/view/SdlTtfRuntime.cpp
        )
        target_include_directories(gameplay_skin_session_factory_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(gameplay_skin_session_factory_tests PRIVATE
            cxx_std_23
        )
        target_compile_definitions(gameplay_skin_session_factory_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_GAMEPLAY_SKIN_SESSION_FACTORY_TESTING=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(gameplay_skin_session_factory_tests PRIVATE
            ${COMMON_LIBS}
            bgfx
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(gameplay_skin_session_factory_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(gameplay_skin_session_factory_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(gameplay_skin_session_factory_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(gameplay_skin_session_factory_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(skin_configuration_write_queue_tests
            tests/skin_configuration_write_queue_tests.cpp
            src/skin/SkinConfigurationWriteQueue.cpp
        )
        target_include_directories(skin_configuration_write_queue_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(skin_configuration_write_queue_tests PRIVATE cxx_std_23)

        add_executable(play_skin_state_bridge_tests
            tests/play_skin_state_bridge_tests.cpp
            src/FileChecksum.cpp
            src/Utils.cpp
            src/path.cpp
            src/scene/play/PlayfieldProjection.cpp
            src/scene/play/SkinGameplayGraphState.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/GameplaySkinBuiltinCatalog.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/PlaySkinStateBridge.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
        )
        target_include_directories(play_skin_state_bridge_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(play_skin_state_bridge_tests PRIVATE cxx_std_23)
        target_compile_definitions(play_skin_state_bridge_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(play_skin_state_bridge_tests PRIVATE
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(play_skin_state_bridge_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(play_skin_state_bridge_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(play_skin_state_bridge_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(play_skin_state_bridge_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(play_skin_session_tests
            tests/play_skin_session_tests.cpp
            src/ArchiveFile.cpp
            src/MinizBridge.c
            src/FileChecksum.cpp
            src/Utils.cpp
            src/bms_parser.cpp
            src/path.cpp
            src/rendering/SkinQuadBatchRenderer.cpp
            src/rendering/UniformCache.cpp
            src/scene/play/PlayfieldChartVisualModel.cpp
            src/scene/play/PlayfieldProjection.cpp
            src/scene/play/SkinGameplayGraphState.cpp
            src/skin/SkinConfigurationWriteQueue.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/GameplaySkinBuiltinCatalog.cpp
            src/skin/beatoraja/GameplaySkinDocumentLoader.cpp
            src/skin/beatoraja/GameplaySkinSourceFormat.cpp
            src/skin/beatoraja/JsonGameplaySkinDecoder.cpp
            src/skin/beatoraja/Lr2GameplaySkinDecoder.cpp
            src/skin/beatoraja/Lr2SkinCsvParser.cpp
            src/skin/beatoraja/Lr2SkinHeaderDecoder.cpp
            src/skin/beatoraja/GameplaySkinValidator.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/LuaSkinTableDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/PlaySkinSession.cpp
            src/skin/beatoraja/PlaySkinStateBridge.cpp
            src/skin/beatoraja/PlaySkinViewport.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinDestinationEvaluator.cpp
            src/skin/beatoraja/Skin2DRenderer.cpp
            src/skin/beatoraja/SkinBpmGraphRenderer.cpp
            src/skin/beatoraja/SkinGaugeGraphRenderer.cpp
            src/skin/beatoraja/SkinNoteDistributionGraphRenderer.cpp
            src/skin/beatoraja/SkinHitErrorVisualizerRenderer.cpp
            src/skin/beatoraja/SkinTimingVisualizerRenderer.cpp
            src/skin/beatoraja/Skin2DRendererSubmit.cpp
            src/skin/beatoraja/SyntheticReplayGhostOverlay.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinPerformanceTelemetry.cpp
            src/skin/beatoraja/SkinMovieCatalog.cpp
            src/skin/beatoraja/SkinMovieCatalogFrame.cpp
            src/skin/beatoraja/SkinResourceCatalog.cpp
            src/skin/beatoraja/PomyuCharaResource.cpp
            src/skin/beatoraja/SkinBitmapFontParser.cpp
            src/skin/beatoraja/SkinTextAtlas.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinArchiveImporter.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
            src/view/DecodedImageCache.cpp
            src/view/ImageDecodeCoordinator.cpp
            src/view/ImageFileDecoder.cpp
            src/view/SdlTtfRuntime.cpp
        )
        target_include_directories(play_skin_session_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(play_skin_session_tests PRIVATE cxx_std_23)
        target_compile_definitions(play_skin_session_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_PLAY_SKIN_SESSION_TESTING=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(play_skin_session_tests PRIVATE
            ${COMMON_LIBS}
            bgfx
            utf8proc::utf8proc
        )
        if(WIN32)
            target_link_libraries(play_skin_session_tests PRIVATE
                ${LUAJIT_LIB}
                shell32
                ole32
                advapi32
            )
        else()
            target_link_libraries(play_skin_session_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(play_skin_session_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(play_skin_session_tests PRIVATE
                "-framework Foundation"
                iconv
            )
        endif()

        get_target_property(_gameplay_skin_benchmark_sources
            play_skin_session_tests SOURCES)
        list(REMOVE_ITEM _gameplay_skin_benchmark_sources
            tests/play_skin_session_tests.cpp)
        add_executable(gameplay_skin_loading_benchmark_tests
            tests/gameplay_skin_loading_benchmark_tests.cpp
            ${_gameplay_skin_benchmark_sources}
        )
        target_include_directories(gameplay_skin_loading_benchmark_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(gameplay_skin_loading_benchmark_tests PRIVATE
            cxx_std_23
        )
        target_compile_definitions(gameplay_skin_loading_benchmark_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_PLAY_SKIN_SESSION_TESTING=1
            ASOBMASHOW_SKIN_LOADING_TELEMETRY=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        get_target_property(_gameplay_skin_benchmark_links
            play_skin_session_tests LINK_LIBRARIES)
        target_link_libraries(gameplay_skin_loading_benchmark_tests PRIVATE
            ${_gameplay_skin_benchmark_links}
        )

        add_executable(playfield_presentation_coordinator_tests
            tests/playfield_presentation_coordinator_tests.cpp
            src/scene/play/PlayfieldPresentationCoordinator.cpp
        )
        target_include_directories(playfield_presentation_coordinator_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(playfield_presentation_coordinator_tests PRIVATE
            cxx_std_23
        )
        target_compile_definitions(playfield_presentation_coordinator_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_PLAYFIELD_PRESENTATION_COORDINATOR_TESTING=1
        )

        add_executable(replay_playfield_presentation_tests
            tests/replay_playfield_presentation_tests.cpp
            src/ArchiveFile.cpp
            src/AtomicFile.cpp
            src/FileChecksum.cpp
            src/MinizBridge.c
            src/Utils.cpp
            src/audio/GameplayBgaMissStateTracker.cpp
            src/bgfx_helper.cpp
            src/bms_parser.cpp
            src/path.cpp
            src/rendering/Camera.cpp
            src/rendering/Color.cpp
            src/rendering/common.cpp
            src/rendering/RenderPlan.cpp
            src/rendering/SimpleBatchRenderer.cpp
            src/rendering/SkinQuadBatchRenderer.cpp
            src/rendering/TexBatchRenderer.cpp
            src/rendering/UniformCache.cpp
            src/repositories/ChartStorageIdentity.cpp
            src/ScoreProvenance.cpp
            src/scene/play/BMSRenderer.cpp
            src/scene/play/BeatorajaHiSpeedChart.cpp
            src/scene/play/BuiltInPlayfieldPresentation.cpp
            src/scene/play/CompiledGameplayJudge.cpp
            src/scene/play/GameplayCandidateRules.cpp
            src/scene/play/GameplayDefinition.cpp
            src/scene/play/GameplayGaugeRules.cpp
            src/scene/play/GameplayJudgeRules.cpp
            src/scene/play/GameplayNoteJudgeRole.cpp
            src/scene/play/GameplaySimulation.cpp
            src/scene/play/GameplaySkinSessionFactory.cpp
            src/scene/play/Judge.cpp
            src/scene/play/JudgementIndicatorRenderer.cpp
            src/scene/play/PlayfieldChartVisualModel.cpp
            src/scene/play/PlayfieldPresentationCoordinator.cpp
            src/scene/play/PlayfieldProjection.cpp
            src/scene/play/PlayfieldVisualState.cpp
            src/scene/play/ReplayPlayfieldPresentation.cpp
            src/scene/play/ReplayVideoGameplayPreflight.cpp
            src/scene/play/SkinGameplayGraphState.cpp
            src/skin/SkinConfigurationWriteQueue.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/BgfxSkinTextureDevice.cpp
            src/skin/beatoraja/GameplaySkinBuiltinCatalog.cpp
            src/skin/beatoraja/GameplaySkinDocumentLoader.cpp
            src/skin/beatoraja/GameplaySkinSourceFormat.cpp
            src/skin/beatoraja/JsonGameplaySkinDecoder.cpp
            src/skin/beatoraja/Lr2GameplaySkinDecoder.cpp
            src/skin/beatoraja/Lr2SkinCsvParser.cpp
            src/skin/beatoraja/Lr2SkinHeaderDecoder.cpp
            src/skin/beatoraja/GameplaySkinValidator.cpp
            src/skin/beatoraja/LuaSkinBindingDecoder.cpp
            src/skin/beatoraja/LuaSkinFileSystem.cpp
            src/skin/beatoraja/LuaSkinHostModules.cpp
            src/skin/beatoraja/LuaSkinAudioHost.cpp
            src/skin/beatoraja/LuaSkinLegacyInputHost.cpp
            src/skin/beatoraja/LuaSkinHttpClient.cpp
            src/skin/beatoraja/LuaSkinRuntime.cpp
            src/skin/beatoraja/LuaSkinTableDecoder.cpp
            src/skin/beatoraja/NumericGlyphAtlas.cpp
            src/skin/beatoraja/PlaySkinSession.cpp
            src/skin/beatoraja/PlaySkinStateBridge.cpp
            src/skin/beatoraja/PlaySkinViewport.cpp
            src/skin/beatoraja/Skin2DRenderer.cpp
            src/skin/beatoraja/SkinBpmGraphRenderer.cpp
            src/skin/beatoraja/SkinGaugeGraphRenderer.cpp
            src/skin/beatoraja/SkinNoteDistributionGraphRenderer.cpp
            src/skin/beatoraja/SkinHitErrorVisualizerRenderer.cpp
            src/skin/beatoraja/SkinTimingVisualizerRenderer.cpp
            src/skin/beatoraja/Skin2DRendererSubmit.cpp
            src/skin/beatoraja/SyntheticReplayGhostOverlay.cpp
            src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
            src/skin/beatoraja/SkinCoverNormalization.cpp
            src/skin/beatoraja/SkinDestinationEvaluator.cpp
            src/skin/beatoraja/SkinDiagnosticHistory.cpp
            src/skin/beatoraja/SkinGaugeNodeExpansion.cpp
            src/skin/beatoraja/SkinJudgeNormalization.cpp
            src/skin/beatoraja/SkinJudgeNumberNormalization.cpp
            src/skin/beatoraja/SkinModelValidator.cpp
            src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp
            src/skin/beatoraja/SkinNoteLineNormalization.cpp
            src/skin/beatoraja/SkinNoteNormalization.cpp
            src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp
            src/skin/beatoraja/SkinPerformanceTelemetry.cpp
            src/skin/beatoraja/SkinMovieCatalog.cpp
            src/skin/beatoraja/SkinMovieCatalogFrame.cpp
            src/skin/beatoraja/SkinResourceCatalog.cpp
            src/skin/beatoraja/PomyuCharaResource.cpp
            src/skin/beatoraja/SkinBitmapFontParser.cpp
            src/skin/beatoraja/SkinTextAtlas.cpp
            src/skin/beatoraja/SkinTextGraphNormalization.cpp
            src/skin/package/SkinAliasDetector.cpp
            src/skin/package/SkinPackageCatalog.cpp
            src/skin/package/SkinPathPolicy.cpp
            src/skin/package/SkinTreeSnapshotter.cpp
            src/settings/AudioVideoSettings.cpp
            src/utils/SpriteLoader.cpp
            src/view/DecodedImageCache.cpp
            src/view/ImageDecodeCoordinator.cpp
            src/view/ImageFileDecoder.cpp
            src/view/SdlTtfRuntime.cpp
            src/view/TextView.cpp
            src/view/View.cpp
            src/video/RendererAccessCoordinator.cpp
            bgfx/bimg/3rdparty/lodepng/lodepng.cpp
        )
        target_include_directories(replay_playfield_presentation_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(replay_playfield_presentation_tests PRIVATE
            cxx_std_23
        )
        target_compile_definitions(replay_playfield_presentation_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_GAMEPLAY_SKIN_SESSION_FACTORY_TESTING=1
            ASOBMASHOW_REPLAY_PLAYFIELD_PRESENTATION_TESTING=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
            ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )
        target_link_libraries(replay_playfield_presentation_tests PRIVATE
            ${COMMON_LIBS}
            bgfx
            yogacore
            utf8proc::utf8proc
            iconv
        )
        if(WIN32)
            target_link_libraries(replay_playfield_presentation_tests PRIVATE
                ${LUAJIT_LIB}
                advapi32
            )
        else()
            target_link_libraries(replay_playfield_presentation_tests PRIVATE
                ${LUAJIT_LIBRARIES}
            )
        endif()
        if(APPLE)
            target_sources(replay_playfield_presentation_tests PRIVATE
                src/skin/package/SkinAliasDetectorApple.mm
            )
            target_link_libraries(replay_playfield_presentation_tests PRIVATE
                "-framework Foundation"
            )
        endif()

        add_executable(builtin_playfield_presentation_tests
            tests/builtin_playfield_presentation_tests.cpp
        )
        target_include_directories(builtin_playfield_presentation_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(builtin_playfield_presentation_tests PRIVATE
            cxx_std_23
        )
        target_link_libraries(builtin_playfield_presentation_tests PRIVATE
            ${COMMON_LIBS}
            bgfx
        )

        add_executable(gameplay_skin_integration_tests
            tests/gameplay_skin_integration_tests.cpp
        )
        target_include_directories(gameplay_skin_integration_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(gameplay_skin_integration_tests PRIVATE
            cxx_std_23
        )
        add_executable(skin_performance_telemetry_tests
            tests/skin_performance_telemetry_tests.cpp
            src/skin/beatoraja/SkinPerformanceTelemetry.cpp
        )
        target_include_directories(skin_performance_telemetry_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(skin_performance_telemetry_tests PRIVATE
            cxx_std_23
        )
        target_compile_definitions(skin_performance_telemetry_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
        )
        target_link_libraries(skin_performance_telemetry_tests PRIVATE bgfx)

        add_executable(skin_acceptance_recorder_tests
            tests/skin_acceptance_recorder_tests.cpp
            src/BuildIdentity.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/SkinAcceptanceRecorder.cpp
            src/skin/beatoraja/SkinOverlayDigestProvider.cpp
            src/skin/beatoraja/SkinPerformanceTelemetry.cpp
            src/skin/package/SkinPathPolicy.cpp
        )
        target_include_directories(skin_acceptance_recorder_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(skin_acceptance_recorder_tests PRIVATE
            cxx_std_23
        )
        target_compile_definitions(skin_acceptance_recorder_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_BUILD_COMMIT="1111111111111111111111111111111111111111"
            ASOBMASHOW_BUILD_CONFIGURATION="Release"
            ASOBMASHOW_SOURCE_CLEAN=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
        )
        target_link_libraries(skin_acceptance_recorder_tests PRIVATE
            utf8proc::utf8proc
        )

        add_executable(skin_overlay_digest_provider_tests
            tests/skin_overlay_digest_provider_tests.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/skin/beatoraja/SkinOverlayDigestProvider.cpp
            src/skin/package/SkinPathPolicy.cpp
        )
        target_include_directories(skin_overlay_digest_provider_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(skin_overlay_digest_provider_tests PRIVATE
            cxx_std_23
        )
        target_compile_definitions(skin_overlay_digest_provider_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
        )
        target_link_libraries(skin_overlay_digest_provider_tests PRIVATE
            utf8proc::utf8proc
        )

        add_executable(gameplay_skin_acceptance_controller_tests
            tests/gameplay_skin_acceptance_controller_tests.cpp
            src/BuildIdentity.cpp
            src/FileChecksum.cpp
            src/skin/SkinProfileSettings.cpp
            src/skin/SkinStoragePaths.cpp
            src/scene/GameplaySkinAcceptanceController.cpp
            src/skin/beatoraja/SkinAcceptanceRecorder.cpp
            src/skin/beatoraja/SkinOverlayDigestProvider.cpp
            src/skin/beatoraja/SkinPerformanceTelemetry.cpp
            src/skin/package/SkinPathPolicy.cpp
        )
        target_include_directories(gameplay_skin_acceptance_controller_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(gameplay_skin_acceptance_controller_tests PRIVATE
            cxx_std_23
        )
        target_compile_definitions(gameplay_skin_acceptance_controller_tests PRIVATE
            ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
            ASOBMASHOW_BUILD_COMMIT="1111111111111111111111111111111111111111"
            ASOBMASHOW_BUILD_CONFIGURATION="Release"
            ASOBMASHOW_SOURCE_CLEAN=1
            ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
        )
        target_link_libraries(gameplay_skin_acceptance_controller_tests PRIVATE
            utf8proc::utf8proc
        )

    endif()

    add_executable(practice_session_tests
        tests/practice_session_tests.cpp
        src/ScoreProvenance.cpp
        src/bms_parser.cpp
        src/practice/PracticeConfiguration.cpp
        src/practice/PracticeSession.cpp
        src/scene/play/GameplayGaugeRules.cpp
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
        src/replay/ReplaySetup.cpp
        src/replay/ReplaySetupProvenance.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(gameplay_playback_startup_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_playback_startup_tests PRIVATE cxx_std_23)
    if(TARGET SDL2::SDL2)
        target_link_libraries(gameplay_playback_startup_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(gameplay_playback_startup_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(gameplay_playback_startup_tests PRIVATE SDL2)
    endif()

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
        src/scene/play/SkinGameplayGraphState.cpp
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
        src/scene/play/SkinGameplayGraphState.cpp
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
        src/ScoreProvenance.cpp
        src/input/InputBindingResolver.cpp
        src/input/InputDefaults.cpp
        src/input/InputNormalizer.cpp
        src/input/LogicalGameplayInputAdapter.cpp
        src/scene/play/CompiledGameplayJudge.cpp
        src/scene/play/GameplayJudgeRules.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/GameplayNoteJudgeRole.cpp
        src/scene/play/GameplayCandidateRules.cpp
        src/scene/play/GameplayDefinition.cpp
        src/scene/play/GameplaySimulation.cpp
        src/scene/play/SkinGameplayGraphState.cpp
        src/scene/play/Judge.cpp
        src/scene/play/RealtimeGameplayInputBridge.cpp
        src/scene/play/RealtimeGameplayWorker.cpp
        src/replay/ReplayPlayback.cpp
        src/replay/ReplaySetup.cpp
    )
    target_include_directories(realtime_gameplay_worker_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(realtime_gameplay_worker_tests PRIVATE cxx_std_23)
    target_compile_definitions(realtime_gameplay_worker_tests PRIVATE
        BX_CONFIG_DEBUG=1
    )
    if(TARGET SDL2::SDL2)
        target_link_libraries(realtime_gameplay_worker_tests PRIVATE SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        target_link_libraries(realtime_gameplay_worker_tests PRIVATE SDL2::SDL2-static)
    elseif(TARGET SDL2)
        target_link_libraries(realtime_gameplay_worker_tests PRIVATE SDL2)
    endif()
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
        src/scene/play/VirtualControllerLayout.cpp
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
        src/Uuid.cpp
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
        src/rendering/Color.cpp
        src/scene/ResultGaugeHistory.cpp
    )
    target_include_directories(result_gauge_history_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(result_gauge_history_tests PRIVATE cxx_std_23)

    add_executable(result_presentation_model_tests
        tests/result_presentation_model_tests.cpp
        src/bms_parser.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/ScoreProvenance.cpp
        src/practice/PracticeAnalytics.cpp
        src/practice/PracticeConfiguration.cpp
        src/practice/PracticeResultModel.cpp
        src/scene/ResultGaugeHistory.cpp
        src/scene/ResultPresentationModel.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/Judge.cpp
        src/skin/DefaultSkin.cpp
        src/view/Button.cpp
        src/view/SdlTtfRuntime.cpp
        src/view/TextView.cpp
        src/view/View.cpp
    )
    target_include_directories(result_presentation_model_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(result_presentation_model_tests PRIVATE cxx_std_23)
    target_link_libraries(result_presentation_model_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )

    add_executable(remote_result_scene_tests
        tests/remote_result_scene_tests.cpp
        src/bms_parser.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/rendering/Color.cpp
        src/ResultRecordSummary.cpp
        src/replay/ReplayCapabilities.cpp
        src/ScoreProvenance.cpp
        src/practice/PracticeAnalytics.cpp
        src/practice/PracticeConfiguration.cpp
        src/practice/PracticeResultModel.cpp
        src/scene/ResultGaugeHistory.cpp
        src/scene/ResultPresentationModel.cpp
        src/scene/RemoteResultRecallController.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/Judge.cpp
    )
    target_include_directories(remote_result_scene_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_definitions(remote_result_scene_tests PRIVATE
        ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_compile_features(remote_result_scene_tests PRIVATE cxx_std_23)
    target_link_libraries(remote_result_scene_tests PRIVATE ${COMMON_LIBS})

    add_executable(result_image_exporter_partial_tests
        tests/result_image_exporter_partial_tests.cpp
        src/bms_parser.cpp
        src/ir/IrRemoteScoreModels.cpp
        src/rendering/Color.cpp
        src/rendering/common.cpp
        src/rendering/UniformCache.cpp
        src/ScoreProvenance.cpp
        src/practice/PracticeAnalytics.cpp
        src/practice/PracticeConfiguration.cpp
        src/practice/PracticeResultModel.cpp
        src/scene/ResultGaugeHistory.cpp
        src/scene/ResultPresentationModel.cpp
        src/scene/play/GameplayGaugeRules.cpp
        src/scene/play/Judge.cpp
        src/ResultImageExporterPresentation.cpp
        src/skin/DefaultSkin.cpp
        src/view/Button.cpp
        src/view/SdlTtfRuntime.cpp
        src/view/TextView.cpp
        src/view/View.cpp
    )
    target_include_directories(result_image_exporter_partial_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_definitions(result_image_exporter_partial_tests PRIVATE
        ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_compile_features(result_image_exporter_partial_tests PRIVATE cxx_std_23)
    target_link_libraries(result_image_exporter_partial_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )

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
        src/bms_parser.cpp
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
        src/skin/SkinProfileSettings.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ir/IrUploadCandidates.cpp
        src/settings/AudioVideoSettings.cpp
        src/input/GyroscopeTurntable.cpp
        src/input/InputProfile.cpp
        src/input/VirtualControllerConfig.cpp
        src/input/InputDefaults.cpp
        src/input/InputProfileStore.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/ir/IrOutboxModels.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
        src/ScoreProvenance.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayFileStore.cpp
        src/replay/ReplayProfileTransfer.cpp
        src/replay/ReplayProfileInventory.cpp
        src/replay/ReplayReferenceAgreement.cpp
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
    target_link_libraries(player_profile_manager_tests PRIVATE utf8proc::utf8proc)
    if(WIN32)
        target_link_libraries(player_profile_manager_tests PRIVATE shell32 ole32)
    endif()

    add_executable(profile_switch_tests
        tests/profile_switch_tests.cpp
        src/DifficultyTableImporter.cpp
        src/CourseIdentity.cpp
        src/FileChecksum.cpp
        src/ProfileSessionCoordinator.cpp
        src/ProfileSettingsPersistenceCoordinator.cpp
        src/replay/ReplayFileReconciler.cpp
        src/replay/ReplayProfileReconciliation.cpp
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
        src/skin/SkinProfileSettings.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ir/IrUploadCandidates.cpp
        src/settings/AudioVideoSettings.cpp
        src/input/GyroscopeTurntable.cpp
        src/input/InputProfile.cpp
        src/input/VirtualControllerConfig.cpp
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
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/repositories/ReplayRepositoryLegacySummaries.cpp
        src/ir/IrOutboxModels.cpp
        src/ResultPersistenceCoordinator.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
        src/ScoreProvenance.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayFileStore.cpp
        src/replay/ReplayProfileTransfer.cpp
        src/replay/ReplayProfileInventory.cpp
        src/replay/ReplayReferenceAgreement.cpp
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
        src/skin/SkinProfileSettings.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/ir/IrProfileSettings.cpp
        src/ir/IrCredentialBackend.cpp
        src/ir/IrCredentialStore.cpp
        src/ir/PendingIrCredentialCleanup.cpp
        src/ir/IrReceiptModels.cpp
        src/ir/IrSubmission.cpp
        src/ir/IrSubmissionModern.cpp
        src/ir/IrSubmissionSnapshot.cpp
        src/ir/IrUploadCandidates.cpp
        src/settings/AudioVideoSettings.cpp
        src/input/GyroscopeTurntable.cpp
        src/input/InputProfile.cpp
        src/input/VirtualControllerConfig.cpp
        src/input/InputDefaults.cpp
        src/input/InputProfileStore.cpp
        src/repositories/ScoreRepository.cpp
        src/repositories/ScoreRepositoryQueries.cpp
        src/repositories/ScoreRepositorySchema.cpp
        src/repositories/ReplayRepository.cpp
        src/repositories/ReplayRepositoryIrOutbox.cpp
        src/repositories/ReplayRepositoryModernResults.cpp
        src/repositories/ReplayRepositoryRecords.cpp
        src/repositories/ReplayRepositorySchema.cpp
        src/repositories/ReplayRepositoryLegacyMigration.cpp
        src/ir/IrOutboxModels.cpp
        src/ResultPersistenceModel.cpp
        src/ModernResult.cpp
        src/ScoreProvenance.cpp
        src/replay/BeatorajaReplayPath.cpp
        src/replay/ReplayFileLifecycle.cpp
        src/replay/ReplayFileStore.cpp
        src/replay/ReplayProfileTransfer.cpp
        src/replay/ReplayProfileInventory.cpp
        src/replay/ReplayReferenceAgreement.cpp
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

    add_executable(start_select_control_tests
        tests/start_select_control_tests.cpp
    )
    target_include_directories(start_select_control_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(start_select_control_tests PRIVATE cxx_std_23)

    add_executable(beatoraja_hispeed_tests
        tests/beatoraja_hispeed_tests.cpp
    )
    target_include_directories(beatoraja_hispeed_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(beatoraja_hispeed_tests PRIVATE cxx_std_23)

    add_executable(settings_scene_preview_authority_tests
        tests/settings_scene_preview_authority_tests.cpp
        src/settings/AudioVideoSettings.cpp
    )
    target_include_directories(settings_scene_preview_authority_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(settings_scene_preview_authority_tests PRIVATE cxx_std_23)

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

    add_executable(quaternion_math_tests
        tests/quaternion_math_tests.cpp
        src/math/Quaternion.cpp
        src/math/Vector3.cpp
    )
    target_include_directories(quaternion_math_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(quaternion_math_tests PRIVATE cxx_std_23)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(quaternion_math_tests PRIVATE
            -Wuninitialized
            -Werror=uninitialized
        )
    endif()

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

    add_executable(skin_tree_snapshotter_tests
        tests/skin_tree_snapshotter_tests.cpp
        src/FileChecksum.cpp
        src/skin/SkinProfileSettings.cpp
        src/skin/SkinStoragePaths.cpp
        src/skin/package/SkinAliasDetector.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/skin/package/SkinTreeSnapshotter.cpp
    )
    target_include_directories(skin_tree_snapshotter_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(skin_tree_snapshotter_tests PRIVATE cxx_std_23)
    target_compile_definitions(skin_tree_snapshotter_tests PRIVATE
        ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
        ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(skin_tree_snapshotter_tests PRIVATE
        utf8proc::utf8proc
    )
    if(APPLE)
        target_sources(skin_tree_snapshotter_tests PRIVATE
            src/skin/package/SkinAliasDetectorApple.mm
        )
        target_link_libraries(skin_tree_snapshotter_tests PRIVATE
            "-framework Foundation"
        )
    endif()

    add_executable(gameplay_skin_lifecycle_tests
        tests/gameplay_skin_lifecycle_tests.cpp
        src/FileChecksum.cpp
        src/skin/GameplaySkinLifecycle.cpp
        src/skin/SkinProfileSettings.cpp
        src/skin/SkinStoragePaths.cpp
        src/skin/package/SkinAliasDetector.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/skin/package/SkinTreeSnapshotter.cpp
    )
    target_include_directories(gameplay_skin_lifecycle_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_skin_lifecycle_tests PRIVATE cxx_std_23)
    target_compile_definitions(gameplay_skin_lifecycle_tests PRIVATE
        ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1
        ASOBMASHOW_GAMEPLAY_SKIN_LIFECYCLE_TESTING=1
        ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
    )
    target_link_libraries(gameplay_skin_lifecycle_tests PRIVATE
        utf8proc::utf8proc
    )
    if(APPLE)
        target_sources(gameplay_skin_lifecycle_tests PRIVATE
            src/skin/package/SkinAliasDetectorApple.mm
        )
        target_link_libraries(gameplay_skin_lifecycle_tests PRIVATE
            "-framework Foundation"
        )
    endif()

    add_executable(gameplay_skin_settings_tests
        tests/gameplay_skin_settings_tests.cpp
        src/PlatformDocumentHandoff.cpp
        src/AtomicFile.cpp
        src/FileChecksum.cpp
        src/VersionedJson.cpp
        src/scene/GameplaySkinSettingsController.cpp
        src/skin/SkinCommitCoordinator.cpp
        src/skin/SkinProfileSettings.cpp
        src/skin/SkinStoragePaths.cpp
        src/skin/beatoraja/GameplaySkinSourceFormat.cpp
        src/skin/beatoraja/SkinDiagnosticHistory.cpp
        src/skin/package/SkinAliasDetector.cpp
        src/skin/package/SkinArchiveImporter.cpp
        src/skin/package/SkinPackageCatalog.cpp
        src/skin/package/SkinPackageOperationService.cpp
        src/skin/package/SkinPackageStore.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/skin/package/SkinTreeSnapshotter.cpp
    )
    target_include_directories(gameplay_skin_settings_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    set_property(TARGET gameplay_skin_settings_tests PROPERTY CXX_STANDARD 20)
    set_property(TARGET gameplay_skin_settings_tests PROPERTY CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(gameplay_skin_settings_tests PRIVATE
        ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING
        ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
    )
    target_link_libraries(gameplay_skin_settings_tests PRIVATE
        BZip2::BZip2
        LibLZMA::LibLZMA
        TinyFileDialogs
        ZLIB::ZLIB
        utf8proc::utf8proc
    )
    if(TARGET zstd::libzstd_static)
        target_link_libraries(gameplay_skin_settings_tests PRIVATE
            zstd::libzstd_static
        )
    endif()
    if(TARGET LibArchive::LibArchive)
        target_link_libraries(gameplay_skin_settings_tests PRIVATE
            LibArchive::LibArchive
        )
    else()
        target_include_directories(gameplay_skin_settings_tests PRIVATE
            ${LibArchive_INCLUDE_DIRS}
        )
        target_link_libraries(gameplay_skin_settings_tests PRIVATE
            ${LibArchive_LIBRARIES}
        )
    endif()
    if(APPLE)
        target_sources(gameplay_skin_settings_tests PRIVATE
            src/skin/package/SkinAliasDetectorApple.mm
        )
        target_link_libraries(gameplay_skin_settings_tests PRIVATE
            "-framework Foundation"
        )
    endif()
    if(WIN32)
        target_link_libraries(gameplay_skin_settings_tests PRIVATE advapi32)
    endif()

    add_executable(gameplay_skin_settings_presentation_tests
        tests/gameplay_skin_settings_presentation_tests.cpp
        src/scene/GameplaySkinSettingsPresentation.cpp
    )
    target_include_directories(gameplay_skin_settings_presentation_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    set_property(TARGET gameplay_skin_settings_presentation_tests
        PROPERTY CXX_STANDARD 20
    )
    set_property(TARGET gameplay_skin_settings_presentation_tests
        PROPERTY CXX_STANDARD_REQUIRED ON
    )

    add_executable(gameplay_skin_traits_tests
        tests/gameplay_skin_traits_tests.cpp
    )
    target_include_directories(gameplay_skin_traits_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_skin_traits_tests PRIVATE cxx_std_23)

    add_executable(skin_archive_importer_tests
        tests/skin_archive_importer_tests.cpp
        src/FileChecksum.cpp
        src/skin/SkinProfileSettings.cpp
        src/skin/SkinStoragePaths.cpp
        src/skin/beatoraja/GameplaySkinSourceFormat.cpp
        src/skin/package/SkinAliasDetector.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/skin/package/SkinTreeSnapshotter.cpp
        src/skin/package/SkinArchiveImporter.cpp
    )
    target_include_directories(skin_archive_importer_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(skin_archive_importer_tests PRIVATE cxx_std_23)
    target_compile_definitions(skin_archive_importer_tests PRIVATE
        ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
        ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(skin_archive_importer_tests PRIVATE
        utf8proc::utf8proc
    )
    if(TARGET LibArchive::LibArchive)
        target_link_libraries(skin_archive_importer_tests PRIVATE
            LibArchive::LibArchive
        )
    else()
        target_include_directories(skin_archive_importer_tests PRIVATE
            ${LibArchive_INCLUDE_DIRS}
        )
        target_link_libraries(skin_archive_importer_tests PRIVATE
            ${LibArchive_LIBRARIES}
        )
    endif()
    if(APPLE)
        target_sources(skin_archive_importer_tests PRIVATE
            src/skin/package/SkinAliasDetectorApple.mm
        )
        target_link_libraries(skin_archive_importer_tests PRIVATE
            "-framework Foundation"
        )
    endif()
    if(WIN32)
        target_link_libraries(skin_archive_importer_tests PRIVATE advapi32)
    endif()

    add_executable(skin_diagnostic_history_tests
        tests/skin_diagnostic_history_tests.cpp
        src/AtomicFile.cpp
        src/FileChecksum.cpp
        src/skin/beatoraja/SkinDiagnosticHistory.cpp
        src/skin/package/SkinPackageCatalog.cpp
        src/skin/package/SkinPathPolicy.cpp
    )
    target_include_directories(skin_diagnostic_history_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(skin_diagnostic_history_tests PRIVATE cxx_std_23)
    target_link_libraries(skin_diagnostic_history_tests PRIVATE utf8proc::utf8proc)

    # The operation-service target owns queue/worker semantics around the
    # independently usable prepare operations. Store/catalog transactional
    # integration remains in skin_package_store_tests.
    add_executable(skin_package_operation_service_tests
        tests/skin_package_operation_service_tests.cpp
        src/AtomicFile.cpp
        src/FileChecksum.cpp
        src/VersionedJson.cpp
        src/skin/SkinProfileSettings.cpp
        src/skin/SkinStoragePaths.cpp
        src/skin/beatoraja/GameplaySkinSourceFormat.cpp
        src/skin/package/SkinAliasDetector.cpp
        src/skin/package/SkinArchiveImporter.cpp
        src/skin/package/SkinPackageCatalog.cpp
        src/skin/package/SkinPackageOperationService.cpp
        src/skin/package/SkinPackageStore.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/skin/package/SkinTreeSnapshotter.cpp
    )
    target_include_directories(skin_package_operation_service_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(skin_package_operation_service_tests PRIVATE cxx_std_23)
    target_compile_definitions(skin_package_operation_service_tests PRIVATE
        ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
        ASOBMASHOW_SKIN_OPERATION_SERVICE_TESTING
    )
    target_link_libraries(skin_package_operation_service_tests PRIVATE
        BZip2::BZip2
        LibLZMA::LibLZMA
        ZLIB::ZLIB
        utf8proc::utf8proc
    )
    if(TARGET LibArchive::LibArchive)
        target_link_libraries(skin_package_operation_service_tests PRIVATE LibArchive::LibArchive)
    else()
        target_include_directories(skin_package_operation_service_tests PRIVATE ${LibArchive_INCLUDE_DIRS})
        target_link_libraries(skin_package_operation_service_tests PRIVATE ${LibArchive_LIBRARIES})
    endif()
    if(APPLE)
        target_sources(skin_package_operation_service_tests PRIVATE
            src/skin/package/SkinAliasDetectorApple.mm
        )
        target_link_libraries(skin_package_operation_service_tests PRIVATE "-framework Foundation")
    endif()
    add_test(NAME skin_package_operation_service_tests
             COMMAND $<TARGET_FILE:skin_package_operation_service_tests>)
    set_tests_properties(skin_package_operation_service_tests PROPERTIES
        WORKING_DIRECTORY $<TARGET_FILE_DIR:skin_package_operation_service_tests>)
    if(MSVC)
        target_sources(skin_package_operation_service_tests PRIVATE
            tests/support/MsvcTestDiagnostics.cpp
        )
    endif()

    # Transactional skin package store/catalog contract suite.
    find_package(zstd CONFIG QUIET)
    add_executable(skin_package_store_tests
        tests/skin_package_store_tests.cpp
        src/AtomicFile.cpp
        src/FileChecksum.cpp
        src/VersionedJson.cpp
        src/skin/SkinCommitCoordinator.cpp
        src/skin/SkinProfileSettings.cpp
        src/skin/SkinStoragePaths.cpp
        src/skin/beatoraja/GameplaySkinSourceFormat.cpp
        src/skin/package/SkinAliasDetector.cpp
        src/skin/package/SkinArchiveImporter.cpp
        src/skin/package/SkinPackageCatalog.cpp
        src/skin/package/SkinPackageOperationService.cpp
        src/skin/package/SkinPackageStore.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/skin/package/SkinTreeSnapshotter.cpp
    )
    target_include_directories(skin_package_store_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    # Keep this suite on the language level used by the iOS application target
    # so shared package-store sources cannot silently acquire C++23-only APIs.
    set_property(TARGET skin_package_store_tests PROPERTY CXX_STANDARD 20)
    set_property(TARGET skin_package_store_tests PROPERTY CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(skin_package_store_tests PRIVATE
        ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
    )
    target_link_libraries(skin_package_store_tests PRIVATE
        BZip2::BZip2
        LibLZMA::LibLZMA
        ZLIB::ZLIB
        utf8proc::utf8proc
    )
    if(TARGET zstd::libzstd_static)
        target_link_libraries(skin_package_store_tests PRIVATE
            zstd::libzstd_static
        )
    endif()
    if(TARGET LibArchive::LibArchive)
        target_link_libraries(skin_package_store_tests PRIVATE
            LibArchive::LibArchive
        )
    else()
        target_include_directories(skin_package_store_tests PRIVATE
            ${LibArchive_INCLUDE_DIRS}
        )
        target_link_libraries(skin_package_store_tests PRIVATE
            ${LibArchive_LIBRARIES}
        )
    endif()
    if(APPLE)
        target_sources(skin_package_store_tests PRIVATE
            src/skin/package/SkinAliasDetectorApple.mm
        )
        target_link_libraries(skin_package_store_tests PRIVATE
            "-framework Foundation"
        )
    endif()

    add_executable(skin_commit_coordinator_tests
        tests/skin_commit_coordinator_tests.cpp
        tests/support/SkinActivationCommitStoreFake.cpp
        src/AtomicFile.cpp
        src/FileChecksum.cpp
        src/VersionedJson.cpp
        src/skin/SkinCommitCoordinator.cpp
        src/skin/SkinProfileSettings.cpp
        src/skin/SkinStoragePaths.cpp
        src/skin/beatoraja/GameplaySkinSourceFormat.cpp
        src/skin/package/SkinAliasDetector.cpp
        src/skin/package/SkinArchiveImporter.cpp
        src/skin/package/SkinPackageCatalog.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/skin/package/SkinTreeSnapshotter.cpp
    )
    target_include_directories(skin_commit_coordinator_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tests
    )
    target_compile_features(skin_commit_coordinator_tests PRIVATE cxx_std_23)
    target_compile_definitions(skin_commit_coordinator_tests PRIVATE
        ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
    )
    target_link_libraries(skin_commit_coordinator_tests PRIVATE
        BZip2::BZip2
        LibLZMA::LibLZMA
        ZLIB::ZLIB
        utf8proc::utf8proc
    )
    if(TARGET zstd::libzstd_static)
        target_link_libraries(skin_commit_coordinator_tests PRIVATE
            zstd::libzstd_static
        )
    endif()
    if(TARGET LibArchive::LibArchive)
        target_link_libraries(skin_commit_coordinator_tests PRIVATE
            LibArchive::LibArchive
        )
    else()
        target_include_directories(skin_commit_coordinator_tests PRIVATE
            ${LibArchive_INCLUDE_DIRS}
        )
        target_link_libraries(skin_commit_coordinator_tests PRIVATE
            ${LibArchive_LIBRARIES}
        )
    endif()
    if(APPLE)
        target_sources(skin_commit_coordinator_tests PRIVATE
            src/skin/package/SkinAliasDetectorApple.mm
        )
        target_link_libraries(skin_commit_coordinator_tests PRIVATE
            "-framework Foundation"
        )
    endif()
    if(WIN32)
        target_link_libraries(skin_commit_coordinator_tests PRIVATE advapi32)
    endif()
    add_test(NAME skin_commit_coordinator_tests
             COMMAND skin_commit_coordinator_tests)
    if(MSVC)
        target_sources(skin_commit_coordinator_tests PRIVATE
            tests/support/MsvcTestDiagnostics.cpp
        )
    endif()

    add_executable(lua_skin_file_system_tests
        tests/lua_skin_file_system_tests.cpp
        src/FileChecksum.cpp
        src/skin/SkinProfileSettings.cpp
        src/skin/SkinStoragePaths.cpp
        src/skin/beatoraja/LuaSkinFileSystem.cpp
        src/skin/beatoraja/SkinCompatibilityDiagnostics.cpp
        src/skin/package/SkinAliasDetector.cpp
        src/skin/package/SkinPathPolicy.cpp
        src/skin/package/SkinTreeSnapshotter.cpp
    )
    target_include_directories(lua_skin_file_system_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(lua_skin_file_system_tests PRIVATE cxx_std_23)
    target_compile_definitions(lua_skin_file_system_tests PRIVATE
        ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS
        ASOBMASHOW_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    target_link_libraries(lua_skin_file_system_tests PRIVATE
        utf8proc::utf8proc
    )
    if(WIN32)
        target_link_libraries(lua_skin_file_system_tests PRIVATE advapi32)
    endif()
    if(APPLE)
        target_sources(lua_skin_file_system_tests PRIVATE
            src/skin/package/SkinAliasDetectorApple.mm
        )
        target_link_libraries(lua_skin_file_system_tests PRIVATE
            "-framework Foundation"
        )
    endif()

    add_executable(lua_skin_java_pattern_tests
        tests/lua_skin_java_pattern_tests.cpp
    )
    target_include_directories(lua_skin_java_pattern_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(lua_skin_java_pattern_tests PRIVATE cxx_std_23)
    asobmashow_enable_lua_skin_java_pattern(lua_skin_java_pattern_tests)
    find_package(Java QUIET COMPONENTS Runtime Development)
    if(Java_JAVA_EXECUTABLE AND Java_JAVAC_EXECUTABLE)
        add_test(NAME lua_skin_java_pattern_oracle_tests
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/lua_skin_java_pattern_oracle_tests.py
                $<TARGET_FILE:lua_skin_java_pattern_tests>)
    endif()
    if(APPLE AND NOT IOS)
        add_executable(lua_skin_java_pattern_foundation_tests
            tests/lua_skin_java_pattern_tests.cpp
            src/skin/beatoraja/LuaSkinJavaPatternFoundation.mm
        )
        target_include_directories(lua_skin_java_pattern_foundation_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(lua_skin_java_pattern_foundation_tests PRIVATE
            cxx_std_23)
        target_compile_definitions(lua_skin_java_pattern_foundation_tests PRIVATE
            ASOBMASHOW_TEST_FOUNDATION_JAVA_PATTERN=1)
        target_link_libraries(lua_skin_java_pattern_foundation_tests PRIVATE
            "-framework Foundation")
        if(Java_JAVA_EXECUTABLE AND Java_JAVAC_EXECUTABLE)
            add_test(NAME lua_skin_java_pattern_foundation_oracle_tests
                COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/lua_skin_java_pattern_oracle_tests.py
                    $<TARGET_FILE:lua_skin_java_pattern_foundation_tests>)
        endif()

        add_executable(lua_skin_foundation_http_transport_tests
            tests/lua_skin_foundation_http_transport_tests.cpp
            src/skin/beatoraja/LuaSkinFoundationHttpTransport.mm
        )
        target_include_directories(lua_skin_foundation_http_transport_tests PRIVATE
            ${CMAKE_SOURCE_DIR}/src
        )
        target_compile_features(lua_skin_foundation_http_transport_tests PRIVATE
            cxx_std_23)
        target_compile_definitions(lua_skin_foundation_http_transport_tests PRIVATE
            ASOBMASHOW_TEST_FOUNDATION_HTTP_TRANSPORT=1)
        target_link_libraries(lua_skin_foundation_http_transport_tests PRIVATE
            "-framework Foundation")
        add_test(NAME lua_skin_foundation_http_transport_tests
                 COMMAND lua_skin_foundation_http_transport_tests)
    endif()

    foreach(archive_file_target IN ITEMS
        builtin_renderer_characterization_tests
        ir_upload_candidates_tests
        music_playlist_repository_tests
        chart_repository_tests
        archive_file_concurrency_tests
        chart_library_scanner_tests
        difficulty_table_importer_tests
        modern_result_recall_tests
        jukebox_restore_tests
        image_view_fade_tests
        chart_list_item_view_tests
        ir_upload_candidate_list_view_tests
        find_bms_download_tests
        gameplay_bga_target_tests
        play_skin_session_tests
        profile_switch_tests
    )
        if(TARGET ${archive_file_target})
            asobmashow_stage_sevenzip_runtime(${archive_file_target})
        endif()
    endforeach()

    foreach(ui_batch_view_target IN ITEMS
        builtin_renderer_characterization_tests
        ir_ranking_modal_tests
        image_view_fade_tests
        chart_list_item_view_tests
        context_menu_view_tests
        view_layout_tests
        button_enabled_tests
        text_input_box_tests
        checkbox_button_content_tests
        play_options_panel_view_tests
        replay_summary_list_view_tests
        result_record_list_view_tests
        ir_upload_candidate_list_view_tests
        replay_playfield_presentation_tests
        result_presentation_model_tests
        result_image_exporter_partial_tests
    )
        if(TARGET ${ui_batch_view_target})
            target_sources(${ui_batch_view_target} PRIVATE
                src/rendering/UiBatchRenderer.cpp
            )
        endif()
    endforeach()

    function(asobmashow_register_test target_name)
        set(test_name ${target_name})
        set(test_command_arguments)
        if(ARGC GREATER 1)
            set(test_name ${ARGV1})
        endif()
        if(ARGC GREATER 2)
            list(SUBLIST ARGV 2 -1 test_command_arguments)
        endif()

        get_property(test_target_prepared TARGET ${target_name}
            PROPERTY ASOBMASHOW_TEST_TARGET_PREPARED)
        if(NOT test_target_prepared)
            target_compile_definitions(${target_name} PRIVATE SDL_MAIN_HANDLED)
            # Many legacy fixtures use assert() for setup as well as validation.
            # Release's NDEBUG would otherwise remove database/resource setup calls
            # and make the test binaries exercise a different program.
            if(MSVC)
                target_compile_options(${target_name} PRIVATE /UNDEBUG)
                target_sources(${target_name} PRIVATE
                    tests/support/MsvcTestDiagnostics.cpp
                )
            else()
                target_compile_options(${target_name} PRIVATE -UNDEBUG)
            endif()
            set_property(TARGET ${target_name}
                PROPERTY ASOBMASHOW_TEST_TARGET_PREPARED TRUE)
        endif()
        add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${target_name}> ${test_command_arguments})
        set_tests_properties(${test_name} PROPERTIES
            WORKING_DIRECTORY $<TARGET_FILE_DIR:${target_name}>
        )
    endfunction()

    asobmashow_register_test(skin_package_store_tests)

    foreach(eligibility_target IN ITEMS
        chart_replay_context_tests
        chart_replay_persistence_tests
        course_replay_context_tests
        course_replay_persistence_tests
        ir_submission_service_tests
        ir_upload_candidate_list_view_tests
        ir_upload_candidates_tests
        legacy_result_summary_tests
        player_profile_manager_tests
        profile_archive_tests
        profile_switch_tests
        replay_file_action_service_tests
        replay_legacy_migration_tests
        replay_repository_modern_chart_tests
        replay_repository_modern_course_tests
        tachi_batch_manual_tests
        tachi_driver_tests
    )
        target_link_libraries(${eligibility_target} PRIVATE
            ir_submission_eligibility_test_support
        )
    endforeach()

    asobmashow_register_test(skin_path_policy_tests)
    asobmashow_register_test(gameplay_skin_source_format_tests)
    asobmashow_register_test(skin_tree_snapshotter_tests)
    asobmashow_register_test(gameplay_skin_lifecycle_tests)
    asobmashow_register_test(gameplay_skin_settings_tests)
    asobmashow_register_test(gameplay_skin_settings_presentation_tests)
    asobmashow_register_test(gameplay_skin_traits_tests)
    asobmashow_register_test(skin_archive_importer_tests)
    asobmashow_register_test(skin_diagnostic_history_tests)
    asobmashow_register_test(lua_skin_file_system_tests)
    asobmashow_register_test(find_bms_download_tests)
    asobmashow_register_test(video_frame_layout_tests)
    asobmashow_register_test(video_decode_state_tests)

    asobmashow_register_test(audio_video_settings_tests foundation_av_settings)
    asobmashow_register_test(audio_mix_tests foundation_av_audio_mix)
    asobmashow_register_test(audio_wrapper_lifecycle_tests foundation_av_audio_wrapper_lifecycle)
    set_tests_properties(foundation_av_audio_wrapper_lifecycle PROPERTIES TIMEOUT 15)
    asobmashow_register_test(audio_device_manager_tests foundation_av_audio_device)
    asobmashow_register_test(jukebox_restore_tests foundation_av_jukebox_restore)
    set_tests_properties(foundation_av_jukebox_restore PROPERTIES TIMEOUT 30)

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
        replay_capabilities_tests
        replay_limits_tests
        durable_payload_contract_tests
        replay_format_authority_tests
        replay_setup_tests
        beatoraja_replay_path_tests
        replay_codec_primitive_tests
        beatoraja_replay_codec_tests
        replay_file_lifecycle_tests
        replay_file_store_tests
        replay_file_action_service_tests
        replay_file_reconciler_tests
        replay_profile_transfer_tests
        replay_codec_store_contract_tests
        replay_playback_tests
        replay_input_recorder_tests
        course_continuation_tests
        course_replay_capture_tests
        modern_result_tests
        result_contract_tests
        ir_submission_snapshot_tests
        modern_result_recall_tests
        result_record_summary_tests
        course_identity_tests
        ir_driver_tests
        ir_account_lookup_service_tests
        ir_receipt_models_tests
        ir_upload_candidates_tests
        ir_remote_score_models_tests
        ir_score_reconciliation_tests
        ir_score_import_projection_tests
        tachi_user_score_parser_tests
        tachi_batch_manual_tests
        tachi_driver_tests
        bokutachi_cache_store_tests
        tachi_ranking_parser_tests
        ir_submission_service_tests
        ir_ranking_service_tests
        ir_settings_presentation_tests
        ir_result_presentation_tests
        ir_saved_result_upload_tests
        ir_saved_result_batch_upload_tests
        ir_ranking_modal_tests
        replay_legacy_migration_tests
        legacy_result_summary_tests
        replay_repository_modern_chart_tests
        replay_repository_modern_course_tests
        chart_replay_persistence_tests
        replay_file_association_coordinator_tests
        course_replay_persistence_tests
        chart_replay_context_tests
        course_replay_context_tests
        course_replay_consumer_tests
        replay_playback_driver_tests
        best_replay_resolver_tests
        music_playlist_repository_tests
        chart_repository_tests
        chart_scan_work_scheduler_tests
        archive_file_concurrency_tests
        chart_library_scanner_tests
        difficulty_table_importer_tests
        result_persistence_model_tests
        chart_record_filters_tests
        main_menu_library_tests
        chart_meta_index_order_tests
        score_cache_query_tests
        app_database_initializer_tests
        application_startup_tests
        application_result_recovery_tests
        chart_filter_sort_panel_view_tests
        image_fade_tests
        decoded_image_cache_tests
        image_decode_coordinator_tests
        image_file_decoder_tests
        image_view_fade_tests
        chart_list_item_view_tests
        dropdown_view_tests
        context_menu_view_tests
        view_layout_tests
        button_enabled_tests
        text_input_box_tests
        checkbox_button_content_tests
        difficulty_table_url_completion_tests
        replay_summary_list_view_tests
        result_record_list_view_tests
        ir_upload_candidate_list_view_tests
        ir_uploads_controller_tests
    )
        asobmashow_register_test(${test_target})
    endforeach()
    if (TARGET skin_resource_catalog_tests)
        asobmashow_register_test(skin_resource_catalog_tests)
    endif()
    if (TARGET skin_live_resource_counters_tests)
        asobmashow_register_test(skin_live_resource_counters_tests)
    endif()
    if (TARGET skin_process_resident_memory_tests)
        asobmashow_register_test(skin_process_resident_memory_tests)
    endif()
    if (TARGET gameplay_skin_validator_tests)
        asobmashow_register_test(gameplay_skin_validator_tests)
    endif()
    if (TARGET ir_http_client_tests)
        asobmashow_register_test(ir_http_client_tests)
    endif()
    if (TARGET lua_skin_runtime_tests)
        asobmashow_register_test(lua_skin_runtime_tests)
    endif()
    if (TARGET lua_skin_binding_decoder_tests)
        asobmashow_register_test(lua_skin_binding_decoder_tests)
    endif()
    if (TARGET lua_skin_table_decoder_tests)
        asobmashow_register_test(lua_skin_table_decoder_tests)
    endif()
    if (TARGET lua_skin_gauge_live_integration_tests)
        asobmashow_register_test(lua_skin_gauge_live_integration_tests)
    endif()
    if (TARGET lua_skin_text_graph_live_integration_tests)
        asobmashow_register_test(lua_skin_text_graph_live_integration_tests)
    endif()
    if (TARGET lua_skin_binding_live_integration_tests)
        asobmashow_register_test(lua_skin_binding_live_integration_tests)
    endif()
    if (TARGET lua_skin_note_live_integration_tests)
        asobmashow_register_test(lua_skin_note_live_integration_tests)
    endif()
    if (TARGET lua_skin_host_modules_tests)
        asobmashow_register_test(lua_skin_host_modules_tests)
    endif()
    if (TARGET lua_skin_http_transport_tests)
        asobmashow_register_test(lua_skin_http_transport_tests)
    endif()
    if (TARGET beatoraja_skin_model_tests)
        asobmashow_register_test(beatoraja_skin_model_tests)
    endif()
    if (TARGET json_gameplay_skin_decoder_tests)
        asobmashow_register_test(json_gameplay_skin_decoder_tests)
    endif()
    if (TARGET lr2_skin_csv_parser_tests)
        asobmashow_register_test(lr2_skin_csv_parser_tests)
    endif()
    if (TARGET lr2_gameplay_skin_decoder_tests)
        asobmashow_register_test(lr2_gameplay_skin_decoder_tests)
    endif()
    if (TARGET beatoraja_gameplay_cross_format_tests)
        asobmashow_register_test(beatoraja_gameplay_cross_format_tests)
    endif()
    if (TARGET play_skin_viewport_tests)
        asobmashow_register_test(play_skin_viewport_tests)
    endif()
    if (TARGET skin_destination_evaluator_tests)
        asobmashow_register_test(skin_destination_evaluator_tests)
    endif()
    if (TARGET skin_draw_command_tests)
        asobmashow_register_test(skin_draw_command_tests)
    endif()
    if (TARGET play_skin_touch_geometry_tests)
        asobmashow_register_test(play_skin_touch_geometry_tests)
    endif()
    if (TARGET skin_quad_batch_renderer_tests)
        asobmashow_register_test(skin_quad_batch_renderer_tests)
    endif()
    add_executable(ui_batch_renderer_tests
        tests/ui_batch_renderer_tests.cpp
        src/rendering/UiBatchRenderer.cpp
        src/rendering/Color.cpp
        src/rendering/UniformCache.cpp
        src/view/View.cpp
    )
    target_include_directories(ui_batch_renderer_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(ui_batch_renderer_tests PRIVATE cxx_std_23)
    target_link_libraries(ui_batch_renderer_tests PRIVATE
        ${COMMON_LIBS} bgfx yogacore
    )
    asobmashow_register_test(ui_batch_renderer_tests)
    if (TARGET skin_renderer_golden_tests)
        asobmashow_register_test(skin_renderer_golden_tests)
        set_tests_properties(skin_renderer_golden_tests PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESOURCE_LOCK metal_device
        )
    endif()
    if(TARGET builtin_renderer_characterization_tests)
        asobmashow_register_test(builtin_renderer_characterization_tests)
        set_tests_properties(builtin_renderer_characterization_tests PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESOURCE_LOCK metal_device
        )
    endif()
    if (TARGET skin_gauge_node_expansion_tests)
        asobmashow_register_test(skin_gauge_node_expansion_tests)
    endif()
    if (TARGET skin_judge_number_normalization_tests)
        asobmashow_register_test(skin_judge_number_normalization_tests)
    endif()
    if (TARGET skin_judge_normalization_tests)
        asobmashow_register_test(skin_judge_normalization_tests)
    endif()
    if (TARGET skin_note_normalization_tests)
        asobmashow_register_test(skin_note_normalization_tests)
    endif()
    if (TARGET skin_note_lane_geometry_normalization_tests)
        asobmashow_register_test(skin_note_lane_geometry_normalization_tests)
    endif()
    if (TARGET skin_note_line_normalization_tests)
        asobmashow_register_test(skin_note_line_normalization_tests)
    endif()
    if (TARGET skin_text_graph_normalization_tests)
        asobmashow_register_test(skin_text_graph_normalization_tests)
    endif()
    if (TARGET skin_cover_normalization_tests)
        asobmashow_register_test(skin_cover_normalization_tests)
    endif()
    if (TARGET skin_object_resolution_precedence_tests)
        asobmashow_register_test(skin_object_resolution_precedence_tests)
    endif()
    if (TARGET numeric_glyph_atlas_tests)
        asobmashow_register_test(numeric_glyph_atlas_tests)
    endif()
    if (TARGET numeric_glyph_model_validator_tests)
        asobmashow_register_test(numeric_glyph_model_validator_tests)
    endif()
    set_tests_properties(application_result_recovery_tests PROPERTIES
        TIMEOUT 10
    )
    asobmashow_register_test(score_provenance_tests foundation_provenance_contract)
    asobmashow_register_test(gameplay_ruleset_tests)
    asobmashow_register_test(playfield_chart_visual_model_tests)
    asobmashow_register_test(playfield_projection_tests)
    if(TARGET play_skin_state_bridge_tests)
        asobmashow_register_test(play_skin_state_bridge_tests)
    endif()
    if(TARGET gameplay_skin_session_factory_tests)
        asobmashow_register_test(gameplay_skin_session_factory_tests)
    endif()
    if(TARGET play_skin_session_tests)
        asobmashow_register_test(play_skin_session_tests)
    endif()
    if(TARGET gameplay_skin_loading_benchmark_tests)
        asobmashow_register_test(gameplay_skin_loading_benchmark_tests)
    endif()
    if(TARGET playfield_presentation_coordinator_tests)
        asobmashow_register_test(playfield_presentation_coordinator_tests)
    endif()
    if(TARGET replay_playfield_presentation_tests)
        asobmashow_register_test(replay_playfield_presentation_tests)
        set_tests_properties(replay_playfield_presentation_tests PROPERTIES
            RESOURCE_LOCK metal_device
        )
    endif()
    if(TARGET builtin_playfield_presentation_tests)
        asobmashow_register_test(builtin_playfield_presentation_tests)
    endif()
    if(TARGET gameplay_skin_integration_tests)
        asobmashow_register_test(gameplay_skin_integration_tests)
    endif()
    if(TARGET skin_performance_telemetry_tests)
        asobmashow_register_test(skin_performance_telemetry_tests)
    endif()
    if(TARGET skin_acceptance_recorder_tests)
        asobmashow_register_test(skin_acceptance_recorder_tests)
    endif()
    if(TARGET skin_overlay_digest_provider_tests)
        asobmashow_register_test(skin_overlay_digest_provider_tests)
    endif()
    if(TARGET gameplay_skin_acceptance_controller_tests)
        asobmashow_register_test(gameplay_skin_acceptance_controller_tests)
    endif()
    if(TARGET skin_configuration_write_queue_tests)
        asobmashow_register_test(skin_configuration_write_queue_tests)
    endif()
    asobmashow_register_test(play_options_panel_view_tests)
    asobmashow_register_test(gameplay_judge_rules_tests)
    asobmashow_register_test(judgement_indicator_range_tests)
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
    asobmashow_register_test(profile_settings_persistence_tests foundation_profile_settings_persistence)
    asobmashow_register_test(ir_credential_store_tests ir_credential_store_tests)
    asobmashow_register_test(ir_credential_migration_tests ir_credential_migration_tests)
    asobmashow_register_test(pending_ir_credential_cleanup_tests pending_ir_credential_cleanup_tests)
    asobmashow_register_test(practice_configuration_tests)
    asobmashow_register_test(practice_rule_override_tests)
    asobmashow_register_test(gameplay_score_state_tests)
    asobmashow_register_test(playfield_visual_state_tests)
    asobmashow_register_test(gameplay_bga_target_tests)
    asobmashow_register_test(bgfx_skin_texture_device_tests)
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
    asobmashow_register_test(result_presentation_model_tests)
    asobmashow_register_test(remote_result_scene_tests)
    asobmashow_register_test(result_image_exporter_partial_tests)
    asobmashow_register_test(chart_practice_range_tests)
    asobmashow_register_test(practice_preset_store_tests)
    asobmashow_register_test(player_profile_manager_tests foundation_profile_manager_bootstrap --shard bootstrap)
    asobmashow_register_test(player_profile_manager_tests foundation_profile_manager_deletion --shard deletion)
    asobmashow_register_test(player_profile_manager_tests foundation_profile_manager_integrity --shard integrity)
    asobmashow_register_test(player_profile_manager_tests profile_manager_invalid_shard --shard invalid)
    set_tests_properties(profile_manager_invalid_shard PROPERTIES WILL_FAIL TRUE)
    asobmashow_register_test(profile_switch_tests foundation_profile_switch)
    set_tests_properties(foundation_profile_switch PROPERTIES TIMEOUT 30)
    asobmashow_register_test(profile_archive_tests foundation_profile_archive_portable --shard portable)
    asobmashow_register_test(profile_archive_tests foundation_profile_archive_validation --shard validation)
    asobmashow_register_test(profile_archive_tests foundation_profile_archive_transactions --shard transactions)
    asobmashow_register_test(profile_archive_tests foundation_profile_archive_faults --shard faults)
    asobmashow_register_test(profile_archive_tests profile_archive_invalid_shard --shard invalid)
    set_tests_properties(profile_archive_invalid_shard PROPERTIES WILL_FAIL TRUE)
    asobmashow_register_test(profile_settings_controller_tests foundation_profile_controller)
    asobmashow_register_test(profile_runtime_reapply_tests foundation_profile_runtime)
    asobmashow_register_test(input_device_registry_tests foundation_input_registry)
    asobmashow_register_test(logical_gameplay_input_tests foundation_input_gameplay)
    asobmashow_register_test(start_select_control_tests)
    asobmashow_register_test(beatoraja_hispeed_tests)
    asobmashow_register_test(settings_scene_preview_authority_tests)
    asobmashow_register_test(input_timestamp_tests foundation_input_timestamp)
    asobmashow_register_test(windows_realtime_input_mapping_tests foundation_input_windows_realtime_mapping)
    asobmashow_register_test(settings_audio_video_model_tests foundation_av_settings_ui)
    asobmashow_register_test(display_settings_manager_tests foundation_av_display)
    asobmashow_register_test(frame_pacer_tests foundation_av_frame_pacer)
    asobmashow_register_test(quaternion_math_tests foundation_math_quaternion)
    asobmashow_register_test(sdl_display_backend_tests foundation_av_sdl_display)
    asobmashow_register_test(score_provenance_db_tests foundation_provenance_db)
    asobmashow_register_test(midi_input_tests foundation_input_midi)

    get_property(asobmashow_directory_targets DIRECTORY
        PROPERTY BUILDSYSTEM_TARGETS)
    foreach(asobmashow_target IN LISTS asobmashow_directory_targets)
        get_target_property(asobmashow_target_sources
            ${asobmashow_target} SOURCES)
        if(asobmashow_target_sources)
            # PracticeConfiguration owns chart mutations and therefore needs
            # the parser implementation, including in standalone test targets.
            list(FIND asobmashow_target_sources
                "src/practice/PracticeConfiguration.cpp"
                asobmashow_practice_source_index)
            list(FIND asobmashow_target_sources "src/bms_parser.cpp"
                asobmashow_parser_source_index)
            if(NOT asobmashow_practice_source_index EQUAL -1 AND
               asobmashow_parser_source_index EQUAL -1)
                target_sources(${asobmashow_target} PRIVATE
                    src/bms_parser.cpp)
            endif()

            list(FIND asobmashow_target_sources
                "src/skin/beatoraja/LuaSkinFileSystem.cpp"
                asobmashow_lua_skin_file_system_source_index)
            if(NOT asobmashow_lua_skin_file_system_source_index EQUAL -1)
                asobmashow_enable_lua_skin_java_pattern(${asobmashow_target})
            endif()

            # Utils.cpp uses CoreFoundation for exact MS932 decoding on Apple.
            # Keep standalone consumers on the application/iOS link contract.
            if(APPLE)
                list(FIND asobmashow_target_sources "src/Utils.cpp"
                    asobmashow_utils_source_index)
                if(NOT asobmashow_utils_source_index EQUAL -1)
                    target_link_libraries(${asobmashow_target} PRIVATE
                        "-framework CoreFoundation")
                endif()
            endif()
        endif()
    endforeach()
endif()
