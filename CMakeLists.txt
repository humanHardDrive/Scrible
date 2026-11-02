cmake_minimum_required(VERSION 3.24)
project(XournalScribe VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(XS_BUILD_DESKTOP "Build the SDL3 desktop front end (for development)" ON)
option(XS_BUILD_TESTS "Build the tests and the xopp-render tool" ON)
option(XS_BUILD_KINDLE "Build the Kindle front end (cross-compile; needs FBINK_DIR)" OFF)

if(XS_BUILD_KINDLE)
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

function(xs_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

add_library(xscribe_core STATIC
  src/core/Canvas.cpp
  src/core/Document.cpp
  src/core/Editor.cpp
  src/core/EinkPanel.cpp
  src/core/Png.cpp
  src/core/Renderer.cpp
  src/core/Toolbar.cpp
  src/core/XoppFile.cpp)
target_include_directories(xscribe_core PUBLIC src/core)
target_compile_definitions(xscribe_core PRIVATE XS_VERSION="${PROJECT_VERSION}")
target_link_libraries(xscribe_core PUBLIC ZLIB::ZLIB PRIVATE pugixml::pugixml)
xs_warnings(xscribe_core)

if(XS_BUILD_TESTS)
  add_executable(xopp-render tools/xopp_render.cpp)
  target_link_libraries(xopp-render PRIVATE xscribe_core)
  xs_warnings(xopp-render)

  add_executable(xscribe-tests tests/tests.cpp)
  target_link_libraries(xscribe-tests PRIVATE xscribe_core)
  xs_warnings(xscribe-tests)
  enable_testing()
  add_test(NAME xscribe-tests COMMAND xscribe-tests)
endif()

if(XS_BUILD_DESKTOP)
  find_package(SDL3 CONFIG REQUIRED)
  add_executable(xscribe-desktop src/desktop/main.cpp)
  target_link_libraries(xscribe-desktop PRIVATE xscribe_core SDL3::SDL3)
  xs_warnings(xscribe-desktop)
endif()

if(XS_BUILD_KINDLE)
  set(FBINK_DIR "" CACHE PATH "FBInk source tree containing a built Release/libfbink.a")
  if(NOT EXISTS "${FBINK_DIR}/fbink.h" OR NOT EXISTS "${FBINK_DIR}/Release/libfbink.a")
    message(FATAL_ERROR "Set FBINK_DIR to an FBInk tree built with `make staticlib KINDLE=1` (see scripts/build-kindle.sh)")
  endif()
  add_executable(xscribe
    src/kindle/main.cpp
    src/kindle/EvdevInput.cpp
    src/kindle/FbinkDisplay.cpp
    src/kindle/PowerEvents.cpp)
  target_include_directories(xscribe PRIVATE "${FBINK_DIR}")
  target_link_libraries(xscribe PRIVATE xscribe_core "${FBINK_DIR}/Release/libfbink.a" m)
  xs_warnings(xscribe)
endif()
