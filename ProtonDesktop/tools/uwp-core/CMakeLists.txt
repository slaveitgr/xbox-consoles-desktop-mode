# SPDX-License-Identifier: GPL-2.0-only
# Deliberately incomplete until real required Platform services have been ported.
cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
# Toolchain identification must not require a packaged UWP executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
project(ProtonDesktopUwpCore LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore" OR NOT MSVC
   OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Use WindowsStore x64 clang-cl with genuine Windows SDK/Visual Studio libraries")
endif()
get_filename_component(PROTONDESKTOP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(BOXEDWINE_SOURCE "" CACHE PATH "Pristine pinned full Boxedwine64 checkout")
get_filename_component(BOXEDWINE_SOURCE "${BOXEDWINE_SOURCE}" REALPATH)
if(NOT EXISTS "${BOXEDWINE_SOURCE}/include/boxedwine.h")
  message(FATAL_ERROR "Set BOXEDWINE_SOURCE to the pinned full/sparse checkout")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BOXEDWINE_SOURCE}" rev-parse HEAD
  OUTPUT_VARIABLE source_pin OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT source_pin STREQUAL "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4")
  message(FATAL_ERROR "Unexpected Boxedwine64 source revision: ${source_pin}")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BOXEDWINE_SOURCE}" status --porcelain --untracked-files=all
  OUTPUT_VARIABLE source_status OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT source_status STREQUAL "")
  message(FATAL_ERROR "Upstream checkout must remain pristine: ${source_status}")
endif()
add_compile_definitions(WINAPI_FAMILY=WINAPI_FAMILY_APP _WIN32_WINNT=0x0A00 WINVER=0x0A00
  WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
# Do not inherit the desktop platform's broad default Win32 import list.
set(CMAKE_C_STANDARD_LIBRARIES "WindowsApp.lib")
set(CMAKE_CXX_STANDARD_LIBRARIES "WindowsApp.lib")

# Do not call a STATIC_LIBRARY compiler probe proof of arithmetic linkage.
# The final link check below must resolve the genuine compiler-rt helpers.
set(PROTON_CLANG_BUILTINS "" CACHE FILEPATH "Official clang_rt.builtins-x86_64.lib")
if(NOT PROTON_CLANG_BUILTINS)
  get_filename_component(clang_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(clang_root "${clang_bin}" DIRECTORY)
  file(GLOB runtimes LIST_DIRECTORIES FALSE
    "${clang_root}/lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib")
  list(SORT runtimes COMPARE NATURAL ORDER DESCENDING)
  if(runtimes)
    list(GET runtimes 0 PROTON_CLANG_BUILTINS)
  endif()
endif()
if(NOT EXISTS "${PROTON_CLANG_BUILTINS}")
  message(FATAL_ERROR "Select the installed official x64 compiler-rt builtins library")
endif()

# Genuine SDL common, event, atomic, CPU, thread and timer implementation.
# Existing disabled subsystem implementations may return 'unsupported'; no
# substitute graphics, filesystem, audio or kernel behavior is supplied.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
foreach(subsystem AUDIO VIDEO RENDER JOYSTICK HAPTIC POWER FILESYSTEM LOADSO SENSOR)
  set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
endforeach()
set(FORCE_STATIC_VCRT OFF CACHE BOOL "" FORCE)
set(LIBC ON CACHE BOOL "" FORCE)
set(DIRECTX OFF CACHE BOOL "" FORCE)
set(VIDEO_OPENGL OFF CACHE BOOL "" FORCE)
set(VIDEO_OPENGLES OFF CACHE BOOL "" FORCE)
set(VIDEO_VULKAN OFF CACHE BOOL "" FORCE)
include(CheckIncludeFile)
foreach(header stdint.h inttypes.h stdlib.h stddef.h stdarg.h float.h errno.h)
  string(TOUPPER "${header}" header_name)
  string(REPLACE "." "_" header_name "${header_name}")
  check_include_file("${header}" "PROTON_HAVE_${header_name}")
  if(PROTON_HAVE_${header_name})
    set("HAVE_${header_name}" 1)
  endif()
endforeach()
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
include("${PROTONDESKTOP_ROOT}/tools/windows-core/prepare_sdl_overlay.cmake")
set(sdl_overlay "${CMAKE_CURRENT_BINARY_DIR}/upstream-overlay/sdl2")
proton_prepare_sdl_overlay("${BOXEDWINE_SOURCE}/lib/sdl2" "${sdl_overlay}")
add_subdirectory("${sdl_overlay}" "${CMAKE_CURRENT_BINARY_DIR}/sdl2" EXCLUDE_FROM_ALL)
# The legacy SDL CMake appends desktop libraries unconditionally. Do not let
# those hide unresolved APIs in this APP-family diagnostic.
set_property(TARGET SDL2-static PROPERTY LINK_LIBRARIES WindowsApp)
set_property(TARGET SDL2-static PROPERTY INTERFACE_LINK_LIBRARIES WindowsApp)

# Reuse actual tested replacement methods, without duplicate desktop Platform.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${PROTONDESKTOP_ROOT}/backend/uwp" "${CMAKE_CURRENT_BINARY_DIR}/uwp-platform")
set(core_sources)
foreach(directory emulation io kernel sdl util x11 x11wire opengl vulkan)
  file(GLOB_RECURSE batch CONFIGURE_DEPENDS "${BOXEDWINE_SOURCE}/source/${directory}/*.cpp")
  list(APPEND core_sources ${batch})
endforeach()
list(FILTER core_sources EXCLUDE REGEX "/source/util/networkutils\\.cpp$")
file(GLOB platform_sources CONFIGURE_DEPENDS "${BOXEDWINE_SOURCE}/platform/sdl/*.cpp")
# Keep the real function but rename its unused desktop main, allowing our
# explicit diagnostic entry without global test-mode changes to kernel code.
set_source_files_properties("${BOXEDWINE_SOURCE}/platform/sdl/knativesystem.cpp"
  PROPERTIES COMPILE_DEFINITIONS "main=proton_unused_desktop_entry")
file(GLOB softfloat_sources CONFIGURE_DEPENDS
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/*.c"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/8086-SSE/*.c")
add_library(proton-uwp-core-objects OBJECT ${core_sources} ${platform_sources} ${softfloat_sources})
target_compile_features(proton-uwp-core-objects PRIVATE cxx_std_20)
target_compile_definitions(proton-uwp-core-objects PRIVATE
  BOXEDWINE_GUEST_X64=1 BOXEDWINE_64=1 BOXEDWINE_MSVC BOXEDWINE_MULTI_THREADED
  BOXEDWINE_HAS_SETJMP BOXEDWINE_DISABLE_UI SDL2=1 SDL_MAIN_HANDLED SIMDE_SSE2_NO_NATIVE
  USING_GENERATED_CONFIG_H)
target_include_directories(proton-uwp-core-objects PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/sdl2/include" "${sdl_overlay}/include"
  "${BOXEDWINE_SOURCE}/include" "${BOXEDWINE_SOURCE}/lib/simde"
  "${BOXEDWINE_SOURCE}/lib/glew/include" "${BOXEDWINE_SOURCE}/lib/pugixml/src"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/include"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source"
  "${BOXEDWINE_SOURCE}/lib/softfloat/source/8086-SSE")
target_compile_options(proton-uwp-core-objects PRIVATE /O1 /Gy /Gw /W3
  "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
  "$<$<COMPILE_LANGUAGE:CXX>:/FI${PROTONDESKTOP_ROOT}/tools/windows-core/int128_compat.h>"
  "/FI${CMAKE_CURRENT_BINARY_DIR}/sdl2/include/SDL_config.h")
add_executable(proton-uwp-core-linkcheck LinkProbe.cpp
  $<TARGET_OBJECTS:proton-uwp-core-objects>
  $<TARGET_OBJECTS:protondesktop_uwp_platform_memory>
  $<TARGET_OBJECTS:protondesktop_uwp_platform_host>)
target_link_options(proton-uwp-core-linkcheck PRIVATE /APPCONTAINER /OPT:REF /OPT:ICF /SUBSYSTEM:CONSOLE)
target_link_libraries(proton-uwp-core-linkcheck PRIVATE SDL2-static
  protondesktop_uwp_memory protondesktop_uwp_host_services "${PROTON_CLANG_BUILTINS}" WindowsApp)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/qualification-scope.txt" CONTENT
  "source=${source_pin}\nkind=WindowsStore APP compile/link diagnostic only\npackaged=no\nrun=no\nWine=no\nSDL presentation/audio/filesystem/controller subsystems=disabled\nMissing required platform services must fail link\n")
