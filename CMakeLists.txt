cmake_minimum_required(VERSION 3.24)
project(Scrible VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SCRIBLE_BUILD_DESKTOP "Build the SDL3 desktop front end (for development)" ON)
option(SCRIBLE_BUILD_TESTS "Build the tests and the xopp-render tool" ON)
option(SCRIBLE_BUILD_KINDLE "Build the Kindle front end (cross-compile; needs FBINK_DIR)" OFF)

# 64-bit stat()/readdir() on 32-bit Linux (the Kindle): without it they fail with
# EOVERFLOW on filesystems with large inode numbers, and the notebook list comes up empty.
add_compile_definitions(_FILE_OFFSET_BITS=64)

if(SCRIBLE_BUILD_KINDLE)
  # The firmware's libstdc++ is older than the toolchain's, so every binary carries its own.
  add_link_options(-static-libstdc++ -static-libgcc)
  # Silences GCC's notes about an ARM ABI change in GCC 7.1, irrelevant for a static C++ runtime.
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi>)
endif()

include(FetchContent)
FetchContent_Declare(pugixml
  URL https://github.com/zeux/pugixml/releases/download/v1.15/pugixml-1.15.tar.gz
  URL_HASH SHA256=655ade57fa703fb421c2eb9a0113b5064bddb145d415dd1f88c79353d90d511a
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(pugixml)

find_package(ZLIB REQUIRED)

function(scrible_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

add_library(scrible_core STATIC
  src/core/About.cpp
  src/core/App.cpp
  src/core/Canvas.cpp
  src/core/DigitReader.cpp
  src/core/Document.cpp
  src/core/Editor.cpp
  src/core/EinkPanel.cpp
  src/core/Eraser.cpp
  src/core/Lasso.cpp
  src/core/Library.cpp
  src/core/MathPopup.cpp
  src/core/Paper.cpp
  src/core/PaperMenu.cpp
  src/core/Png.cpp
  src/core/Renderer.cpp
  src/core/Settings.cpp
  src/core/Shapes.cpp
  src/core/TextPrompt.cpp
  src/core/Toolbar.cpp
  src/core/ToolPanel.cpp
  src/core/Widgets.cpp
  src/core/XoppFile.cpp)
target_include_directories(scrible_core PUBLIC src/core)
target_compile_definitions(scrible_core PRIVATE SCRIBLE_VERSION="${PROJECT_VERSION}")
# The commit being built, for the About dialog; checked on every build.
set(SCRIBLE_BUILD_INFO "${CMAKE_BINARY_DIR}/generated/BuildInfo.h")
add_custom_target(scrible_build_info
  COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUT=${SCRIBLE_BUILD_INFO}
          -P "${CMAKE_SOURCE_DIR}/cmake/build-info.cmake"
  BYPRODUCTS "${SCRIBLE_BUILD_INFO}")
add_dependencies(scrible_core scrible_build_info)
target_include_directories(scrible_core PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(scrible_core PUBLIC ZLIB::ZLIB PRIVATE pugixml::pugixml)
scrible_warnings(scrible_core)

if(SCRIBLE_BUILD_TESTS)
  add_executable(xopp-render tools/xopp_render.cpp)
  target_link_libraries(xopp-render PRIVATE scrible_core)
  scrible_warnings(xopp-render)

  add_executable(scrible-tests tests/tests.cpp)
  target_link_libraries(scrible-tests PRIVATE scrible_core)
  scrible_warnings(scrible-tests)
  enable_testing()
  add_test(NAME scrible-tests COMMAND scrible-tests)
endif()

if(SCRIBLE_BUILD_DESKTOP)
  find_package(SDL3 CONFIG REQUIRED)
  add_executable(scrible-desktop src/desktop/main.cpp)
  target_link_libraries(scrible-desktop PRIVATE scrible_core SDL3::SDL3)
  scrible_warnings(scrible-desktop)
endif()

if(SCRIBLE_BUILD_KINDLE)
  set(FBINK_DIR "" CACHE PATH "FBInk source tree containing a built Release/libfbink.a")
  if(NOT EXISTS "${FBINK_DIR}/fbink.h" OR NOT EXISTS "${FBINK_DIR}/Release/libfbink.a")
    message(FATAL_ERROR "Set FBINK_DIR to an FBInk tree built with `make staticlib KINDLE=1` (see scripts/build-kindle.sh)")
  endif()
  add_executable(scrible
    src/kindle/main.cpp
    src/kindle/EvdevInput.cpp
    src/kindle/FbinkDisplay.cpp
    src/kindle/PowerEvents.cpp)
  target_include_directories(scrible PRIVATE "${FBINK_DIR}")
  target_link_libraries(scrible PRIVATE scrible_core "${FBINK_DIR}/Release/libfbink.a" m)
  scrible_warnings(scrible)
endif()
