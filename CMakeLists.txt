cmake_minimum_required(VERSION 3.25)

include(ExternalProject)
project(AsoBMaShow VERSION 1.0 LANGUAGES C CXX)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0")
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

add_library(TinyFileDialogs STATIC ${SRC_DIR}/tinyfiledialogs.c)
add_library(SQLite3 STATIC ${SRC_DIR}/sqlite3.c)

if (PLATFORM_MACOS AND BUILD_MACOS_BUNDLE)
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
if (PLATFORM_MACOS AND BUILD_MACOS_BUNDLE)
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

if (PLATFORM_MACOS)
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
add_subdirectory(yoga)
