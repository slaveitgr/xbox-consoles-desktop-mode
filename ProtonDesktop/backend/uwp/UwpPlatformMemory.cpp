// SPDX-License-Identifier: GPL-2.0-only
// Replacement definitions for the memory-method block of the pinned upstream
// platform/windows/platform.cpp. Never link both sets of definitions.
#include "boxedwine.h"
#include "UwpMemory.h"
#include <Windows.h>
#include <cstdlib>

#if defined(BOXEDWINE_JIT) || defined(BOXEDWINE_JIT_X64) || defined(BOXEDWINE_JIT_ARMV8)
#error The ProtonDesktop UWP memory adapter supports software interpretation only.
#endif
static_assert(sizeof(void*) == 8, "The UWP interpreter target must be x64");
static_assert(PAGE_READ == protondesktop::uwp::GuestRead &&
              PAGE_WRITE == protondesktop::uwp::GuestWrite &&
              PAGE_EXEC == protondesktop::uwp::GuestExecute, "Upstream guest permission ABI changed");
namespace
{
    [[noreturn]] void MemoryFailure(const char* operation)
    {
        const auto error = static_cast<unsigned long>(GetLastError());
        kpanic_fmt("ProtonDesktop UWP %s failed (Win32 error %lu)", operation, error);
        std::abort();
    }
    U32 GeometryPages(std::size_t bytes)
    {
        if (bytes < K_PAGE_SIZE || bytes % K_PAGE_SIZE != 0 ||
            bytes / K_PAGE_SIZE > static_cast<std::size_t>(std::numeric_limits<U32>::max()))
        {
            SetLastError(ERROR_NOT_SUPPORTED);
            MemoryFailure("memory geometry");
        }
        return static_cast<U32>(bytes / K_PAGE_SIZE);
    }
}
U32 Platform::getPageAllocationGranularity()
{ return GeometryPages(protondesktop::uwp::GetMemoryGeometry().allocationBytes); }
U32 Platform::getPagePermissionGranularity()
{ return GeometryPages(protondesktop::uwp::GetMemoryGeometry().pageBytes); }
U32 Platform::allocateNativeMemory(U64 address)
{
    if (!protondesktop::uwp::CommitDataPages(reinterpret_cast<void*>(address),
            protondesktop::uwp::GetMemoryGeometry().allocationBytes))
        MemoryFailure("allocateNativeMemory");
    return 0;
}
void Platform::releaseNativeMemory(void* address, U64 /*len*/)
{
    if (!protondesktop::uwp::ReleaseDataReservation(address))
        MemoryFailure("releaseNativeMemory");
}
U8* Platform::alloc64kBlock(U32 count, bool executable)
{
    auto* result = static_cast<U8*>(protondesktop::uwp::AllocateDataBlocks(count, executable));
    if (!result) MemoryFailure("alloc64kBlock");
    return result;
}
U8* Platform::reserveNativeMemory64k(U32 count)
{
    auto* result = static_cast<U8*>(protondesktop::uwp::ReserveDataBlocks(count));
    if (!result) MemoryFailure("reserveNativeMemory64k");
    return result;
}
void Platform::commitNativeMemoryPage(void* address)
{
    if (!protondesktop::uwp::CommitDataPages(address, K_PAGE_SIZE))
        MemoryFailure("commitNativeMemoryPage");
}
U32 Platform::updateNativePermission(U64 address, U32 permission, U32 len)
{
    const auto bytes = len ? static_cast<std::size_t>(len) :
        protondesktop::uwp::GetMemoryGeometry().pageBytes;
    if (!protondesktop::uwp::ProtectDataPages(reinterpret_cast<void*>(address), bytes,
                                            permission & PAGE_PERMISSION_MASK))
        MemoryFailure("updateNativePermission");
    return 0;
}
