// SPDX-License-Identifier: MIT
#include "bridge/GuestContextShims.h"
#include "runtime/GuestExecution.h"
#include <Windows.h>
#include <cstdlib>
#include <cwchar>

using namespace Win32Bridge::Bridge;

xcdm::guest::Teb64* Win32Bridge::Bridge::RequireGuestTeb() noexcept
{
    // A manually attached TEB is not enough: executable bridge calls require
    // a live owned entry scope as well as the matching per-thread binding.
    if (xcdm::guest::CurrentExecutionContext())
    {
        auto* teb = static_cast<xcdm::guest::Teb64*>(xcdm_guest_current_teb());
        if (teb) return teb;
    }
    ::RaiseException(0xc0000005u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    std::abort();
}

namespace
{
    void* WINAPI GuestNtCurrentTeb() noexcept { return RequireGuestTeb(); }
    void* WINAPI GuestRtlGetCurrentPeb() noexcept
    {
        return reinterpret_cast<void*>(RequireGuestTeb()->Peb);
    }
    ULONG WINAPI GuestRtlGetLastWin32Error() noexcept { return RequireGuestTeb()->LastErrorValue; }
    void WINAPI GuestRtlSetLastWin32Error(ULONG error) noexcept { RequireGuestTeb()->LastErrorValue = error; }
}

ImportResolution Win32Bridge::Bridge::ResolveGuestContextImport(const ImportedSymbol& symbol)
{
    ImportResolution result{};
    result.disposition = ImportDisposition::Unsupported;
    if (symbol.importedByOrdinal || _wcsicmp(symbol.library.c_str(), L"ntdll.dll") != 0)
        return result;
    if (symbol.name == L"NtCurrentTeb")
        result.targetAddress = reinterpret_cast<ULONGLONG>(&GuestNtCurrentTeb);
    else if (symbol.name == L"RtlGetCurrentPeb")
        result.targetAddress = reinterpret_cast<ULONGLONG>(&GuestRtlGetCurrentPeb);
    else if (symbol.name == L"RtlGetLastWin32Error")
        result.targetAddress = reinterpret_cast<ULONGLONG>(&GuestRtlGetLastWin32Error);
    else if (symbol.name == L"RtlSetLastWin32Error")
        result.targetAddress = reinterpret_cast<ULONGLONG>(&GuestRtlSetLastWin32Error);
    if (result.targetAddress)
    {
        result.disposition = ImportDisposition::NeedsBridge;
        result.note = L"Bridge-owned guest context; a live thread-entry scope is required.";
    }
    return result;
}
