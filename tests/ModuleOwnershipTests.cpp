// SPDX-License-Identifier: MIT
#include "runtime/ModuleOwnership.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

using namespace xcdm::runtime;
namespace {
int checks = 0, failures = 0;
void Check(bool passed, const char* label) {
    ++checks;
    if (!passed) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
void Classification() {
    const ModuleOwnership policy(ModuleProfile::StrictWine);
    for (const auto& name : { L"shell32", L"SHELL32.DLL", L"ShElL32.dLl" }) {
        const auto decision = policy.Decide(name);
        Check(decision.owner == ModuleOwner::Wine && decision.canonicalName == L"shell32.dll",
              "Wine module spellings select one canonical owner");
    }
    for (const auto& name : { L"ntdll.dll", L"win32u", L"KERNELBASE.DLL", L"kernel32" })
        Check(policy.Decide(name).owner == ModuleOwner::Bridge, "low seam module is bridge-owned");
    for (const auto& name : { L"ole32", L"oleaut32", L"combase", L"rpcrt4", L"user32", L"gdi32", L"advapi32", L"ucrtbase", L"winspool.drv" })
        Check(policy.Decide(name).owner == ModuleOwner::Wine, "shell and COM stack stay Wine-owned");
    Check(policy.Decide(L"API-MS-WIN-CORE-LIBRARYLOADER-L1-2-0").canonicalName == L"kernel32.dll",
          "reviewed API-set alias chooses the same module owner");
    Check(policy.Decide(L"api-ms-win-crt-runtime-l1-1-0.dll").owner == ModuleOwner::Wine,
          "CRT alias does not inherit the core bridge owner");
    for (const std::wstring name : { L"new-dll.dll", L"api-ms-win-core-unknown-l1-1-0.dll",
                                    L"ext-ms-win-ntuser-mystery-l1-1-0.dll", L"fake-ole-contract.dll" })
        Check(policy.Decide(name).owner == ModuleOwner::Reject, "unknown module/contract fails closed");
    std::vector<std::wstring> unsafe = { L"", L".", L"..", L"shell32.", L"shell32..dll",
        L" shell32.dll", L"shell32.dll ", L"C:\\Windows\\System32\\shell32.dll", L"../shell32.dll",
        L"\\\\server\\share\\shell32.dll", L"shell32.dll:stream", L"shell32.dll.other", L"sh\u0435ll32.dll",
        L"shell32\n.dll", L"shell32/dll", std::wstring(256, L'a'), std::wstring(L"shell32.dll\0ntdll.dll", 22) };
    for (const auto& name : unsafe) Check(policy.Decide(name).owner == ModuleOwner::Reject,
                                         "unsafe path, encoding or ambiguous name rejected");
    Check(!policy.AllowsBridgeHandle(L"ole32.dll"), "Wine-owned module cannot use a synthetic bridge handle");
    Check(!policy.AllowsBridgeHandle(L"new-dll.dll"), "unknown module cannot use a synthetic bridge handle");
    Check(policy.AllowsBridgeHandle(L"ntdll.dll"), "bridge seam can use a guest-only bridge handle");
    const ModuleOwnership legacy;
    Check(!legacy.IsStrict() && legacy.Decide(L"C:\\plugins\\module.dll").owner == ModuleOwner::Legacy,
          "default legacy profile preserves existing path policy");
    Check(legacy.AllowsBridgeHandle(L"ole32.dll"), "default legacy synthetic handles are unchanged");
    static_assert(!std::is_copy_assignable_v<ModuleOwnership>, "session policy must be immutable");
}
void Routing() {
    const ModuleOwnership strict(ModuleProfile::StrictWine), legacy;
    const auto exercise = [&](const ModuleDecision& decision, std::uintptr_t bridgeAddress,
                              std::uintptr_t wineAddress, std::uintptr_t expected,
                              int expectedBridgeCalls, int expectedWineCalls, const char* label) {
        int bridgeCalls = 0, wineCalls = 0;
        const auto result = RouteModuleSymbol(decision,
            [&] { ++bridgeCalls; return bridgeAddress; },
            [&] { ++wineCalls; return wineAddress; },
            [](std::uintptr_t value) { return value != 0; }, std::uintptr_t{0});
        Check(result == expected && bridgeCalls == expectedBridgeCalls && wineCalls == expectedWineCalls, label);
    };
    exercise(strict.Decide(L"shell32.dll"), 0x1111, 0x2222, 0x2222, 0, 1,
             "Wine owner ignores an available same-name bridge shim");
    exercise(strict.Decide(L"shell32.dll"), 0x1111, 0, 0, 0, 1,
             "missing Wine export never falls back to an available bridge shim");
    exercise(strict.Decide(L"ntdll.dll"), 0, 0x2222, 0, 1, 0,
             "missing bridge export never loads a same-named Wine DLL");
    exercise(strict.Decide(L"ntdll.dll"), 0x1111, 0x2222, 0x1111, 1, 0,
             "bridge owner ignores an available same-name Wine export");
    exercise(strict.Decide(L"unowned.dll"), 0x1111, 0x2222, 0, 0, 0,
             "unowned import invokes neither provider");
    exercise(strict.Decide(L"C:\\fake\\shell32.dll"), 0x1111, 0x2222, 0, 0, 0,
             "unsafe explicit path cannot bypass the selected owner");
    exercise(legacy.Decide(L"shell32.dll"), 0x1111, 0x2222, 0x1111, 1, 0,
             "legacy known shim remains first");
    exercise(legacy.Decide(L"plugin.dll"), 0, 0x2222, 0x2222, 1, 1,
             "legacy unknown shim retains guest DLL fallback");
    exercise(strict.Decide(L"api-ms-win-crt-runtime-l1-1-0.dll"), 0x1111, 0x2222, 0x2222, 0, 1,
             "reviewed CRT contract uses the Wine route");
    exercise(strict.Decide(L"api-ms-win-core-libraryloader-l1-2-0.dll"), 0x1111, 0x2222, 0x1111, 1, 0,
             "reviewed libraryloader contract uses the bridge route");

    struct Failure { std::uintptr_t address; const char* note; };
    const auto failedLegacy = RouteModuleSymbol(legacy.Decide(L"missing.dll"),
        [] { return Failure{0, "original shim diagnostic"}; },
        [] { return Failure{0, "guest lookup diagnostic"}; },
        [](const Failure& value) { return value.address != 0; }, Failure{0, "rejected"});
    Check(std::string(failedLegacy.note) == "original shim diagnostic",
          "legacy unresolved imports retain their original shim diagnostic");

    // A forwarded export legitimately changes modules and providers. Exercise
    // nested routing itself, not a fake PE loader or a simulated Explorer.
    int bridgeCalls = 0, wineCalls = 0;
    const auto resolved = [](std::uintptr_t value) { return value != 0; };
    const auto forwarded = RouteModuleSymbol(strict.Decide(L"shell32.dll"),
        [&] { ++bridgeCalls; return std::uintptr_t{0xdead}; },
        [&] {
            ++wineCalls;
            return RouteModuleSymbol(strict.Decide(L"kernel32.dll"),
                [&] { ++bridgeCalls; return std::uintptr_t{0x1234}; },
                [&] { ++wineCalls; return std::uintptr_t{0xdead}; }, resolved, std::uintptr_t{0});
        }, resolved, std::uintptr_t{0});
    Check(forwarded == 0x1234 && bridgeCalls == 1 && wineCalls == 1,
          "forwarder target is reclassified by target module rather than source owner");
    bridgeCalls = wineCalls = 0;
    const auto missing = RouteModuleSymbol(strict.Decide(L"shell32.dll"),
        [&] { ++bridgeCalls; return std::uintptr_t{0xdead}; },
        [&] {
            ++wineCalls;
            return RouteModuleSymbol(strict.Decide(L"ole32.dll"),
                [&] { ++bridgeCalls; return std::uintptr_t{0xdead}; },
                [&] { ++wineCalls; return std::uintptr_t{0}; }, resolved, std::uintptr_t{0});
        }, resolved, std::uintptr_t{0});
    Check(missing == 0 && bridgeCalls == 0 && wineCalls == 2,
          "missing forwarded Wine export cannot select the bridge at either level");
    Check(strict.Decide(L"api-ms-win-crt-runtime-l1-1-0.dll").canonicalName == strict.Decide(L"ucrtbase.dll").canonicalName,
          "aliases share cache identity and cannot split ownership");
    Check(legacy.Decide(L"ole32").owner == ModuleOwner::Legacy && strict.Decide(L"ole32").owner == ModuleOwner::Wine,
          "concurrent session profiles do not alter each other's ownership");
}

} // namespace
int main() {
    Classification(); Routing();
    std::cout << checks << " module ownership checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
