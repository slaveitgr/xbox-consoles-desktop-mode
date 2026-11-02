// SPDX-License-Identifier: GPL-2.0-only
#include "UwpMemory.h"
#ifndef _WIN32
#error UwpMemory.cpp requires the Windows SDK; the policy header is portable.
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <memoryapi.h>

namespace protondesktop::uwp
{
    MemoryGeometry GetMemoryGeometry() noexcept
    {
        static const MemoryGeometry geometry = [] {
            SYSTEM_INFO info{};
            GetSystemInfo(&info);
            return MemoryGeometry{info.dwAllocationGranularity, info.dwPageSize};
        }();
        return geometry;
    }
    namespace
    {
        bool ValidNativeRange(void* address, std::size_t bytes) noexcept
        {
            if (ValidPageRange(reinterpret_cast<std::uintptr_t>(address), bytes,
                               GetMemoryGeometry().pageBytes)) return true;
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
    }
    void* AllocateDataBlocks(std::uint32_t count, bool executable) noexcept
    {
        if (executable) { SetLastError(ERROR_NOT_SUPPORTED); return nullptr; }
        std::size_t bytes = 0;
        if (!CheckedBlockBytes(count, &bytes))
        { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
        return VirtualAllocFromApp(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    void* ReserveDataBlocks(std::uint32_t count) noexcept
    {
        std::size_t bytes = 0;
        if (!CheckedBlockBytes(count, &bytes))
        { SetLastError(ERROR_INVALID_PARAMETER); return nullptr; }
        return VirtualAllocFromApp(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE);
    }
    bool CommitDataPages(void* address, std::size_t bytes) noexcept
    {
        if (!ValidNativeRange(address, bytes)) return false;
        return VirtualAllocFromApp(address, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    }
    bool ProtectDataPages(void* address, std::size_t bytes, std::uint32_t guestPermission) noexcept
    {
        if (!ValidNativeRange(address, bytes)) return false;
        ULONG protection = PAGE_NOACCESS;
        switch (DataAccessForGuest(guestPermission))
        {
        case DataAccess::ReadWrite: protection = PAGE_READWRITE; break;
        case DataAccess::ReadOnly: protection = PAGE_READONLY; break;
        case DataAccess::None: break;
        }
        ULONG oldProtection = 0;
        return VirtualProtectFromApp(address, bytes, protection, &oldProtection) != FALSE;
    }
    bool ReleaseDataReservation(void* baseAddress) noexcept
    {
        if (!baseAddress) { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        // MEM_RELEASE requires the allocation's base and a zero byte count.
        return VirtualFree(baseAddress, 0, MEM_RELEASE) != FALSE;
    }
}
