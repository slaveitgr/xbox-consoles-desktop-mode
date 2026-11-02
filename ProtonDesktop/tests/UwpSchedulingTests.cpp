// SPDX-License-Identifier: GPL-2.0-only
#include "backend/uwp/UwpScheduling.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#include <exception>
#include <thread>
#endif

using namespace protondesktop::uwp;
namespace
{
    unsigned checks = 0;
    void Require(bool condition, const char* message)
    {
        ++checks;
        if (!condition) throw std::runtime_error(message);
    }
    void PolicyChecks()
    {
        std::uint64_t result = 0x55;
        Require(!SelectAffinityMask(0, 0, &result) && result == 0x55, "empty allowed mask fails unchanged");
        Require(!SelectAffinityMask(3, 1, nullptr), "null output fails");
        Require(SelectAffinityMask(0xA4, 0, &result) && result == 0xA4, "zero count restores all eligible bits");
        Require(SelectAffinityMask(0xA4, 1, &result) && result == 4, "one CPU respects sparse allowed mask");
        Require(SelectAffinityMask(0xA4, 2, &result) && result == 0x24, "two CPUs select lowest allowed bits");
        Require(SelectAffinityMask(0xA4, 3, &result) && result == 0xA4, "exact sparse CPU count");
        Require(!SelectAffinityMask(0xA4, 4, &result) && result == 0xA4, "too many CPUs fail without partial mask");
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        Require(SelectAffinityMask(maximum, 64, &result) && result == maximum, "all 64 bits without invalid shift");
        Require(SelectAffinityMask(std::uint64_t{1} << 63, 1, &result) && result == (std::uint64_t{1} << 63),
                "highest processor bit is retained");
        Require(!SelectAffinityMask(maximum, 65, &result) && result == (std::uint64_t{1} << 63),
                "count above mask width fails unchanged");
        Require(!SelectAffinityMask(maximum, std::numeric_limits<std::uint32_t>::max(), &result),
                "maximum integer count rejected");
        // Every possible one-CPU restriction remains within the caller's mask.
        for (unsigned bit = 0; bit < 64; ++bit)
        {
            const auto mask = std::uint64_t{1} << bit;
            Require(SelectAffinityMask(mask, 1, &result) && result == mask, "one eligible processor selection");
        }
    }
#ifdef _WIN32
    void NativeChecks()
    {
        // Isolate mutations from the test coordinator. Exceptions are captured,
        // joined, and rethrown; they never escape the std::thread entry point.
        std::exception_ptr failure;
        std::thread worker([&] {
            try
            {
                Require(SetCurrentThreadPriority(ThreadPriority::Normal), "real normal thread priority");
                Require(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_NORMAL, "read back normal priority");
                Require(!SetCurrentThreadPriority(static_cast<ThreadPriority>(99)) &&
                        GetLastError() == ERROR_INVALID_PARAMETER, "invalid priority reports error");
                Require(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_NORMAL,
                        "invalid priority leaves OS state unchanged");
                Require(SetCurrentThreadPriority(ThreadPriority::AboveNormal), "real bounded priority boost");
                Require(GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_ABOVE_NORMAL,
                        "read back actual boosted priority");
                Require(SetCurrentThreadPriority(ThreadPriority::Normal), "restore normal test priority");

                DWORD_PTR allowed = 0, system = 0;
                Require(GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system) != FALSE &&
                        allowed != 0 && system != 0, "supported current-group process mask");
                std::uint64_t selected = 0;
                Require(SelectAffinityMask(allowed, 1, &selected), "derive single eligible CPU");
                std::uint64_t previous = 0;
                Require(SetCurrentThreadAffinityCount(1, &previous) && previous != 0,
                        "real affinity API returns prior thread mask");
                GROUP_AFFINITY actual{};
                Require(GetThreadGroupAffinity(GetCurrentThread(), &actual) != FALSE && actual.Mask == selected,
                        "read back actual single CPU affinity");
                const auto beforeInvalid = actual;
                std::uint64_t unchanged = 0x55;
                Require(!SetCurrentThreadAffinityCount(65, &unchanged) && unchanged == 0x55 &&
                        GetLastError() == ERROR_INVALID_PARAMETER, "invalid affinity count fails unchanged");
                Require(GetThreadGroupAffinity(GetCurrentThread(), &actual) != FALSE &&
                        actual.Mask == beforeInvalid.Mask && actual.Group == beforeInvalid.Group,
                        "invalid count leaves real affinity unchanged");
                Require(SetCurrentThreadAffinityCount(0), "reset affinity to all process-allowed CPUs");
                Require(GetThreadGroupAffinity(GetCurrentThread(), &actual) != FALSE && actual.Mask == allowed,
                        "read back full allowed affinity");
                Require(SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(previous)) != 0,
                        "restore original worker affinity");
            }
            catch (...) { failure = std::current_exception(); }
        });
        worker.join();
        if (failure) std::rethrow_exception(failure);
    }
#endif
}
int main()
{
    try
    {
        PolicyChecks();
#ifdef _WIN32
        NativeChecks();
        std::printf("UWP scheduling: %u checks passed (Windows APIs; not an Xbox run)\n", checks);
#else
        std::printf("UWP affinity policy: %u checks passed (Windows APIs not executed)\n", checks);
#endif
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "UWP scheduling check %u failed: %s\n", checks, error.what());
        return 1;
    }
}
