// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>

namespace protondesktop::uwp
{
    enum class ThreadPriority { Normal, AboveNormal };

    // Restrict selection to CPUs the process can actually use in the current
    // processor group. count=0 resets to all allowed CPUs; otherwise select the
    // lowest count eligible bits. Failed selection leaves output unchanged.
    constexpr bool SelectAffinityMask(std::uint64_t allowedMask, std::uint32_t count,
                                      std::uint64_t* output) noexcept
    {
        if (!output || !allowedMask || count > 64) return false;
        if (!count) { *output = allowedMask; return true; }
        std::uint64_t selected = 0;
        for (unsigned bit = 0; bit < 64; ++bit)
        {
            const auto mask = std::uint64_t{1} << bit;
            if (!(allowedMask & mask)) continue;
            selected |= mask;
            if (!--count) { *output = selected; return true; }
        }
        return false;
    }

#ifdef _WIN32
    // Actual UWP-supported scheduling APIs, affecting only the calling host
    // thread. On failure GetLastError explains why; no success is synthesized.
    bool SetCurrentThreadPriority(ThreadPriority priority) noexcept;
    // On success previousMask receives the real mask returned by Windows.
    // Zero/unsupported multi-group process masks fail closed; this API does
    // not select a different processor group or pin another guest thread.
    bool SetCurrentThreadAffinityCount(std::uint32_t count,
                                       std::uint64_t* previousMask = nullptr) noexcept;
#endif
}
