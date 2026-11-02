cmake_minimum_required(VERSION 3.20)
project(XboxDesktopModeResearch VERSION 0.10.0 LANGUAGES C CXX)

include(CTest)
option(XCDM_SANITIZERS "Enable address/undefined sanitizers for portable host tests" OFF)

add_library(xcdm_x86emu STATIC
    third_party/libx86emu/api.c
    third_party/libx86emu/decode.c
    third_party/libx86emu/mem.c
    third_party/libx86emu/ops.c
    third_party/libx86emu/ops2.c
    third_party/libx86emu/prim_ops.c)
target_include_directories(xcdm_x86emu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libx86emu/include")

add_library(xcdm_runtime STATIC
    runtime/ModuleOwnership.cpp
    runtime/PeImage.cpp
    runtime/GamePreflight.cpp
    runtime/GuestEnvironment.cpp
    runtime/GuestExecution.cpp
    runtime/Pe32Image.cpp
    runtime/LowAddressProbe.cpp
    runtime/PackagedBridgeProbe.cpp
    runtime/Win32Probe32.cpp
    runtime/X86Execution.cpp
    runtime/RuntimeProbe.cpp
    samples/X86Win32Fixture.cpp
    samples/Return42Fixture.cpp)
target_link_libraries(xcdm_runtime PRIVATE xcdm_x86emu)
target_include_directories(xcdm_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(xcdm_runtime PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(xcdm_runtime PRIVATE /W4 /permissive-)
    target_compile_definitions(xcdm_runtime PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(xcdm_runtime PUBLIC windowsapp)
else()
    target_compile_options(xcdm_runtime PRIVATE -Wall -Wextra -Wpedantic)
    if(XCDM_SANITIZERS)
        target_compile_options(xcdm_x86emu PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_compile_options(xcdm_runtime PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(xcdm_runtime PUBLIC -fsanitize=address,undefined)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(lan_transfer_tests tests/lan-transfer-tests.cpp)
    target_compile_features(lan_transfer_tests PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(lan_transfer_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(lan_transfer_tests PRIVATE -Wall -Wextra -Wpedantic)
        if(XCDM_SANITIZERS)
            target_compile_options(lan_transfer_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(lan_transfer_tests PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
    add_test(NAME mihawk_lan_input_contract COMMAND lan_transfer_tests)
    add_executable(module_ownership_tests tests/ModuleOwnershipTests.cpp)
    target_link_libraries(module_ownership_tests PRIVATE xcdm_runtime)
    add_test(NAME wine_module_ownership_contract COMMAND module_ownership_tests)
    add_executable(pe_image_tests tests/PeImageTests.cpp)
    target_link_libraries(pe_image_tests PRIVATE xcdm_runtime)
    add_test(NAME pe_image_contract COMMAND pe_image_tests)
    add_executable(runtime_probe_tests tests/RuntimeProbeTests.cpp)
    target_link_libraries(runtime_probe_tests PRIVATE xcdm_runtime)
    add_test(NAME runtime_probe_contract COMMAND runtime_probe_tests)
    add_executable(game_preflight_tests tests/GamePreflightTests.cpp)
    target_link_libraries(game_preflight_tests PRIVATE xcdm_runtime)
    add_test(NAME game_preflight_contract COMMAND game_preflight_tests)
    add_executable(pe32_guest_tests tests/Pe32GuestTests.cpp)
    target_link_libraries(pe32_guest_tests PRIVATE xcdm_runtime)
    add_test(NAME pe32_guest_execution_contract COMMAND pe32_guest_tests)
    add_executable(x86_execution_tests tests/X86ExecutionTests.cpp)
    target_link_libraries(x86_execution_tests PRIVATE xcdm_runtime)
    add_test(NAME x86_integer_execution_contract COMMAND x86_execution_tests)
    add_executable(win32_startup_tests tests/Win32StartupTests.cpp)
    target_link_libraries(win32_startup_tests PRIVATE xcdm_runtime)
    add_test(NAME win32_bounded_startup_contract COMMAND win32_startup_tests)
    add_executable(low_address_probe_tests tests/LowAddressProbeTests.cpp)
    target_link_libraries(low_address_probe_tests PRIVATE xcdm_runtime)
    add_test(NAME low_address_probe_contract COMMAND low_address_probe_tests)
    add_executable(guest_environment_tests tests/GuestEnvironmentTests.cpp)
    target_link_libraries(guest_environment_tests PRIVATE xcdm_runtime)
    if(NOT MSVC)
        find_package(Threads REQUIRED)
        target_link_libraries(guest_environment_tests PRIVATE Threads::Threads)
    endif()
    add_test(NAME wine_guest_environment_contract COMMAND guest_environment_tests)
    add_executable(guest_execution_tests tests/GuestExecutionTests.cpp)
    target_link_libraries(guest_execution_tests PRIVATE xcdm_runtime)
    if(NOT MSVC)
        target_link_libraries(guest_execution_tests PRIVATE Threads::Threads)
    endif()
    add_test(NAME wine_guest_execution_contract COMMAND guest_execution_tests)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME wine_registry_seed_contract
                 COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/wine_registry_seed_test.py")
        add_test(NAME wine_guest_audit_fixtures
                 COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/audit_wine_guest_test.py")
        set_tests_properties(wine_guest_audit_fixtures PROPERTIES SKIP_RETURN_CODE 77)
        # Optional: check the modeled TEB/PEB against a pinned Wine checkout.
        set(XCDM_WINE_SOURCE "" CACHE PATH "Pinned Wine source tree for guest layout checks")
        if(XCDM_WINE_SOURCE)
            add_test(NAME wine_guest_layout_matches_wine
                     COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_guest_layout.py"
                             --winternl "${XCDM_WINE_SOURCE}/include/winternl.h"
                             --layout-exe $<TARGET_FILE:guest_environment_tests>)
        endif()
    endif()
endif()
