# CMake toolchain file for Kindles on firmware 5.16.3 or newer (hard-float ABI),
# using koxtoolchain's "kindlehf" toolchain (arm-kindlehf-linux-gnueabihf, GCC 14).
# Put the toolchain's bin/ on PATH, or set XS_KINDLE_TC to its root directory.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(XS_KINDLE_TRIPLE arm-kindlehf-linux-gnueabihf)
if(DEFINED ENV{XS_KINDLE_TC})
  set(_xs_tc_prefix "$ENV{XS_KINDLE_TC}/bin/")
endif()
set(CMAKE_C_COMPILER "${_xs_tc_prefix}${XS_KINDLE_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${_xs_tc_prefix}${XS_KINDLE_TRIPLE}-g++")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
