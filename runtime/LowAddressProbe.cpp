#include "LowAddressProbe.h"

#include <cstddef>
#include <cstdint>
#include <sstream>

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <memoryapi.h>
#define XCDM_LOW_ADDRESS_WINDOWS 1
#else
#define XCDM_LOW_ADDRESS_WINDOWS 0
#endif

namespace xcdm::runtime {
namespace {

#if XCDM_LOW_ADDRESS_WINDOWS

struct Reservation {
    void* address = nullptr;

    Reservation() noexcept = default;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

    ~Reservation() noexcept {
        if (address != nullptr) ::VirtualFree(address, 0, MEM_RELEASE);
    }

    bool Free() noexcept {
        if (address == nullptr) return true;
        if (!::VirtualFree(address, 0, MEM_RELEASE)) return false;
        address = nullptr;
        return true;
    }
};

struct RequestedRange {
    std::uint32_t address;
    std::size_t bytes;
    const char* description;
};

constexpr RequestedRange kRanges[] = {
    {0x00400000U, 0x01200000U, "image at 0x00400000 (18 MiB)"},
    {0x10000000U, 0x00010000U, "stack at 0x10000000 (64 KiB)"},
    {0x7ffd0000U, 0x00010000U, "TEB container at 0x7ffd0000 (64 KiB, includes 0x7ffde000)"},
};

// Keep SEH separate from RAII owners: MSVC /EHsc cannot unwind C++ objects
// from a function containing __try. Touch only successfully owned RW pages.
bool CheckReadWrite(void* address, std::size_t bytes, std::uint32_t* fault) noexcept {
#if defined(_MSC_VER)
    __try {
#endif
        auto* data = static_cast<volatile std::uint8_t*>(address);
        data[0] = 0x5a;
        data[bytes - 1] = 0xa5;
        if (data[0] != 0x5a || data[bytes - 1] != 0xa5) {
            *fault = ERROR_WRITE_FAULT;
            return false;
        }
        return true;
#if defined(_MSC_VER)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = static_cast<std::uint32_t>(GetExceptionCode());
        return false;
    }
#endif
}

ProbeResult Failure(const char* operation, const RequestedRange& range,
                    std::uint32_t errorCode) {
    return {ProbeStatus::Failed,
        std::string(operation) + ": " + range.description + ".", errorCode};
}

ProbeResult Probe() {
    Reservation reservations[3];

    // Keep all three mappings alive together: three independent successes
    // followed by immediate frees would not establish the DBT's requirement.
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& range = kRanges[i];
        auto* requested = reinterpret_cast<void*>(static_cast<std::uintptr_t>(range.address));
        reservations[i].address = ::VirtualAllocFromApp(requested, range.bytes,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (reservations[i].address == nullptr) {
            const auto error = static_cast<std::uint32_t>(::GetLastError());
            return Failure("VirtualAllocFromApp(PAGE_READWRITE) failed", range, error);
        }
        if (reservations[i].address != requested) {
            std::ostringstream message;
            message << "Exact reservation failed: " << range.description
                    << "; allocator returned 0x" << std::hex
                    << reinterpret_cast<std::uintptr_t>(reservations[i].address) << ".";
            return {ProbeStatus::Failed, message.str(), ERROR_INVALID_ADDRESS};
        }
    }

    for (std::size_t i = 0; i < 3; ++i) {
        std::uint32_t fault = 0;
        if (!CheckReadWrite(reservations[i].address, kRanges[i].bytes, &fault)) {
            return Failure("First/last-byte read/write check failed", kRanges[i], fault);
        }
    }

    // Attempt every release even if one fails, while preserving the first
    // error. A failed release remains owned for the destructor to retry.
    std::size_t firstFailedRelease = 3;
    std::uint32_t releaseError = 0;
    for (std::size_t i = 3; i != 0; --i) {
        if (!reservations[i - 1].Free()) {
            const auto error = static_cast<std::uint32_t>(::GetLastError());
            if (firstFailedRelease == 3) {
                firstFailedRelease = i - 1;
                releaseError = error;
            }
        }
    }
    if (firstFailedRelease != 3) {
        return Failure("VirtualFree(MEM_RELEASE) failed", kRanges[firstFailedRelease], releaseError);
    }

    return {ProbeStatus::Passed,
        "Simultaneous exact RW reservations passed: image 0x00400000 (18 MiB), "
        "stack 0x10000000 (64 KiB), TEB container 0x7ffd0000 (64 KiB, includes 0x7ffde000). "
        "First/last bytes were read and written; all reservations released. No guest code executed.", 0};
}

#endif // XCDM_LOW_ADDRESS_WINDOWS

} // namespace

ProbeResult RunLowAddressProbe() noexcept {
    try {
#if XCDM_LOW_ADDRESS_WINDOWS
        return Probe();
#else
        return {ProbeStatus::Unsupported,
            "Exact low-address reservation diagnostic requires an x64 Windows/UWP host.", 0};
#endif
    } catch (...) {
        // Preserve the noexcept contract even if formatting a report fails.
        return {ProbeStatus::Failed, {}, 0};
    }
}

} // namespace xcdm::runtime
