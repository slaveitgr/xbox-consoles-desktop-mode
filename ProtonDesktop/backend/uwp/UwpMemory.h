// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>

namespace protondesktop::uwp
{
    constexpr std::size_t BlockBytes = 64u * 1024u;
    constexpr std::uint32_t GuestRead = 1u;
    constexpr std::uint32_t GuestWrite = 2u;
    constexpr std::uint32_t GuestExecute = 4u;
    enum class DataAccess { None, ReadOnly, ReadWrite };

    // Guest execute permission allows the interpreter to READ instruction bytes.
    // It never grants host execute permission; this port has no native JIT.
    constexpr DataAccess DataAccessForGuest(std::uint32_t permission) noexcept
    {
        return (permission & GuestWrite) ? DataAccess::ReadWrite :
            ((permission & (GuestRead | GuestExecute)) ? DataAccess::ReadOnly : DataAccess::None);
    }
    constexpr bool CheckedBlockBytes(std::uint32_t count, std::size_t* result) noexcept
    {
        if (!result || count == 0 || count > std::numeric_limits<std::size_t>::max() / BlockBytes)
            return false;
        *result = static_cast<std::size_t>(count) * BlockBytes;
        return true;
    }
    constexpr bool ValidPageRange(std::uintptr_t address, std::size_t bytes,
                                  std::size_t pageBytes) noexcept
    {
        return address != 0 && bytes != 0 && pageBytes != 0 &&
            (pageBytes & (pageBytes - 1)) == 0 && address % pageBytes == 0 &&
            bytes <= std::numeric_limits<std::uintptr_t>::max() - address;
    }

#ifdef _WIN32
    struct MemoryGeometry { std::size_t allocationBytes; std::size_t pageBytes; };
    MemoryGeometry GetMemoryGeometry() noexcept;
    // All failures set the calling thread's Win32 last error. No API below can
    // create executable pages, even if the app later gains codeGeneration.
    void* AllocateDataBlocks(std::uint32_t count, bool executable) noexcept;
    void* ReserveDataBlocks(std::uint32_t count) noexcept;
    bool CommitDataPages(void* address, std::size_t bytes) noexcept;
    bool ProtectDataPages(void* address, std::size_t bytes, std::uint32_t guestPermission) noexcept;
    bool ReleaseDataReservation(void* baseAddress) noexcept;
#endif
}
