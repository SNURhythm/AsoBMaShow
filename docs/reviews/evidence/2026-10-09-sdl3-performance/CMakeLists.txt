cmake_minimum_required(VERSION 3.25)
project(SDLComparison LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
add_subdirectory("${REPO}/SDL" SDL)
add_subdirectory("${REPO}/SDL_ttf" SDL_ttf)
# Keep the baseline font dependency independent of SDL3_ttf's vendored
# Freetype::Freetype alias in this build graph.
execute_process(COMMAND ${CMAKE_COMMAND} -E env
  "PKG_CONFIG_LIBDIR=${REPO}/cmake-build-release/vcpkg_installed/arm64-osx/lib/pkgconfig:${REPO}/cmake-build-release/vcpkg_installed/arm64-osx/share/pkgconfig"
  "PKG_CONFIG_PATH=" pkg-config --static --libs SDL2_ttf
  OUTPUT_VARIABLE baseline_links OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
separate_arguments(baseline_links NATIVE_COMMAND "${baseline_links}")
if(NOT PROBE_SOURCE)
  set(PROBE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/probe.cpp")
endif()
foreach(version IN ITEMS 2 3)
  if(version EQUAL 2)
    set(tree "${BASELINE}")
    set(sdl_libs ${baseline_links})
  else()
    set(tree "${CANDIDATE}")
    set(sdl_libs SDL3::SDL3 SDL3_ttf::SDL3_ttf)
  endif()
  add_executable(probe${version} "${PROBE_SOURCE}"
    "${tree}/src/input/SDLTouchInputSource.cpp"
    "${tree}/src/i18n/Localization.cpp"
    "${tree}/src/rendering/Color.cpp"
    "${tree}/src/rendering/common.cpp"
    "${tree}/src/rendering/UniformCache.cpp"
    "${tree}/src/rendering/UiBatchRenderer.cpp"
    "${tree}/src/view/SdlTtfRuntime.cpp"
    "${tree}/src/view/TextView.cpp"
    "${tree}/src/view/View.cpp")
  if(PROBE_DROPDOWNS)
    target_sources(probe${version} PRIVATE
      "${tree}/src/view/DropdownView.cpp"
      "${tree}/src/view/Button.cpp"
      "${tree}/src/view/ScrollView.cpp")
  endif()
  if(version EQUAL 2)
    target_include_directories(probe${version} PRIVATE
      "${REPO}/cmake-build-release/vcpkg_installed/arm64-osx/include"
      "${REPO}/cmake-build-release/vcpkg_installed/arm64-osx/include/SDL2")
  endif()
  target_compile_definitions(probe${version} PRIVATE BENCH_SDL=${version} BX_CONFIG_DEBUG=0)
  target_include_directories(probe${version} PRIVATE "${tree}/src" "${REPO}/include"
    "${REPO}/bgfx/bgfx/include" "${REPO}/bgfx/bx/include" "${REPO}/bgfx/bimg/include"
    "${REPO}/yoga" "${REPO}/yoga/lib")
  target_link_libraries(probe${version} PRIVATE ${sdl_libs}
    "${REPO}/cmake-build-release/bgfx/cmake/bgfx/libbgfx.a"
    "${REPO}/cmake-build-release/bgfx/cmake/bimg/libbimg.a"
    "${REPO}/cmake-build-release/bgfx/cmake/bx/libbx.a"
    "${REPO}/cmake-build-release/yoga/yoga/libyogacore.a"
    "-framework Cocoa" "-framework QuartzCore" "-framework Metal" "-framework IOKit"
    "-framework OpenGL" "-framework CoreFoundation")
endforeach()
