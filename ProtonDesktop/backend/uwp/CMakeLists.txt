# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(ProtonDesktopUwpMemory LANGUAGES CXX)
  include(CTest)
endif()
get_filename_component(PROTONDESKTOP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_library(protondesktop_uwp_memory_policy INTERFACE)
target_include_directories(protondesktop_uwp_memory_policy INTERFACE "${PROTONDESKTOP_ROOT}")
target_compile_features(protondesktop_uwp_memory_policy INTERFACE cxx_std_17)
if(WIN32)
  add_library(protondesktop_uwp_memory STATIC UwpMemory.cpp)
  target_link_libraries(protondesktop_uwp_memory PUBLIC protondesktop_uwp_memory_policy WindowsApp)
  target_compile_definitions(protondesktop_uwp_memory PUBLIC
    WINAPI_FAMILY=WINAPI_FAMILY_APP _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
  # Optional compile proof against the actual pinned upstream declarations.
  # This is an OBJECT target: full UWP core linkage needs the other Platform
  # services and must exclude upstream's duplicate desktop memory definitions.
  set(BOXEDWINE_SOURCE "" CACHE PATH "Pristine pinned full Boxedwine64 source")
  if(BOXEDWINE_SOURCE)
    if(NOT EXISTS "${BOXEDWINE_SOURCE}/include/boxedwine.h")
      message(FATAL_ERROR "BOXEDWINE_SOURCE must be a full Boxedwine64 checkout")
    endif()
    find_package(Git REQUIRED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BOXEDWINE_SOURCE}" rev-parse HEAD
      OUTPUT_VARIABLE source_pin OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT source_pin STREQUAL "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4")
      message(FATAL_ERROR "Unexpected Boxedwine64 source revision: ${source_pin}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BOXEDWINE_SOURCE}" diff --quiet HEAD --
      RESULT_VARIABLE source_changed)
    if(NOT source_changed EQUAL 0)
      message(FATAL_ERROR "Compile against pristine pinned Boxedwine64 headers")
    endif()
    add_library(protondesktop_uwp_platform_memory OBJECT UwpPlatformMemory.cpp)
    target_link_libraries(protondesktop_uwp_platform_memory PRIVATE protondesktop_uwp_memory)
    target_compile_features(protondesktop_uwp_platform_memory PRIVATE cxx_std_20)
    target_compile_definitions(protondesktop_uwp_platform_memory PRIVATE
      BOXEDWINE_MSVC BOXEDWINE_64 BOXEDWINE_GUEST_X64 BOXEDWINE_MULTI_THREADED BOXEDWINE_DISABLE_UI SDL2=1)
    target_include_directories(protondesktop_uwp_platform_memory PRIVATE
      "${BOXEDWINE_SOURCE}/include" "${BOXEDWINE_SOURCE}/lib/sdl2/include"
      "${BOXEDWINE_SOURCE}/lib/simde")
  endif()
endif()
if(BUILD_TESTING)
  add_executable(protondesktop_uwp_memory_tests "${PROTONDESKTOP_ROOT}/tests/UwpPlatformMemoryTests.cpp")
  target_link_libraries(protondesktop_uwp_memory_tests PRIVATE protondesktop_uwp_memory_policy)
  if(WIN32)
    target_link_libraries(protondesktop_uwp_memory_tests PRIVATE protondesktop_uwp_memory)
  endif()
  add_test(NAME protondesktop_uwp_memory COMMAND protondesktop_uwp_memory_tests)
endif()
