#include "Win32Probe32.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#define XCDM_BRIDGE_EXPORT __declspec(dllexport)
#else
// Keeps the same protocol testable on a portable host. Packaged loading is
// implemented only by the Windows/UWP caller, not by this test convention.
#define XCDM_BRIDGE_EXPORT __attribute__((visibility("default")))
#define WINAPI
#endif

namespace {
constexpr std::uint32_t MaximumOutput = 1024U * 1024U;
constexpr std::uint32_t MaximumImage = 64U * 1024U * 1024U;
constexpr std::uint32_t MaximumInstructions = 1000000U;
}

// These C exports are separate from the existing public WinRT class ABI.
extern "C" XCDM_BRIDGE_EXPORT std::uint32_t WINAPI XcdmBridgeAbiVersion() noexcept {
    return 1;
}

extern "C" XCDM_BRIDGE_EXPORT std::uint32_t WINAPI XcdmBridgeRun32(
    std::uint32_t operation, const std::uint8_t* image, std::uint32_t imageSize,
    std::uint32_t instructionLimit, char* output, std::uint32_t outputCapacity) noexcept {
    // Return codes: 0=guest/test passed, 1=guest stopped, 2=bad arguments or
    // insufficient output capacity, 3=unexpected C++ exception.
    if (!output || outputCapacity < 2 || outputCapacity > MaximumOutput) return 2;
    output[0] = '\0';
    if ((operation != 1 && operation != 2) ||
        (operation == 1 && (image != nullptr || imageSize != 0)) ||
        (operation == 2 && (!image || !imageSize || imageSize > MaximumImage ||
            !instructionLimit || instructionLimit > MaximumInstructions))) return 2;
    try {
        const auto result = operation == 1
            ? xcdm::runtime::RunWin32Probe32()
            : xcdm::runtime::TraceWin32Startup32(image, imageSize, instructionLimit);
        // Current runtime messages, import names and debug strings are bounded
        // ASCII, which is valid UTF-8. Copy the two strings without a second
        // potentially large concatenated allocation, and never truncate them.
        const std::size_t available = static_cast<std::size_t>(outputCapacity) - 2;
        if (result.summary.size() > available ||
            result.report.size() > available - result.summary.size()) return 2;
        std::memcpy(output, result.summary.data(), result.summary.size());
        output[result.summary.size()] = '\n';
        std::memcpy(output + result.summary.size() + 1, result.report.data(), result.report.size());
        output[result.summary.size() + 1 + result.report.size()] = '\0';
        return result.passed ? 0U : 1U;
    } catch (...) {
        // No allocation or partial successful report on the exception path.
        output[0] = '\0';
        return 3;
    }
}
