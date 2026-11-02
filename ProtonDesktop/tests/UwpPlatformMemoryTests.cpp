// SPDX-License-Identifier: GPL-2.0-only
#include "backend/uwp/UwpMemory.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <Windows.h>
#endif
using namespace protondesktop::uwp;
namespace
{
    unsigned checks = 0;
    void Require(bool condition, const char* name)
    {
        ++checks;
        if (!condition) throw std::runtime_error(name);
    }
    void PolicyChecks()
    {
        std::size_t size = 123;
        Require(!CheckedBlockBytes(0, &size) && size == 123, "zero block count fails without changing size");
        Require(!CheckedBlockBytes(1, nullptr), "null size output fails");
        Require(CheckedBlockBytes(1, &size) && size == 65536, "one 64KiB block");
        Require(CheckedBlockBytes(2, &size) && size == 131072, "two 64KiB blocks");
        const bool maxFits = std::numeric_limits<std::uint32_t>::max() <=
            std::numeric_limits<std::size_t>::max() / BlockBytes;
        Require(CheckedBlockBytes(std::numeric_limits<std::uint32_t>::max(), &size) == maxFits,
                "block multiplication respects the host size limit");
        const DataAccess expected[] = {DataAccess::None, DataAccess::ReadOnly,
            DataAccess::ReadWrite, DataAccess::ReadWrite, DataAccess::ReadOnly,
            DataAccess::ReadOnly, DataAccess::ReadWrite, DataAccess::ReadWrite};
        for (std::uint32_t flags = 0; flags < 8; ++flags)
            Require(DataAccessForGuest(flags) == expected[flags], "all guest permission combinations map to data access");
        Require(DataAccessForGuest(0x80 | GuestExecute) == DataAccess::ReadOnly,
                "guest execute plus bookkeeping flags never becomes host execute");
        Require(ValidPageRange(0x10000, 4096, 4096), "aligned ordinary page range");
        Require(ValidPageRange(0x10000, 1, 4096), "nonzero partial final page is permitted");
        Require(!ValidPageRange(0, 4096, 4096), "null address denied");
        Require(!ValidPageRange(0x10000, 0, 4096), "zero byte range denied");
        Require(!ValidPageRange(0x10001, 4096, 4096), "unaligned base denied");
        Require(!ValidPageRange(0x10000, 4096, 0), "zero geometry denied");
        Require(!ValidPageRange(0x10000, 4096, 3000), "non-power-of-two geometry denied");
        const auto lastPage = std::numeric_limits<std::uintptr_t>::max() & ~std::uintptr_t{4095};
        Require(!ValidPageRange(lastPage, 4096, 4096), "pointer range wrap denied");
        Require(ValidPageRange(lastPage, 4095, 4096), "last representable nonwrapping page prefix");
    }
#ifdef _WIN32
    struct Reservation
    {
        void* address;
        ~Reservation() { if (address) ReleaseDataReservation(address); }
    };
    MEMORY_BASIC_INFORMATION Query(void* address)
    {
        MEMORY_BASIC_INFORMATION result{};
        Require(VirtualQuery(address, &result, sizeof(result)) == sizeof(result), "VirtualQuery succeeds");
        return result;
    }
    void NativeChecks()
    {
        const auto geometry = GetMemoryGeometry();
        Require(geometry.pageBytes >= 4096 && geometry.allocationBytes >= geometry.pageBytes &&
                geometry.allocationBytes % geometry.pageBytes == 0, "host memory geometry");
        SetLastError(0);
        Require(AllocateDataBlocks(1, true) == nullptr && GetLastError() == ERROR_NOT_SUPPORTED,
                "host executable allocation is rejected");
        SetLastError(0);
        Require(AllocateDataBlocks(0, false) == nullptr && GetLastError() == ERROR_INVALID_PARAMETER,
                "zero allocation is rejected");
        Reservation reserved{ReserveDataBlocks(1)};
        Require(reserved.address != nullptr, "reserve 64KiB via VirtualAllocFromApp");
        Require(Query(reserved.address).State == MEM_RESERVE, "reservation is not committed");
        Require(CommitDataPages(reserved.address, geometry.pageBytes), "commit reserved data page via VirtualAllocFromApp");
        auto* data = static_cast<unsigned char*>(reserved.address);
        Require(data[0] == 0 && data[geometry.pageBytes - 1] == 0, "committed page is zero initialized");
        data[0] = 0x37;
        data[geometry.pageBytes - 1] = 0xA9;
        Require(Query(reserved.address).Protect == PAGE_READWRITE, "data allocation has no execute permission");
        Require(ProtectDataPages(reserved.address, geometry.pageBytes, GuestExecute),
                "guest execute memory changes to readable host data");
        Require(Query(reserved.address).Protect == PAGE_READONLY && data[0] == 0x37,
                "guest execute maps to PAGE_READONLY and preserves bytes");
        Require(ProtectDataPages(reserved.address, geometry.pageBytes, 0), "protect no-access data page");
        Require(Query(reserved.address).Protect == PAGE_NOACCESS, "no-access protection is applied");
        Require(ProtectDataPages(reserved.address, geometry.pageBytes, GuestRead | GuestWrite | GuestExecute),
                "restore writable guest page without native execute");
        Require(Query(reserved.address).Protect == PAGE_READWRITE && data[geometry.pageBytes - 1] == 0xA9,
                "guest RWX is host RW and preserves bytes");
        SetLastError(0);
        Require(!ProtectDataPages(data + 1, geometry.pageBytes, GuestRead) && GetLastError() == ERROR_INVALID_PARAMETER,
                "unaligned protection request rejected before OS rounding");
        SetLastError(0);
        Require(!CommitDataPages(nullptr, geometry.pageBytes) && GetLastError() == ERROR_INVALID_PARAMETER,
                "null commit cannot allocate a new unrelated region");
        Require(ReleaseDataReservation(reserved.address), "release full reservation with MEM_RELEASE size zero");
        reserved.address = nullptr;
        Reservation committed{AllocateDataBlocks(1, false)};
        Require(committed.address != nullptr, "reserve and commit data blocks via FromApp");
        const auto allocated = Query(committed.address);
        Require(allocated.State == MEM_COMMIT && allocated.Protect == PAGE_READWRITE,
                "block allocation is committed writable non-executable data");
        SetLastError(0);
        Require(!ReleaseDataReservation(nullptr) && GetLastError() == ERROR_INVALID_PARAMETER,
                "null release fails explicitly");
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
        std::printf("UWP memory adapter: %u checks passed (Windows FromApp APIs; not an Xbox run)\n", checks);
#else
        std::printf("UWP memory policy: %u checks passed (portable policy only; Windows APIs not executed)\n", checks);
#endif
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "UWP memory check %u failed: %s\n", checks, error.what());
        return 1;
    }
}
