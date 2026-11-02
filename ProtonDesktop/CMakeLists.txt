cmake_minimum_required(VERSION 3.20)
project(ProtonDesktopContracts LANGUAGES CXX)
add_library(protondesktop_contract STATIC core/BootRequest.cpp backend/NativeBackend.cpp)
target_include_directories(protondesktop_contract PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(protondesktop_contract PUBLIC cxx_std_17)
include(CTest)
if(BUILD_TESTING)
  # Exercise the shared overflow/conversion policies in the normal test run.
  # Windows additionally builds/runs the actual FromApp service implementations.
  add_subdirectory(backend/uwp)
  # Opt-in profile tests use real pinned headers and checked source overlays.
  # The default portable run still needs no upstream checkout/download.
  if(BOXEDWINE_SOURCE)
    add_subdirectory(tools/uwp-core/tests)
    if(WIN32)
      target_compile_definitions(proton-uwp-profile-tests PRIVATE
        WINAPI_FAMILY=WINAPI_FAMILY_APP _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
  endif()
  add_executable(protondesktop_contract_tests tests/BootRequestTests.cpp)
  target_link_libraries(protondesktop_contract_tests PRIVATE protondesktop_contract)
  add_test(NAME protondesktop_boot_contract COMMAND protondesktop_contract_tests)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME protondesktop_python_gates COMMAND ${Python3_EXECUTABLE}
      -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p test_*.py -v)
  endif()
endif()
