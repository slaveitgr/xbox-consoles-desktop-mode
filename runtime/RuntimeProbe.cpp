#include "RuntimeProbe.h"

#include "PeImage.h"
#include "../samples/Return42Fixture.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <memoryapi.h>
#define XCDM_NATIVE_X64_WINDOWS 1
#else
#define XCDM_NATIVE_X64_WINDOWS 0
#endif

namespace xcdm::runtime {
namespace {

ProbeResult Result(ProbeStatus status, const char* message, std::uint32_t errorCode = 0) {
    return {status, message, errorCode};
}

ProbeResult UnexpectedFailure() noexcept {
    // An empty string keeps even an allocation failure inside the public
    // noexcept contract. No execution failure is converted into a pass.
    return {ProbeStatus::Failed, {}, 0};
}

#if XCDM_NATIVE_X64_WINDOWS

constexpr std::uint8_t kReturn42Code[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};

struct Allocation {
    void* address = nullptr;
    ~Allocation() noexcept {
        if (address != nullptr) ::VirtualFree(address, 0, MEM_RELEASE);
    }
    bool Free() noexcept {
        if (address == nullptr) return true;
        if (!::VirtualFree(address, 0, MEM_RELEASE)) return false;
        address = nullptr;
        return true;
    }
};

// Keep SEH in a function with only scalar locals. MSVC rejects __try inside a
// function that requires C++ object unwinding (/EHsc); callers own RAII state.
bool InvokeCode(void* entry, std::uint32_t* value, std::uint32_t* fault) noexcept {
#if defined(_MSC_VER)
    __try {
        *value = reinterpret_cast<std::uint32_t (*)()>(entry)();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = static_cast<std::uint32_t>(GetExceptionCode());
        return false;
    }
#else
    // These probes require MSVC's hardware-fault boundary on Windows.
    (void)entry;
    (void)value;
    (void)fault;
    return false;
#endif
}

ProbeResult FinishInvocation(Allocation& memory, void* entry, const char* success) {
    std::uint32_t value = 0;
    std::uint32_t fault = 0;
    if (!InvokeCode(entry, &value, &fault)) {
        return Result(ProbeStatus::Failed, "Diagnostic code raised a hardware exception.", fault);
    }
    if (value != 42) return Result(ProbeStatus::Failed, "Diagnostic returned a value other than 42.");
    if (!memory.Free()) return Result(ProbeStatus::Failed, "VirtualFree failed.", ::GetLastError());
    return Result(ProbeStatus::Passed, success);
}

std::size_t PageSpan(std::uint32_t size, std::size_t pageSize) noexcept {
    return (static_cast<std::size_t>(size) + pageSize - 1) & ~(pageSize - 1);
}

ProbeResult ExecutableMemoryProbe() {
#if !defined(_MSC_VER)
    return Result(ProbeStatus::Unsupported, "Windows diagnostics require MSVC hardware-exception handling.");
#else
    Allocation memory;
    memory.address = ::VirtualAllocFromApp(nullptr, sizeof(kReturn42Code),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (memory.address == nullptr) {
        return Result(ProbeStatus::Failed, "VirtualAllocFromApp(PAGE_READWRITE) failed.", ::GetLastError());
    }
    std::memcpy(memory.address, kReturn42Code, sizeof(kReturn42Code));
    ULONG previous = 0;
    if (!::VirtualProtectFromApp(memory.address, sizeof(kReturn42Code), PAGE_EXECUTE_READ, &previous)) {
        return Result(ProbeStatus::Failed, "VirtualProtectFromApp(PAGE_EXECUTE_READ) failed; verify codeGeneration capability.", ::GetLastError());
    }
    if (!::FlushInstructionCache(::GetCurrentProcess(), memory.address, sizeof(kReturn42Code))) {
        return Result(ProbeStatus::Failed, "FlushInstructionCache failed.", ::GetLastError());
    }
    return FinishInvocation(memory, memory.address,
        "Writable memory changed to executable/read-only memory; x64 diagnostic returned 42.");
#endif
}

ProbeResult MinimalPeProbe() {
#if !defined(_MSC_VER)
    return Result(ProbeStatus::Unsupported, "Windows diagnostics require MSVC hardware-exception handling.");
#else
    const auto& fixture = samples::Return42PeBytes();
    const auto parsed = ParseMinimalPe(fixture.data(), fixture.size());
    if (!parsed.ok) return {ProbeStatus::Failed, parsed.message, 0};
    const auto& image = parsed.image;
    SYSTEM_INFO systemInfo{};
    ::GetSystemInfo(&systemInfo);
    // The parser deliberately gives each diagnostic section its own 4 KiB
    // pages. Reject a different host layout instead of sharing protections.
    if (systemInfo.dwPageSize != 4096) {
        return Result(ProbeStatus::Unsupported, "The diagnostic requires 4096-byte memory pages.");
    }
    const std::size_t pageSize = systemInfo.dwPageSize;
    Allocation memory;
    memory.address = ::VirtualAllocFromApp(nullptr, image.imageSize,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (memory.address == nullptr) {
        return Result(ProbeStatus::Failed, "Allocating the minimal PE image failed.", ::GetLastError());
    }
    auto* base = static_cast<std::uint8_t*>(memory.address);
    std::memcpy(base, fixture.data(), image.headerSize);
    for (const auto& section : image.sections) {
        if (section.rawSize != 0) {
            std::memcpy(base + section.virtualAddress, fixture.data() + section.rawOffset, section.rawSize);
        }
    }
    ULONG previous = 0;
    // Leave gaps inaccessible; expose only the read-only headers and each
    // validated section with its own permissions. No allocation is ever RWX.
    if (!::VirtualProtectFromApp(base, image.imageSize, PAGE_NOACCESS, &previous)) {
        return Result(ProbeStatus::Failed, "Protecting unmapped PE gaps failed.", ::GetLastError());
    }
    if (!::VirtualProtectFromApp(base, PageSpan(image.headerSize, pageSize), PAGE_READONLY, &previous)) {
        return Result(ProbeStatus::Failed, "Protecting the PE headers failed.", ::GetLastError());
    }
    for (const auto& section : image.sections) {
        const auto span = PageSpan(std::max(section.virtualSize, section.rawSize), pageSize);
        const ULONG protection = (section.characteristics & 0x20000000U) != 0
            ? PAGE_EXECUTE_READ
            : ((section.characteristics & 0x80000000U) != 0 ? PAGE_READWRITE : PAGE_READONLY);
        if (!::VirtualProtectFromApp(base + section.virtualAddress, span, protection, &previous)) {
            return Result(ProbeStatus::Failed, "Applying PE section memory protection failed.", ::GetLastError());
        }
    }
    if (!::FlushInstructionCache(::GetCurrentProcess(), base, image.imageSize)) {
        return Result(ProbeStatus::Failed, "Flushing the PE instruction cache failed.", ::GetLastError());
    }
    return FinishInvocation(memory, base + image.entryPointRva,
        "Embedded no-import PE32+ x64 diagnostic loaded with section protections and returned 42.");
#endif
}

#endif // XCDM_NATIVE_X64_WINDOWS

} // namespace

ProbeResult RunExecutableMemoryProbe() noexcept {
    try {
#if XCDM_NATIVE_X64_WINDOWS
        return ExecutableMemoryProbe();
#else
        return Result(ProbeStatus::Unsupported, "Executable-memory diagnostic requires an x64 Windows/UWP host.");
#endif
    } catch (...) { return UnexpectedFailure(); }
}

ProbeResult RunMinimalPeProbe() noexcept {
    try {
#if XCDM_NATIVE_X64_WINDOWS
        return MinimalPeProbe();
#else
        return Result(ProbeStatus::Unsupported, "Native PE execution requires an x64 Windows/UWP host.");
#endif
    } catch (...) { return UnexpectedFailure(); }
}

} // namespace xcdm::runtime
