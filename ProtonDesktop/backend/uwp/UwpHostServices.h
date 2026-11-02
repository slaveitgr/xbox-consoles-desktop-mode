// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace protondesktop::uwp
{
    constexpr std::uint64_t FileTimeUnixEpoch = 116444736000000000ULL;
    constexpr bool FileTimeToUnixMicroseconds(std::uint64_t ticks, std::uint64_t* output) noexcept
    {
        if (!output || ticks < FileTimeUnixEpoch) return false;
        *output = (ticks - FileTimeUnixEpoch) / 10;
        return true;
    }
    constexpr bool CounterTicksToMicroseconds(std::uint64_t ticks, std::uint64_t frequency,
                                              std::uint64_t* output) noexcept
    {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        constexpr std::uint64_t scale = 1000000;
        if (!output || frequency == 0 || frequency > maximum / scale) return false;
        const auto seconds = ticks / frequency;
        const auto fraction = (ticks % frequency) * scale / frequency;
        if (seconds > (maximum - fraction) / scale) return false;
        *output = seconds * scale + fraction;
        return true;
    }
    constexpr std::int64_t RelativeTimerDueTime(std::uint64_t nanoseconds) noexcept
    {
        // Ceil to a 100ns relative due time, with no ns+99 addition overflow.
        // Even UINT64_MAX nanoseconds fits after scaling into signed 64 bits.
        const auto ticks = nanoseconds / 100 + (nanoseconds % 100 != 0);
        return -static_cast<std::int64_t>(ticks);
    }
    struct DirectoryEntry { std::string utf8Name; bool isDirectory; };
#ifdef _WIN32
    bool ResetMicroCounter() noexcept;
    bool ReadMicroCounter(std::uint64_t* output) noexcept;
    bool ReadUnixMicroseconds(std::uint64_t* output) noexcept;
    bool SleepNanoseconds(std::uint64_t nanoseconds) noexcept;
    bool ReadCpuCount(std::uint32_t* output) noexcept;
    bool Utf8ToWide(const std::string& source, std::wstring* output);
    bool WideToUtf8(const std::wstring& source, std::string* output);
    // Native app-accessible directories only. No broad filesystem capability,
    // path permission bypass, rootfs syscall handling or guest path translation.
    // Results are replaced only after a complete successful enumeration.
    bool EnumerateDirectory(const std::string& utf8Path, std::vector<DirectoryEntry>* output);
#endif
}
