cmake_minimum_required(VERSION 3.20)
project(XboxDesktopModeResearch VERSION 0.3.0 LANGUAGES CXX)

include(CTest)
option(XCDM_SANITIZERS "Enable address/undefined sanitizers for portable host tests" OFF)

add_library(xcdm_runtime STATIC
    runtime/PeImage.cpp
    runtime/GamePreflight.cpp
    runtime/RuntimeProbe.cpp
    samples/Return42Fixture.cpp)
target_include_directories(xcdm_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(xcdm_runtime PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(xcdm_runtime PRIVATE /W4 /permissive-)
    target_compile_definitions(xcdm_runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(xcdm_runtime PUBLIC windowsapp)
else()
    target_compile_options(xcdm_runtime PRIVATE -Wall -Wextra -Wpedantic)
    if(XCDM_SANITIZERS)
        target_compile_options(xcdm_runtime PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(xcdm_runtime PUBLIC -fsanitize=address,undefined)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(pe_image_tests tests/PeImageTests.cpp)
    target_link_libraries(pe_image_tests PRIVATE xcdm_runtime)
    add_test(NAME pe_image_contract COMMAND pe_image_tests)
    add_executable(runtime_probe_tests tests/RuntimeProbeTests.cpp)
    target_link_libraries(runtime_probe_tests PRIVATE xcdm_runtime)
    add_test(NAME runtime_probe_contract COMMAND runtime_probe_tests)
    add_executable(game_preflight_tests tests/GamePreflightTests.cpp)
    target_link_libraries(game_preflight_tests PRIVATE xcdm_runtime)
    add_test(NAME game_preflight_contract COMMAND game_preflight_tests)
endif()
