# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.20)
project(ProtonDesktopUwpProfileTests LANGUAGES CXX)
include(CTest)
get_filename_component(PROTONDESKTOP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(BOXEDWINE_SOURCE "" CACHE PATH "Pristine pinned Boxedwine64 checkout")
if(NOT EXISTS "${BOXEDWINE_SOURCE}/include/boxedwine.h")
  message(FATAL_ERROR "Select BOXEDWINE_SOURCE for real upstream declarations")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../prepare_profile_overlay.cmake")
proton_prepare_uwp_profile_overlay("${BOXEDWINE_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/profile-overlay")
add_executable(proton-uwp-profile-tests ProfileTests.cpp
  "${PROTONDESKTOP_ROOT}/backend/uwp/UwpPlatformProfile.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/profile-overlay/platform/sdl/knativeaudio.cpp")
target_compile_features(proton-uwp-profile-tests PRIVATE cxx_std_20)
target_compile_definitions(proton-uwp-profile-tests PRIVATE
  BOXEDWINE_GUEST_X64=1 BOXEDWINE_64=1 PROTONDESKTOP_UWP_PROFILE=1)
target_include_directories(proton-uwp-profile-tests PRIVATE
  "${BOXEDWINE_SOURCE}/include" "${BOXEDWINE_SOURCE}/lib/simde")
if(MSVC)
  target_compile_definitions(proton-uwp-profile-tests PRIVATE BOXEDWINE_MSVC)
  target_compile_options(proton-uwp-profile-tests PRIVATE /EHsc)
else()
  target_compile_options(proton-uwp-profile-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
add_test(NAME proton_uwp_profile_behavior COMMAND proton-uwp-profile-tests)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME proton_uwp_profile_overlay
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_profile_overlay.py"
      --source "${BOXEDWINE_SOURCE}")
endif()
