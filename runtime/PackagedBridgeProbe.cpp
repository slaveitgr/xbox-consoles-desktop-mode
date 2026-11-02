#include "PackagedBridgeProbe.h"

#include <algorithm>
#include <sstream>
#include <vector>

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <libloaderapi.h>
#define XCDM_PACKAGED_BRIDGE_WINDOWS 1
#else
#define XCDM_PACKAGED_BRIDGE_WINDOWS 0
#endif

namespace xcdm::runtime {
namespace {

constexpr std::uint32_t OutputCapacity = 1024U * 1024U;
constexpr std::size_t MaximumImage = 64U * 1024U * 1024U;
constexpr std::uint64_t MaximumInstructions = 1000000U;
constexpr const char* BackendReport =
    "Backend: packaged Win32Bridge component\n"
    "DLL: BridgeRuntime\\Win32Bridge.Runtime.dll\n"
    "Direct-runtime fallback: none\n";

Win32Probe32Result Failed(const std::string& message, std::uint32_t error = 0) {
    std::ostringstream report;
    report << BackendReport << "Failure: " << message << '\n';
    if (error) report << "Error/exception code: " << error << " (0x" << std::hex << error << ")\n";
    return {false, "Win32Bridge: " + message, report.str()};
}

#if XCDM_PACKAGED_BRIDGE_WINDOWS && defined(_MSC_VER)

struct Module {
    HMODULE handle = nullptr;
    ~Module() noexcept { if (handle) ::FreeLibrary(handle); }
    Module() noexcept = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
};

using AbiVersion = std::uint32_t(WINAPI*)();
using Run32 = std::uint32_t(WINAPI*)(std::uint32_t, const std::uint8_t*,
    std::uint32_t, std::uint32_t, char*, std::uint32_t);

// SEH functions contain only scalar locals; their callers own the module and
// output buffer, preserving RAII even when a component raises a native fault.
bool InvokeVersion(AbiVersion entry, std::uint32_t* version, std::uint32_t* fault) noexcept {
    __try {
        *version = entry();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = static_cast<std::uint32_t>(GetExceptionCode());
        return false;
    }
}

bool InvokeRun(Run32 entry, std::uint32_t operation, const std::uint8_t* bytes,
               std::uint32_t size, std::uint32_t limit, char* output,
               std::uint32_t capacity, std::uint32_t* status, std::uint32_t* fault) noexcept {
    __try {
        *status = entry(operation, bytes, size, limit, output, capacity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = static_cast<std::uint32_t>(GetExceptionCode());
        return false;
    }
}

Win32Probe32Result Call(std::uint32_t operation, const std::uint8_t* bytes,
                       std::uint32_t size, std::uint32_t limit) {
    Module module;
    module.handle = ::LoadPackagedLibrary(L"BridgeRuntime\\Win32Bridge.Runtime.dll", 0);
    if (!module.handle) {
        const auto error = static_cast<std::uint32_t>(::GetLastError());
        return Failed("LoadPackagedLibrary failed for the packaged component.", error);
    }
    const auto versionEntry = reinterpret_cast<AbiVersion>(::GetProcAddress(module.handle, "XcdmBridgeAbiVersion"));
    if (!versionEntry) {
        const auto error = static_cast<std::uint32_t>(::GetLastError());
        return Failed("Packaged component is missing XcdmBridgeAbiVersion.", error);
    }
    const auto runEntry = reinterpret_cast<Run32>(::GetProcAddress(module.handle, "XcdmBridgeRun32"));
    if (!runEntry) {
        const auto error = static_cast<std::uint32_t>(::GetLastError());
        return Failed("Packaged component is missing XcdmBridgeRun32.", error);
    }
    std::uint32_t version = 0, fault = 0;
    if (!InvokeVersion(versionEntry, &version, &fault))
        return Failed("ABI version export raised a native exception.", fault);
    if (version != 1) return Failed("Packaged component ABI mismatch: expected 1, received " + std::to_string(version) + ".");

    std::vector<char> output(OutputCapacity, '\0');
    std::uint32_t status = 3;
    if (!InvokeRun(runEntry, operation, bytes, size, limit, output.data(),
                   OutputCapacity, &status, &fault))
        return Failed("Execution export raised a native exception.", fault);
    if (status == 2) return Failed("Execution export rejected its arguments or the bounded output capacity.");
    if (status == 3) return Failed("Execution export reported an unexpected exception.");
    if (status > 3) return Failed("Execution export returned an unknown protocol status " + std::to_string(status) + ".");

    const auto end = std::find(output.begin(), output.end(), '\0');
    if (end == output.end()) return Failed("Execution export returned an unterminated report.");
    const auto split = std::find(output.begin(), end, '\n');
    if (split == output.begin() || split == end) return Failed("Execution export returned a malformed summary/report payload.");
    const std::string summary(output.begin(), split);
    const std::string report(split + 1, end);
    return {status == 0, "Win32Bridge: " + summary,
        std::string(BackendReport) + "Component loaded: yes\nABI version: 1\nOperation: " +
        (operation == 1 ? "embedded self-test\n" : "bounded supplied-image startup\n") +
        "Export status: " + std::to_string(status) + "\n\n" + report};
}

#endif

Win32Probe32Result Dispatch(std::uint32_t operation, const std::uint8_t* bytes,
                           std::size_t size, std::uint64_t limit) {
    if (operation == 2 && (!bytes || !size || size > MaximumImage ||
        !limit || limit > MaximumInstructions))
        return Failed("Invalid PE32 input or instruction limit; component was not invoked.");
#if XCDM_PACKAGED_BRIDGE_WINDOWS && defined(_MSC_VER)
    return Call(operation, bytes, static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(limit));
#else
    (void)operation;
    (void)bytes;
    (void)size;
    (void)limit;
    return Failed("Unsupported: packaged component loading requires x64 Windows/UWP with MSVC SEH.");
#endif
}

} // namespace

Win32Probe32Result RunBridgeWin32Probe32() noexcept {
    try { return Dispatch(1, nullptr, 0, 0); }
    catch (...) { return {false, {}, {}}; }
}

Win32Probe32Result TraceBridgeStartup32(const std::uint8_t* bytes, std::size_t size,
                                      std::uint64_t instructionLimit) noexcept {
    try { return Dispatch(2, bytes, size, instructionLimit); }
    catch (...) { return {false, {}, {}}; }
}

} // namespace xcdm::runtime
