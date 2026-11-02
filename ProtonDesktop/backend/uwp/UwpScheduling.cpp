// SPDX-License-Identifier: GPL-2.0-only
#include "UwpScheduling.h"
#ifndef _WIN32
#error UwpScheduling.cpp requires the Windows SDK.
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

static_assert(sizeof(DWORD_PTR) == sizeof(std::uint64_t), "The UWP target must be x64");
namespace protondesktop::uwp
{
    namespace
    {
        bool Fail(DWORD error) noexcept { SetLastError(error); return false; }
    }
    bool SetCurrentThreadPriority(ThreadPriority priority) noexcept
    {
        int nativePriority;
        switch (priority)
        {
        case ThreadPriority::Normal: nativePriority = THREAD_PRIORITY_NORMAL; break;
        case ThreadPriority::AboveNormal: nativePriority = THREAD_PRIORITY_ABOVE_NORMAL; break;
        default: return Fail(ERROR_INVALID_PARAMETER);
        }
        return SetThreadPriority(GetCurrentThread(), nativePriority) != FALSE;
    }

    bool SetCurrentThreadAffinityCount(std::uint32_t count, std::uint64_t* previousMask) noexcept
    {
        if (count > 64) return Fail(ERROR_INVALID_PARAMETER);
        DWORD_PTR allowed = 0, system = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system)) return false;
        if (!allowed || !system || (allowed & ~system)) return Fail(ERROR_NOT_SUPPORTED);
        std::uint64_t selected = 0;
        if (!SelectAffinityMask(allowed, count, &selected)) return Fail(ERROR_INVALID_PARAMETER);
        const auto previous = SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(selected));
        if (!previous) return false;
        if (previousMask) *previousMask = previous;
        return true;
    }
}
