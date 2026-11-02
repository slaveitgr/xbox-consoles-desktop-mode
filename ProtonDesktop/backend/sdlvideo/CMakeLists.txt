# SPDX-License-Identifier: GPL-2.0-only
# Explicit opt-in standalone real-SDL pipeline qualification; no Wine launcher.
cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
option(PROTON_SDLVIDEO_APP_HEADERS
  "Windows desktop diagnostic using genuine APP SDK headers and Store CRT; not AppContainer" OFF)
if((CMAKE_HOST_WIN32 OR CMAKE_SYSTEM_NAME MATCHES "^Windows") AND
   (CMAKE_SYSTEM_NAME STREQUAL "WindowsStore" OR PROTON_SDLVIDEO_APP_HEADERS))
  # project() performs the compiler executable-link smoke test before the
  # target configuration below. The Store CRT already selected in LIB needs
  # RoInitialize through WindowsApp even for that tiny main(). Seed actual
  # cached standard libraries before platform defaults are initialized.
  set(CMAKE_C_STANDARD_LIBRARIES "WindowsApp.lib" CACHE STRING "APP SDK standard libraries" FORCE)
  set(CMAKE_CXX_STANDARD_LIBRARIES "WindowsApp.lib" CACHE STRING "APP SDK standard libraries" FORCE)
  list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    CMAKE_C_STANDARD_LIBRARIES CMAKE_CXX_STANDARD_LIBRARIES)
  if(POLICY CMP0137)
    cmake_policy(SET CMP0137 NEW)
  endif()
  # The internal compiler-smoke project initializes its own platform defaults.
  # Linker flags are forwarded even there; retain the actual APP umbrella as
  # an explicit input instead of relying only on its standard-library cache.
  if(NOT CMAKE_EXE_LINKER_FLAGS MATCHES "(^|[ ;])WindowsApp\\.lib($|[ ;])")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} WindowsApp.lib"
      CACHE STRING "Executable linker flags including the required APP umbrella" FORCE)
  endif()
  # Compiler/feature probes must use the selected release APP CRT, not the
  # debug Store CRT chosen by the default MSVC try_compile configuration.
  set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
endif()
project(ProtonDesktopSdlVideo LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(CTest)
set(BOXEDWINE_SOURCE "" CACHE PATH "Pristine pinned full Boxedwine64 checkout")
get_filename_component(BOXEDWINE_SOURCE "${BOXEDWINE_SOURCE}" REALPATH)
if(NOT EXISTS "${BOXEDWINE_SOURCE}/lib/sdl2/src/video/SDL_video.c")
  message(FATAL_ERROR "Select the pinned BOXEDWINE_SOURCE checkout")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BOXEDWINE_SOURCE}" rev-parse HEAD
  OUTPUT_VARIABLE pin OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT pin STREQUAL "58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4")
  message(FATAL_ERROR "Unexpected Boxedwine64 revision: ${pin}")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${BOXEDWINE_SOURCE}" status --porcelain --untracked-files=all
  OUTPUT_VARIABLE changed OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT changed STREQUAL "")
  message(FATAL_ERROR "The pinned source checkout must stay pristine")
endif()
set(proton_app_partition OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore" OR PROTON_SDLVIDEO_APP_HEADERS)
  if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
      NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The APP-header pipeline requires Windows x64 clang-cl and a genuine Windows SDK")
  endif()
  set(proton_app_partition ON)
  set(PROTON_UWP_CRT_DIR "" CACHE PATH "VC x64 Store CRT directory")
  if(NOT EXISTS "${PROTON_UWP_CRT_DIR}/msvcrt.lib" OR NOT EXISTS "${PROTON_UWP_CRT_DIR}/msvcprt.lib")
    message(FATAL_ERROR "Select the genuine x64 Store CRT in PROTON_UWP_CRT_DIR")
  endif()
  link_directories(BEFORE "${PROTON_UWP_CRT_DIR}")
  add_compile_definitions(PROTON_SDLVIDEO_APP_HEADERS=1
    WINAPI_FAMILY=WINAPI_FAMILY_APP SDL_BUILDING_WINRT=1
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
  set(CMAKE_C_STANDARD_LIBRARIES "WindowsApp.lib")
  set(CMAKE_CXX_STANDARD_LIBRARIES "WindowsApp.lib")
endif()
option(PROTON_SDLVIDEO_SANITIZE "Instrument the pure pipeline tests (GCC/Clang hosts)" OFF)
if(PROTON_SDLVIDEO_SANITIZE)
  if(MSVC)
    message(FATAL_ERROR "Select a GCC/Clang Unix host for this sanitizer option")
  endif()
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
foreach(subsystem VIDEO RENDER EVENTS THREADS TIMERS ATOMIC CPUINFO)
  set(SDL_${subsystem} ON CACHE BOOL "" FORCE)
endforeach()
foreach(subsystem AUDIO JOYSTICK HAPTIC POWER FILESYSTEM LOADSO SENSOR DLOPEN)
  set(SDL_${subsystem} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(option VIDEO_DUMMY VIDEO_OFFSCREEN VIDEO_X11 VIDEO_WAYLAND VIDEO_RPI
    VIDEO_KMSDRM VIDEO_DIRECTFB VIDEO_VIVANTE VIDEO_COCOA VIDEO_OPENGL VIDEO_OPENGLES
    VIDEO_VULKAN DIRECTX RENDER_D3D RENDER_METAL SDL_DLOPEN FORCE_STATIC_VCRT)
  set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(LIBC ON CACHE BOOL "" FORCE)
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
include("${CMAKE_CURRENT_SOURCE_DIR}/prepare_overlay.cmake")
set(overlay "${CMAKE_CURRENT_BINARY_DIR}/sdl-overlay")
proton_prepare_sdlvideo_overlay("${BOXEDWINE_SOURCE}/lib/sdl2" "${overlay}")
add_subdirectory("${overlay}" "${CMAKE_CURRENT_BINARY_DIR}/sdl-build" EXCLUDE_FROM_ALL)
target_sources(SDL2-static PRIVATE HostBridge.cpp SoftwareRenderer.cpp ConfigurationProof.cpp)
target_compile_features(SDL2-static PRIVATE cxx_std_17)
target_include_directories(SDL2-static PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/sdl-build/include>")
target_compile_definitions(SDL2-static PUBLIC USING_GENERATED_CONFIG_H SDL_MAIN_HANDLED)
target_compile_definitions(SDL2-static PRIVATE PROTON_SDLVIDEO_STATIC=1)
if(MSVC)
  target_compile_options(SDL2-static PUBLIC
    "/FI${CMAKE_CURRENT_BINARY_DIR}/sdl-build/include/SDL_config.h")
  target_compile_options(SDL2-static PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
else()
  target_compile_options(SDL2-static PUBLIC
    -include "${CMAKE_CURRENT_BINARY_DIR}/sdl-build/include/SDL_config.h")
endif()
if(proton_app_partition)
  set_property(TARGET SDL2-static PROPERTY LINK_LIBRARIES WindowsApp)
  set_property(TARGET SDL2-static PROPERTY INTERFACE_LINK_LIBRARIES WindowsApp)
endif()
add_library(protondesktop_sdlvideo INTERFACE)
target_link_libraries(protondesktop_sdlvideo INTERFACE SDL2-static)
if(BUILD_TESTING)
  add_executable(proton-sdlvideo-tests tests/SoftwarePipelineTests.cpp)
  target_compile_features(proton-sdlvideo-tests PRIVATE cxx_std_17)
  target_link_libraries(proton-sdlvideo-tests PRIVATE protondesktop_sdlvideo)
  # A WindowsStore link is source/import evidence. Do not execute its console
  # diagnostic as if it were an activated packaged CoreApplication.
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    add_test(NAME proton_sdlvideo_real_software_pipeline COMMAND proton-sdlvideo-tests)
  endif()
  if(PROTON_SDLVIDEO_SANITIZE AND NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    set_tests_properties(proton_sdlvideo_real_software_pipeline PROPERTIES
      ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=detect_leaks=1")
  endif()
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/qualification-scope.txt" CONTENT
  "source=${pin}\nkind=real SDL software texture/framebuffer pipeline\nhost=${CMAKE_SYSTEM_NAME}\nAPP SDK headers and Store CRT=${proton_app_partition}\nConfigurationProof.cpp APP partition guard enabled=${proton_app_partition}\nStore CRT=${PROTON_UWP_CRT_DIR}\nWindowsStore execution=not registered; packaged activation required\nnormal Windows runtime=desktop diagnostic only; APP-header mode requires actual _APP CRT DLLs\nWine=no\nAppContainer=no execution qualification\nXbox=no\ninput/audio/controllers=unimplemented\n")
