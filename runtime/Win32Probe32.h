#pragma once

#include <string>
#include <cstddef>
#include <cstdint>

namespace xcdm::runtime {

struct Win32Probe32Result {
    bool passed = false;
    std::string summary;
    std::string report;
};

// Runs only the embedded, self-authored PE32 fixture through a bounded x86
// interpreter and four explicit Win32 host calls. Never executes a USB image.
Win32Probe32Result RunWin32Probe32() noexcept;

// Bounded integer-only guest runner with SetLastError, GetLastError,
// OutputDebugStringA, guest-only ExitProcess and bounded ANSI GetVersionExA
// (virtual Windows 10.0.19045 profile). Other imports fail closed.
// Exposed for malformed-image/host-call regression tests. The UWP app does not
// pass USB executables here. A successful exit is not game compatibility.
Win32Probe32Result ExecuteWin32IntegerImage(const std::uint8_t* bytes,
    std::size_t size, std::uint64_t instructionLimit = 100000) noexcept;

// Starts a supplied PE32 in the integer interpreter with a virtual TEB/PEB.
// Unknown imports receive stop gates, never fake success. Bounded startup
// tracing does not implement gameplay, FPU/SSE, graphics or general Windows.
// passed means only that guest ExitProcess(0) was reached.
Win32Probe32Result TraceWin32Startup32(const std::uint8_t* bytes,
    std::size_t size, std::uint64_t instructionLimit = 50000) noexcept;

} // namespace xcdm::runtime
