# SPDX-License-Identifier: GPL-2.0-only
# Linux diagnostic only. This is not the UWP backend or a Wine rootfs build.
cmake_minimum_required(VERSION 3.20)
project(ProtonDesktopHeadlessCore LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "This diagnostic currently targets Linux only")
endif()
set(BOXEDWINE_SOURCE "" CACHE PATH "Pristine pinned Boxedwine64 checkout")
if(NOT EXISTS "${BOXEDWINE_SOURCE}/include/boxedwine.h")
  message(FATAL_ERROR "Set BOXEDWINE_SOURCE to the full pinned source checkout")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BOXEDWINE_SOURCE}" rev-parse HEAD
  OUTPUT_VARIABLE source_pin OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT source_pin STREQUAL "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4")
  message(FATAL_ERROR "Unexpected source revision: ${source_pin}")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BOXEDWINE_SOURCE}" diff --quiet HEAD --
  RESULT_VARIABLE source_changed)
if(NOT source_changed EQUAL 0)
  message(FATAL_ERROR "Upstream tracked source must be pristine")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BOXEDWINE_SOURCE}" status --porcelain --untracked-files=all
  OUTPUT_VARIABLE source_status OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT source_status STREQUAL "")
  message(FATAL_ERROR "Unexpected files in pinned source checkout: ${source_status}")
endif()
find_library(SDL2_RUNTIME NAMES SDL2 SDL2-2.0 libSDL2-2.0.so.0 REQUIRED)
find_package(Threads REQUIRED)
# GCC cannot bind a packed ELF field to the logging template's reference.
# Patch a disposable file, leaving the pinned upstream checkout pristine.
find_program(PATCH_PROGRAM patch REQUIRED)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/overlay")
configure_file("${BOXEDWINE_SOURCE}/source/kernel/loader/loader64.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/overlay/loader64.cpp" COPYONLY)
execute_process(COMMAND "${PATCH_PROGRAM}" --batch --forward -p0
  -i "${CMAKE_CURRENT_SOURCE_DIR}/gcc-packed-log.patch"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/overlay"
  COMMAND_ERROR_IS_FATAL ANY)
set(core_sources)
foreach(directory emulation io kernel sdl util x11 x11wire opengl vulkan)
  file(GLOB_RECURSE batch CONFIGURE_DEPENDS "${BOXEDWINE_SOURCE}/source/${directory}/*.cpp")
  list(APPEND core_sources ${batch})
endforeach()
# Downloads and the desktop container UI are outside this diagnostic.
list(FILTER core_sources EXCLUDE REGEX "/source/util/networkutils\\.cpp$")
list(FILTER core_sources EXCLUDE REGEX "/source/kernel/loader/loader64\\.cpp$")
list(APPEND core_sources "${CMAKE_CURRENT_BINARY_DIR}/overlay/loader64.cpp")
file(GLOB platform_sources CONFIGURE_DEPENDS
  "${BOXEDWINE_SOURCE}/platform/linux/*.cpp"
  "${BOXEDWINE_SOURCE}/platform/sdl/*.cpp")
file(GLOB softfloat_sources CONFIGURE_DEPENDS
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/*.c"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/8086-SSE/*.c")
add_executable(boxedwine64-headless ${core_sources} ${platform_sources} ${softfloat_sources})
target_compile_features(boxedwine64-headless PRIVATE cxx_std_20)
target_compile_definitions(boxedwine64-headless PRIVATE
  BOXEDWINE_GUEST_X64=1 BOXEDWINE_MULTI_THREADED BOXEDWINE_HAS_SETJMP
  BOXEDWINE_DISABLE_UI BOXEDWINE_POSIX SDL2=1 SIMDE_SSE2_NO_NATIVE)
target_include_directories(boxedwine64-headless PRIVATE
  "${BOXEDWINE_SOURCE}/include" "${BOXEDWINE_SOURCE}/lib/sdl2/include"
  "${BOXEDWINE_SOURCE}/lib/simde" "${BOXEDWINE_SOURCE}/lib/glew/include"
  "${BOXEDWINE_SOURCE}/lib/pugixml/src"
  "${BOXEDWINE_SOURCE}/source/kernel/loader"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/include"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/8086-SSE")
target_compile_options(boxedwine64-headless PRIVATE -O1 -ffunction-sections -fdata-sections
  -Wno-invalid-offsetof -Wno-unknown-pragmas)
target_link_options(boxedwine64-headless PRIVATE -Wl,--gc-sections)
target_link_libraries(boxedwine64-headless PRIVATE "${SDL2_RUNTIME}" Threads::Threads m)
