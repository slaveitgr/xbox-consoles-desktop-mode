// SPDX-License-Identifier: MIT
#include "ModuleOwnership.h"


namespace xcdm::runtime {
namespace {
struct Entry { std::wstring_view name; ModuleOwner owner; };
constexpr Entry owners[] = {
    { L"ntdll.dll", ModuleOwner::Bridge },
    { L"win32u.dll", ModuleOwner::Bridge },
    { L"kernel32.dll", ModuleOwner::Bridge },
    { L"kernelbase.dll", ModuleOwner::Bridge },
    { L"user32.dll", ModuleOwner::Wine },
    { L"gdi32.dll", ModuleOwner::Wine },
    { L"gdi32full.dll", ModuleOwner::Wine },
    { L"comctl32.dll", ModuleOwner::Wine },
    { L"comdlg32.dll", ModuleOwner::Wine },
    { L"shell32.dll", ModuleOwner::Wine },
    { L"shlwapi.dll", ModuleOwner::Wine },
    { L"shcore.dll", ModuleOwner::Wine },
    { L"ole32.dll", ModuleOwner::Wine },
    { L"oleaut32.dll", ModuleOwner::Wine },
    { L"combase.dll", ModuleOwner::Wine },
    { L"rpcrt4.dll", ModuleOwner::Wine },
    { L"advapi32.dll", ModuleOwner::Wine },
    { L"msvcrt.dll", ModuleOwner::Wine },
    { L"ucrtbase.dll", ModuleOwner::Wine },
    { L"msvcp_win.dll", ModuleOwner::Wine },
    { L"version.dll", ModuleOwner::Wine },
    { L"imm32.dll", ModuleOwner::Wine },
    { L"winspool.drv", ModuleOwner::Wine },
};
struct Alias { std::wstring_view name; std::wstring_view target; };
// Only reviewed exact contracts are accepted here. Extending this list is a
// profile change, not an invitation to infer arbitrary names from substrings.
constexpr Alias aliases[] = {
    { L"api-ms-win-core-libraryloader-l1-1-0.dll", L"kernel32.dll" },
    { L"api-ms-win-core-libraryloader-l1-2-0.dll", L"kernel32.dll" },
    { L"api-ms-win-core-heap-l1-1-0.dll", L"kernel32.dll" },
    { L"api-ms-win-core-synch-l1-1-0.dll", L"kernel32.dll" },
    { L"api-ms-win-core-synch-l1-2-0.dll", L"kernel32.dll" },
    { L"api-ms-win-core-processthreads-l1-1-0.dll", L"kernel32.dll" },
    { L"api-ms-win-core-errorhandling-l1-1-0.dll", L"kernel32.dll" },
    { L"api-ms-win-core-file-l1-1-0.dll", L"kernel32.dll" },
    { L"api-ms-win-core-memory-l1-1-0.dll", L"kernel32.dll" },
    { L"api-ms-win-core-string-l1-1-0.dll", L"kernel32.dll" },
    { L"api-ms-win-core-sysinfo-l1-1-0.dll", L"kernel32.dll" },
    { L"api-ms-win-crt-runtime-l1-1-0.dll", L"ucrtbase.dll" },
    { L"api-ms-win-crt-heap-l1-1-0.dll", L"ucrtbase.dll" },
    { L"api-ms-win-crt-string-l1-1-0.dll", L"ucrtbase.dll" },
};

bool CanonicalName(std::wstring_view requested, std::wstring& name) {
    if (requested.empty() || requested.size() > 255) return false;
    name.reserve(requested.size() + 4);
    for (wchar_t character : requested) {
        if (character >= L'A' && character <= L'Z') character += L'a' - L'A';
        if (!((character >= L'a' && character <= L'z') ||
              (character >= L'0' && character <= L'9') ||
              character == L'_' || character == L'-' || character == L'.')) return false;
        name.push_back(character);
    }
    if (name.front() == L'.' || name.back() == L'.' || name.find(L"..") != std::wstring::npos)
        return false;
    const auto dot = name.find(L'.');
    if (dot == std::wstring::npos) name += L".dll";
    else if (name.find(L'.', dot + 1) != std::wstring::npos) return false;
    return true;
}
} // namespace

ModuleDecision ModuleOwnership::Decide(std::wstring_view requestedName) const {
    if (!IsStrict()) return { ModuleOwner::Legacy, std::wstring(requestedName), L"Legacy shim-first resolution." };
    std::wstring canonical;
    if (!CanonicalName(requestedName, canonical))
        return { ModuleOwner::Reject, {}, L"Strict Wine requires a bare ASCII module name; paths, NULs and ambiguous names are rejected." };
    for (const auto& alias : aliases)
        if (canonical == alias.name) { canonical = alias.target; break; }
    for (const auto& entry : owners)
        if (canonical == entry.name) return { entry.owner, std::move(canonical), {} };
    return { ModuleOwner::Reject, std::move(canonical), L"Module or API-set contract has no owner in the strict Wine profile." };
}

bool ModuleOwnership::AllowsBridgeHandle(std::wstring_view canonicalName) const {
    return !IsStrict() || Decide(canonicalName).owner == ModuleOwner::Bridge;
}
} // namespace xcdm::runtime
