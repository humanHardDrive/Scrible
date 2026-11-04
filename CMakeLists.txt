cmake_minimum_required(VERSION 3.24)
project(Scrible VERSION 1.1.0 LANGUAGES C CXX)
# What comes after the number for a build that isn't a release yet ("dev"); empty for
# a release. The app reports "1.1.0-dev", and the Kindle packages are named that way.
set(SCRIBLE_PRERELEASE "dev")
set(SCRIBLE_VERSION_STRING "${PROJECT_VERSION}")
if(SCRIBLE_PRERELEASE)
  string(APPEND SCRIBLE_VERSION_STRING "-${SCRIBLE_PRERELEASE}")
endif()

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
# Typed text on the page: DejaVu Sans (Xournal++'s "Sans" on most Linux systems), drawn
# with stb_truetype. The font is built into the app.
FetchContent_Declare(dejavu
  URL https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2
  URL_HASH SHA256=fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(stb
  URL https://raw.githubusercontent.com/nothings/stb/6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760/stb_truetype.h
  URL_HASH SHA256=ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab
  DOWNLOAD_NO_EXTRACT TRUE)
FetchContent_MakeAvailable(dejavu stb)
set(SCRIBLE_FONT_DATA "${CMAKE_BINARY_DIR}/generated/SansFont.cpp")
add_custom_command(OUTPUT "${SCRIBLE_FONT_DATA}"
  COMMAND "${CMAKE_COMMAND}" -DIN=${dejavu_SOURCE_DIR}/ttf/DejaVuSans.ttf -DOUT=${SCRIBLE_FONT_DATA} -DNAME=kSansFont
          -P "${CMAKE_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS "${dejavu_SOURCE_DIR}/ttf/DejaVuSans.ttf" "${CMAKE_SOURCE_DIR}/cmake/embed.cmake")

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
  src/core/Font.cpp
  src/core/Keyboard.cpp
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
  src/core/XoppFile.cpp
  src/core/StbTruetype.cpp
  "${SCRIBLE_FONT_DATA}")
# stb's own code isn't held to our warnings.
set_source_files_properties(src/core/StbTruetype.cpp PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(scrible_core PUBLIC src/core)
target_include_directories(scrible_core SYSTEM PRIVATE "${stb_SOURCE_DIR}")
target_compile_definitions(scrible_core PRIVATE SCRIBLE_VERSION="${SCRIBLE_VERSION_STRING}")
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
