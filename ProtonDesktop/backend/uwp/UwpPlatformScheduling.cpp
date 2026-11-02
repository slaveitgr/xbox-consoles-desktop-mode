// SPDX-License-Identifier: GPL-2.0-only
#include "boxedwine.h"
#include "UwpScheduling.h"
#include <Windows.h>
#include <stdexcept>
#include <system_error>

#if defined(BOXEDWINE_JIT) || defined(BOXEDWINE_JIT_X64) || defined(BOXEDWINE_JIT_ARMV8)
#error The ProtonDesktop UWP scheduling adapter supports software interpretation only.
#endif
static_assert(sizeof(void*) == 8, "The UWP interpreter target must be x64");
namespace
{
    [[noreturn]] void SchedulingFailure(const char* operation)
    {
        const auto error = GetLastError();
        throw std::system_error(static_cast<int>(error), std::system_category(), operation);
    }
}
void Platform::setCurrentThreadPriorityHigh()
{
    // A bounded boost for the calling Wine server thread, never realtime.
    if (!protondesktop::uwp::SetCurrentThreadPriority(protondesktop::uwp::ThreadPriority::AboveNormal))
        SchedulingFailure("UWP SetThreadPriority");
}
#ifdef BOXEDWINE_MULTI_THREADED
void Platform::setCpuAffinityForThread(KThread* thread, U32 count)
{
    // The UWP source overlay must invoke this from the worker itself. Upstream
    // stores std::thread::native_handle before detach; it is not valid afterward.
    if (!thread || KThread::currentThread() != thread)
        throw std::runtime_error("UWP affinity requires the current guest worker thread");
    if (!protondesktop::uwp::SetCurrentThreadAffinityCount(count))
        SchedulingFailure("UWP SetThreadAffinityMask");
}
#endif
