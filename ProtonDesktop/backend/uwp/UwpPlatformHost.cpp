// SPDX-License-Identifier: GPL-2.0-only
// Real host-service subset of pinned Boxedwine64 Platform declarations.
// Other Platform methods remain undefined until separately ported/qualified.
#include "boxedwine.h"
#include "UwpHostServices.h"
#include <Windows.h>
#include <cstdlib>
#if defined(BOXEDWINE_JIT) || defined(BOXEDWINE_JIT_X64) || defined(BOXEDWINE_JIT_ARMV8)
#error The ProtonDesktop UWP host adapter supports software interpretation only.
#endif
static_assert(sizeof(void*) == 8, "The UWP interpreter target must be x64");
namespace
{
    [[noreturn]] void HostFailure(const char* operation)
    {
        const auto error = static_cast<unsigned long>(GetLastError());
        kpanic_fmt("ProtonDesktop UWP %s failed (Win32 error %lu)", operation, error);
        std::abort();
    }
}
void Platform::init()
{
    if (!protondesktop::uwp::ResetMicroCounter()) HostFailure("init counter");
}
void Platform::startMicroCounter()
{
    if (!protondesktop::uwp::ResetMicroCounter()) HostFailure("startMicroCounter");
}
U64 Platform::getMicroCounter()
{
    std::uint64_t value = 0;
    if (!protondesktop::uwp::ReadMicroCounter(&value)) HostFailure("getMicroCounter");
    return value;
}
U64 Platform::getSystemTimeAsMicroSeconds()
{
    std::uint64_t value = 0;
    if (!protondesktop::uwp::ReadUnixMicroseconds(&value)) HostFailure("getSystemTimeAsMicroSeconds");
    return value;
}
U32 Platform::nanoSleep(U64 nanoseconds)
{
    if (!protondesktop::uwp::SleepNanoseconds(nanoseconds)) HostFailure("nanoSleep");
    return 0;
}
U32 Platform::getCpuCount()
{
    std::uint32_t count = 0;
    if (!protondesktop::uwp::ReadCpuCount(&count)) HostFailure("getCpuCount");
    return count;
}
void Platform::listNodes(BString nativePath, std::vector<ListNodeResult>& results)
{
    std::vector<protondesktop::uwp::DirectoryEntry> entries;
    if (!protondesktop::uwp::EnumerateDirectory(
            std::string(nativePath.c_str(), static_cast<std::size_t>(nativePath.length())), &entries))
        HostFailure("listNodes");
    // Preserve the upstream append-to-results contract; all OS enumeration and
    // UTF-8 conversion completed before adding any entries to the caller.
    for (const auto& entry : entries)
        results.emplace_back(BString::copy(entry.utf8Name.c_str()), entry.isDirectory);
}
