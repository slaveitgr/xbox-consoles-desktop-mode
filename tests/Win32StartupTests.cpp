#include "runtime/Win32Probe32.h"
#include "samples/X86Win32Fixture.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace {
using Bytes = std::vector<std::uint8_t>;
int failures = 0, checks = 0;
void Check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
void Dword(Bytes& bytes, std::uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) bytes.push_back(static_cast<std::uint8_t>(value >> (n * 8)));
}
void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) bytes.at(at + n) = static_cast<std::uint8_t>(value >> (n * 8));
}
bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
void Call(Bytes& bytes, std::uint32_t iat) { bytes.insert(bytes.end(), {0xff, 0x15}); Dword(bytes, iat); }
void Exit(Bytes& bytes, std::uint8_t code, std::uint32_t base = 0x400000) { bytes.insert(bytes.end(), {0x6a, code}); Call(bytes, base + 0x206c); bytes.insert(bytes.end(), {0xeb, 0xfe}); }
Bytes Program(Bytes code) {
    if (code.size() > 0x200) throw std::length_error("guest test exceeds its code section");
    auto bytes = xcdm::samples::X86Win32PeBytes();
    std::fill(bytes.begin() + 0x200, bytes.begin() + 0x400, 0xcc);
    std::copy(code.begin(), code.end(), bytes.begin() + 0x200);
    Put32(bytes, 0x178 + 8, static_cast<std::uint32_t>(code.size()));
    return bytes;
}
void CompareAndExit(Bytes& code, std::uint32_t value) {
    code.push_back(0x3d); Dword(code, value); // cmp eax, expected
    code.insert(code.end(), {0x75, 10}); // jne over success ExitProcess + self-loop
    Exit(code, 0); Exit(code, 1);
}
auto Trace(const Bytes& bytes, std::uint64_t limit = 50000) {
    return xcdm::runtime::TraceWin32Startup32(bytes.data(), bytes.size(), limit);
}

constexpr std::uint32_t VersionIat = 0x402068;
constexpr std::uint32_t VersionBuffer = 0x402120;
constexpr std::uint32_t ExpectedBuffer = 0x403100;
constexpr std::uint32_t InitialError = 0x1234abcd;

// These programs verify the public stdcall contract from inside the guest.
// Failed assertions take the guest ExitProcess(1) path; only success exits 0.
struct GuestAssertions {
    Bytes code;
    std::vector<std::size_t> failures;
    void Equal() {
        code.insert(code.end(), {0x0f, 0x85}); // jne failure
        failures.push_back(code.size()); Dword(code, 0);
    }
    void Eax(std::uint32_t value) { code.push_back(0x3d); Dword(code, value); Equal(); }
    void Register(unsigned reg, std::uint32_t value) {
        code.insert(code.end(), {0x81, static_cast<std::uint8_t>(0xf8U + reg)});
        Dword(code, value); Equal();
    }
    void Memory(std::uint32_t actual, std::uint32_t expected, std::uint32_t size) {
        code.push_back(0xbe); Dword(code, actual); // mov esi,actual
        code.push_back(0xbf); Dword(code, expected); // mov edi,expected
        code.push_back(0xb9); Dword(code, size); // mov ecx,size
        const auto loop = code.size();
        code.insert(code.end(), {0x8a, 0x06, 0x3a, 0x07}); // mov al,[esi]; cmp al,[edi]
        Equal();
        code.insert(code.end(), {0x46, 0x47, 0x49}); // inc esi; inc edi; dec ecx
        const auto distance = code.size() + 2 - loop;
        code.insert(code.end(), {0x75, static_cast<std::uint8_t>(256U - distance)});
    }
    Bytes Finish(std::uint32_t base = 0x400000) {
        Exit(code, 0, base);
        const auto failure = code.size();
        Exit(code, 1, base);
        for (auto at : failures) Put32(code, at, static_cast<std::uint32_t>(failure - (at + 4)));
        return code;
    }
};

void Push(Bytes& code, std::uint32_t value) { code.push_back(0x68); Dword(code, value); }
void PrepareApiState(GuestAssertions& guest, std::uint32_t base = 0x400000) {
    Push(guest.code, InitialError); Call(guest.code, base + 0x2060);
    for (const auto reg : {3U, 5U, 6U, 7U}) {
        guest.code.push_back(static_cast<std::uint8_t>(0xb8U + reg));
        Dword(guest.code, 0x11223300U + reg);
    }
    guest.code.insert(guest.code.end(), {0x89, 0x25}); Dword(guest.code, base + 0x2100); // mov [saved],esp
}
void BeginSingleArgumentCall(GuestAssertions& guest, std::uint32_t pointer, std::uint32_t base = 0x400000) {
    PrepareApiState(guest, base);
    Push(guest.code, pointer); Call(guest.code, base + 0x2068);
}
void CheckApiReturn(GuestAssertions& guest, std::uint32_t value, std::uint32_t error, std::uint32_t base = 0x400000) {
    guest.Eax(value);
    guest.code.insert(guest.code.end(), {0x3b, 0x25}); Dword(guest.code, base + 0x2100); // cmp esp,[saved]
    guest.Equal();
    for (const auto reg : {3U, 5U, 6U, 7U}) guest.Register(reg, 0x11223300U + reg);
    Call(guest.code, base + 0x2064); guest.Eax(error);
}
Bytes ThirdImportProgram(Bytes code, const std::string& name) {
    auto image = Program(std::move(code));
    if (name.size() >= 0x1e) throw std::length_error("guest import name exceeds its fixture slot");
    std::fill(image.begin() + 0x4c2, image.begin() + 0x4e0, 0);
    std::copy(name.begin(), name.end(), image.begin() + 0x4c2);
    return image;
}
Bytes VersionProgram(Bytes code) { return ThirdImportProgram(std::move(code), "GetVersionExA"); }
void Place(Bytes& image, std::size_t offset, const Bytes& value) {
    if (offset > image.size() || value.size() > image.size() - offset)
        throw std::length_error("guest test data exceeds fixture");
    std::copy(value.begin(), value.end(), image.begin() + offset);
}
void VersionSuccess(std::uint32_t size, std::uint32_t pointer, bool sectionEnd, const char* label) {
    // Nonzero initial bytes expose missed clearing; four-byte guards expose
    // writes beyond either accepted Windows structure size.
    const auto span = size + (sectionEnd ? 4U : 8U);
    Bytes initial(span, 0xa5), expected = initial;
    Put32(initial, 4, size);
    std::fill(expected.begin() + 4, expected.begin() + 4 + size, 0);
    Put32(expected, 4, size); Put32(expected, 8, 10); Put32(expected, 12, 0);
    Put32(expected, 16, 19045); Put32(expected, 20, 2);
    if (size == 156) expected.at(4 + 154) = 1; // VER_NT_WORKSTATION
    GuestAssertions guest; BeginSingleArgumentCall(guest, pointer);
    CheckApiReturn(guest, 1, InitialError);
    guest.Memory(pointer - 4, ExpectedBuffer, span);
    auto image = VersionProgram(guest.Finish());
    Place(image, 0x400 + (pointer - 4 - 0x402000), initial);
    Place(image, 0x700, expected);
    const auto result = Trace(image);
    Check(result.passed && Has(result.report, "Guest exit code: 0") && Has(result.report, "GetVersionExA"), label);
    if (!result.passed) std::cerr << result.report;
}
void VersionInvalidSize(std::uint32_t size) {
    Bytes unchanged(164, 0xa5); Put32(unchanged, 4, size);
    GuestAssertions guest; BeginSingleArgumentCall(guest, VersionBuffer);
    CheckApiReturn(guest, 0, 122); // ERROR_INSUFFICIENT_BUFFER, virtual profile policy
    guest.Memory(VersionBuffer - 4, ExpectedBuffer, static_cast<std::uint32_t>(unchanged.size()));
    auto image = VersionProgram(guest.Finish());
    Place(image, 0x51c, unchanged); Place(image, 0x700, unchanged);
    const auto result = Trace(image);
    Check(result.passed, "GetVersionExA rejects wrong size with FALSE/error 122 and leaves all buffer bytes unchanged");
    if (!result.passed) std::cerr << "Rejected size " << size << ":\n" << result.report;
}
void VersionFault(Bytes image, const char* label) {
    const auto result = Trace(image);
    const bool faulted = !result.passed && Has(result.report, "Host-call failure: GetVersionExA") &&
          (Has(result.report, "buffer") || Has(result.report, "writable") || Has(result.report, "frame")) &&
          !Has(result.report, "\n  ExitProcess\n");
    Check(faulted, label);
    // A terminal marshalling fault exposes no guest-memory snapshot; these
    // checks intentionally do not claim to observe its destination afterward.
    if (!faulted) std::cerr << result.report;
}
void GetVersionExTests() {
    VersionSuccess(148, VersionBuffer, false, "GetVersionExA fills OSVERSIONINFOA, preserves guards/last-error/registers and balances stdcall stack");
    VersionSuccess(156, VersionBuffer, false, "GetVersionExA fills EX service-pack/suite/product/reserved fields and preserves adjacent bytes");
    VersionSuccess(148, VersionBuffer + 1, false, "GetVersionExA marshals an unaligned guest structure without host pointer casts");
    VersionSuccess(156, 0x402164, true, "GetVersionExA accepts a complete structure ending exactly at its writable section boundary");
    for (const auto size : {0U, 4U, 147U, 149U, 152U, 155U, 157U, 0xffffffffU}) VersionInvalidSize(size);

    // Wrong-size rejection requires only its four-byte size field, including
    // at the end of a readable, nonwritable section.
    GuestAssertions shortSize; BeginSingleArgumentCall(shortSize, 0x4031fc);
    CheckApiReturn(shortSize, 0, 122);
    shortSize.code.push_back(0xa1); Dword(shortSize.code, 0x4031fc); shortSize.Eax(147);
    auto image = VersionProgram(shortSize.Finish()); Put32(image, 0x7fc, 147);
    Check(Trace(image).passed, "GetVersionExA wrong-size failure needs only four readable bytes and performs no write");

    for (const auto pointer : {0U, 0x20000000U, 0xfffffffeU, 0x4031feU}) {
        GuestAssertions guest; BeginSingleArgumentCall(guest, pointer); Exit(guest.code, 0);
        VersionFault(VersionProgram(guest.code), "GetVersionExA faults on null, unmapped or wrapping size-field address");
    }
    for (const auto pointer : {0x403020U, 0x400040U, 0x4021fcU}) {
        GuestAssertions guest; BeginSingleArgumentCall(guest, pointer); Exit(guest.code, 0);
        image = VersionProgram(guest.code);
        const auto raw = pointer == 0x403020U ? 0x620U : pointer == 0x400040U ? 0x40U : 0x5fcU;
        Put32(image, raw, 148);
        VersionFault(image, "GetVersionExA faults on read-only/header/truncated output extent");
    }
    GuestAssertions overlap;
    // The argument points into its own live stdcall frame. GetVersionExA must
    // reject this before overwriting the return address or pointer argument.
    Push(overlap.code, InitialError); Call(overlap.code, 0x402060);
    overlap.code.insert(overlap.code.end(), {0x81, 0xec}); Dword(overlap.code, 200); // sub esp,200
    overlap.code.insert(overlap.code.end(), {0x8d, 0x44, 0x24, 0xf0, 0xc7, 0x00}); // lea eax,[esp-16]; mov [eax],148
    Dword(overlap.code, 148); overlap.code.push_back(0x50); // push eax
    Call(overlap.code, VersionIat); Exit(overlap.code, 0);
    VersionFault(VersionProgram(overlap.code), "GetVersionExA rejects output overlapping its live return address and argument frame");
}

Bytes ModuleProgram(Bytes code) { return ThirdImportProgram(std::move(code), "GetModuleHandleA"); }
void ModuleNull(std::uint32_t base, const char* label, const std::string& api = "GetModuleHandleA") {
    GuestAssertions guest; BeginSingleArgumentCall(guest, 0, base);
    guest.Eax(base);
    guest.code.push_back(0xa3); Dword(guest.code, base + 0x2104); // save returned guest handle
    CheckApiReturn(guest, base, InitialError, base);
    guest.code.push_back(0xa1); Dword(guest.code, base + 0x2104);
    guest.code.insert(guest.code.end(), {0x0f, 0xb7, 0x00}); // movzx eax,word [handle]
    guest.Eax(0x5a4d); // handle addresses the mapped DOS header
    guest.code.push_back(0xa1); Dword(guest.code, base + 0x2104);
    guest.code.insert(guest.code.end(), {0x8b, 0x50, 0x3c, 0x01, 0xd0, 0x8b, 0x00}); // handle + e_lfanew -> PE signature
    guest.Eax(0x4550);
    auto image = ThirdImportProgram(guest.Finish(base), api);
    Put32(image, 0xb4, base); // a second fixed preferred base, not relocation support
    const auto result = Trace(image);
    Check(result.passed, label);
    if (!result.passed) std::cerr << result.report;
}
void ModuleName(const Bytes& name, const char* label, const std::string& api = "GetModuleHandleA", std::uint32_t pointer = 0x403020) {
    GuestAssertions guest; BeginSingleArgumentCall(guest, pointer);
    CheckApiReturn(guest, 0, 126);
    guest.Memory(pointer, ExpectedBuffer, static_cast<std::uint32_t>(name.size()));
    auto image = ThirdImportProgram(guest.Finish(), api);
    Place(image, 0x600 + pointer - 0x403000, name); Place(image, 0x700, name);
    const auto result = Trace(image);
    Check(result.passed, label);
    if (!result.passed) std::cerr << result.report;
}
void ModuleFault(Bytes image, const char* label) {
    const auto result = Trace(image);
    const bool faulted = !result.passed &&
        Has(result.report, "Host-call failure: GetModuleHandleA") &&
        Has(result.report, "NUL-terminated ANSI guest name within 512 bytes") &&
        Has(result.report, "Guest last error: 0x1234abcd") &&
        !Has(result.report, "\n  ExitProcess\n");
    Check(faulted, label);
    if (!faulted) std::cerr << result.report;
}
void GetModuleHandleTests() {
    ModuleNull(0x400000, "GetModuleHandleA(NULL) returns the readable guest image header and preserves last-error/stdcall/nonvolatile registers");
    ModuleNull(0x5100000, "GetModuleHandleA(NULL) follows a second fixed preferred guest base rather than a constant or host module");
    for (const auto* name : {"", "KERNEL32.dll", "kernel32", "x86-win32-fixture.exe", "gta_sa.exe", "C:\\games\\gta_sa.exe"}) {
        Bytes text(name, name + std::char_traits<char>::length(name)); text.push_back(0);
        ModuleName(text, "GetModuleHandleA named lookup returns NULL/error 126 without modifying the input or inventing handles");
    }
    ModuleName(Bytes{'n', 0x80, 0xff, 0}, "GetModuleHandleA accepts non-ASCII ANSI name bytes before returning the explicit named-lookup failure");

    // NUL at the exact last readable byte is enough; scanning past it would
    // cross the section boundary and fault.
    GuestAssertions terminal; BeginSingleArgumentCall(terminal, 0x4031ff);
    CheckApiReturn(terminal, 0, 126);
    auto image = ModuleProgram(terminal.Finish()); image[0x7ff] = 0;
    Check(Trace(image).passed, "GetModuleHandleA accepts an empty name terminated at the final readable byte");

    GuestAssertions longest; BeginSingleArgumentCall(longest, 0x403000);
    CheckApiReturn(longest, 0, 126);
    image = ModuleProgram(longest.Finish());
    std::fill(image.begin() + 0x600, image.end(), 'a'); image[0x7ff] = 0;
    Check(Trace(image).passed, "GetModuleHandleA accepts a NUL at offset 511 within its 512-byte bound");

    for (const auto pointer : {0x20000000U, 0xffffffffU, 0x403200U, 0x4031ffU}) {
        GuestAssertions guest; BeginSingleArgumentCall(guest, pointer); Exit(guest.code, 0);
        image = ModuleProgram(guest.code); image[0x7ff] = 'x';
        ModuleFault(image, "GetModuleHandleA faults on unmapped/wrapping pointers or an unterminated name at the mapped boundary");
    }
    GuestAssertions unterminated; BeginSingleArgumentCall(unterminated, 0x403000); Exit(unterminated.code, 0);
    image = ModuleProgram(unterminated.code);
    std::fill(image.begin() + 0x600, image.end(), 'a');
    ModuleFault(image, "GetModuleHandleA faults when all 512 readable name bytes lack NUL");

    // Keep byte 512 mapped and terminated to distinguish the API's scan cap
    // from an incidental section-boundary read failure.
    image.resize(0xa00, 0);
    Put32(image, 0x178 + 80 + 8, 0x400); Put32(image, 0x178 + 80 + 16, 0x400);
    image[0x800] = 0;
    ModuleFault(image, "GetModuleHandleA rejects NUL at offset 512 even when that byte is readable");
}

void CheckedTrace(const Bytes& image, const char* label) {
    const auto result = Trace(image);
    Check(result.passed, label);
    if (!result.passed) std::cerr << result.report;
}
void ScalarStartupApis() {
    for (const auto& api : {std::pair<const char*, std::uint32_t>{"GetErrorMode", 0},
                           {"GetVersion", 0x4a65000a}, {"GetCurrentProcessId", 1}, {"GetCurrentThreadId", 1},
                           {"GetCurrentProcess", 0xffffffffU}, {"GetCurrentThread", 0xfffffffeU},
                           {"GetACP", 1252}, {"GetOEMCP", 437}}) {
        GuestAssertions guest; PrepareApiState(guest);
        Call(guest.code, VersionIat); // No argument: only the return address belongs to this call.
        CheckApiReturn(guest, api.second, InitialError);
        const auto image = ThirdImportProgram(guest.Finish(), api.first);
        const auto result = Trace(image);
        Check(result.passed, "scalar startup API returns its declared guest value, preserves last-error/registers and pops only its return address");
        if (!result.passed) std::cerr << api.first << ":\n" << result.report;
    }
    // IDs must come from the virtual TEB, rather than returning either a host
    // identity or a hard-coded initial value after guest state changes.
    for (const auto& api : {std::pair<const char*, std::uint32_t>{"GetCurrentProcessId", 0x20}, {"GetCurrentThreadId", 0x24}}) {
        GuestAssertions guest;
        guest.code.insert(guest.code.end(), {0x64, 0xc7, 0x05}); Dword(guest.code, api.second); Dword(guest.code, 0x31415926);
        PrepareApiState(guest); Call(guest.code, VersionIat);
        CheckApiReturn(guest, 0x31415926, InitialError);
        guest.code.insert(guest.code.end(), {0x64, 0xa1}); Dword(guest.code, api.second); guest.Eax(0x31415926);
        CheckedTrace(ThirdImportProgram(guest.Finish(), api.first), "virtual process/thread ID query agrees with the guest TEB ClientId field");
    }
    // A zero-argument gate is valid with exactly four stack bytes available.
    // Restore an ordinary stack before the later argument-taking exit call.
    GuestAssertions boundary;
    boundary.code.push_back(0xbc); Dword(boundary.code, 0x10000004);
    Call(boundary.code, VersionIat); boundary.Eax(0x4a65000a);
    boundary.code.push_back(0xbc); Dword(boundary.code, 0x1000fff0);
    CheckedTrace(ThirdImportProgram(boundary.Finish(), "GetVersion"), "zero-argument gate does not require an unread argument slot beyond its four-byte return frame");
}
Bytes ErrorModeProgram(Bytes code) {
    auto image = ThirdImportProgram(std::move(code), "SetErrorMode");
    // Add GetErrorMode after the original four imports, preserving all their
    // IAT slots. The two API calls share one real guest process state.
    Put32(image, 0x450, 0x2120); Put32(image, 0x454, 0);
    Put32(image, 0x470, 0x2120); Put32(image, 0x474, 0);
    Put32(image, 0x98 + 100 + 12 * 8, 24);
    const std::string name = "GetErrorMode";
    std::copy(name.begin(), name.end(), image.begin() + 0x522);
    return image;
}
void ErrorModeTests() {
    GuestAssertions guest; PrepareApiState(guest);
    const auto set = [&guest](std::uint32_t requested, std::uint32_t previous) {
        Push(guest.code, requested); Call(guest.code, VersionIat); guest.Eax(previous);
    };
    const auto get = [&guest](std::uint32_t expected) {
        Call(guest.code, 0x402070); guest.Eax(expected);
    };
    get(0);
    set(0x8001, 0); get(0x8001);
    set(2, 0x8001); get(2);
    set(4, 2); get(4);
    set(0, 4); get(4); // SEM_NOALIGNMENTFAULTEXCEPT remains set.
    set(0x8000, 4); get(0x8004);
    set(1, 0x8004); get(5);
    set(0x8007, 5); get(0x8007);
    set(0, 0x8007); get(4);
    CheckApiReturn(guest, 4, InitialError);
    const auto image = ErrorModeProgram(guest.Finish());
    CheckedTrace(image, "SetErrorMode returns previous mode, replaces supported flags, keeps alignment flag sticky and shares state with GetErrorMode");
    CheckedTrace(image, "error mode starts at zero in another run after the previous guest changed it");

    for (const auto flags : {8U, 0x10U, 0x80000000U, 0xffffffffU}) {
        GuestAssertions invalid; BeginSingleArgumentCall(invalid, flags); Exit(invalid.code, 0);
        const auto result = Trace(ThirdImportProgram(invalid.code, "SetErrorMode"));
        const bool faulted = !result.passed && Has(result.report, "SetErrorMode flags contain unsupported reserved bits") &&
            Has(result.report, "Guest last error: 0x1234abcd") && Has(result.report, "Guest error mode: 0x0") &&
            Has(result.report, "Guest ExitProcess reached: no");
        Check(faulted, "reserved SetErrorMode flags fault without changing virtual error mode or last-error");
        if (!faulted) std::cerr << result.report;
    }
}
void WideModuleFault(const Bytes& image, const char* label) {
    const auto result = Trace(image);
    const bool faulted = !result.passed && Has(result.report, "Host-call failure: GetModuleHandleW") &&
        Has(result.report, "readable NUL-terminated UTF-16 guest name within 512 code units") &&
        Has(result.report, "Guest last error: 0x1234abcd") && Has(result.report, "Guest ExitProcess reached: no");
    Check(faulted, label);
    if (!faulted) std::cerr << result.report;
}
void ExpandReadOnlySection(Bytes& image, std::uint32_t size) {
    image.resize(0x600 + size, 0);
    Put32(image, 0x178 + 80 + 8, size); Put32(image, 0x178 + 80 + 16, size);
}
void WideModuleTests() {
    ModuleNull(0x400000, "GetModuleHandleW(NULL) returns a readable guest image handle with the ordinary stdcall contract", "GetModuleHandleW");
    ModuleNull(0x5100000, "GetModuleHandleW(NULL) follows another fixed preferred guest base", "GetModuleHandleW");
    ModuleName(Bytes{0, 0}, "GetModuleHandleW empty UTF-16 name returns NULL/error 126", "GetModuleHandleW");
    ModuleName(Bytes{'k', 0, 'e', 0, 'r', 0, 'n', 0, 'e', 0, 'l', 0, '3', 0, '2', 0, 0, 0},
               "GetModuleHandleW validates little-endian code units without exposing named host modules", "GetModuleHandleW");
    ModuleName(Bytes{0, 0xff, 0x34, 0x12, 0, 0xd8, 0, 0},
               "GetModuleHandleW accepts opaque non-ASCII and surrogate code units without modifying the input", "GetModuleHandleW");
    ModuleName(Bytes{'a', 0, 0, 0}, "GetModuleHandleW marshals unaligned UTF-16 guest data", "GetModuleHandleW", 0x403021);

    GuestAssertions terminal; BeginSingleArgumentCall(terminal, 0x4031fe);
    CheckApiReturn(terminal, 0, 126);
    auto image = ThirdImportProgram(terminal.Finish(), "GetModuleHandleW");
    image[0x7fe] = 0; image[0x7ff] = 0;
    CheckedTrace(image, "GetModuleHandleW accepts a two-byte NUL at the final readable word");

    GuestAssertions longest; BeginSingleArgumentCall(longest, 0x403000); CheckApiReturn(longest, 0, 126);
    image = ThirdImportProgram(longest.Finish(), "GetModuleHandleW"); ExpandReadOnlySection(image, 0x400);
    std::fill(image.begin() + 0x600, image.end(), 'x'); image[0x9fe] = 0; image[0x9ff] = 0;
    CheckedTrace(image, "GetModuleHandleW accepts NUL code unit 511 within its 1024-byte readable buffer");
    for (const auto pointer : {0x20000000U, 0xffffffffU, 0xfffffffeU, 0x403200U, 0x4031ffU}) {
        GuestAssertions invalid; BeginSingleArgumentCall(invalid, pointer); Exit(invalid.code, 0);
        image = ThirdImportProgram(invalid.code, "GetModuleHandleW"); image[0x7ff] = 0;
        WideModuleFault(image, "GetModuleHandleW faults on unmapped, wrapping or partial two-byte code units");
    }
    for (const auto lowByte : {0U, static_cast<unsigned>('a')}) {
        GuestAssertions invalid; BeginSingleArgumentCall(invalid, 0x4031fe); Exit(invalid.code, 0);
        image = ThirdImportProgram(invalid.code, "GetModuleHandleW");
        image[0x7fe] = static_cast<std::uint8_t>(lowByte); image[0x7ff] = lowByte == 0 ? 0xff : 0;
        WideModuleFault(image, "GetModuleHandleW does not confuse a zero byte inside a nonzero UTF-16 code unit with a terminator");
    }
    GuestAssertions invalid; BeginSingleArgumentCall(invalid, 0x403000); Exit(invalid.code, 0);
    image = ThirdImportProgram(invalid.code, "GetModuleHandleW"); ExpandReadOnlySection(image, 0x400);
    std::fill(image.begin() + 0x600, image.end(), 'x');
    WideModuleFault(image, "GetModuleHandleW faults after 512 nonzero readable code units");
    ExpandReadOnlySection(image, 0x600); image[0xa00] = 0; image[0xa01] = 0;
    WideModuleFault(image, "GetModuleHandleW rejects NUL code unit 512 even when the bytes are mapped");
}
}
int main() {
    const auto original = xcdm::samples::X86Win32PeBytes();
    const auto valid = Trace(original);
    Check(valid.passed && Has(valid.report, "Guest exit code: 0"), "known fixture also executes through startup profile");
    auto virtualWx = original;
    Put32(virtualWx, 0x178 + 36, 0xe0000020);
    Check(Trace(virtualWx).passed, "guest W+X flags are accepted only as interpreted byte data");
    Check(!xcdm::runtime::ExecuteWin32IntegerImage(virtualWx.data(), virtualWx.size()).passed,
          "strict fixture mapping still rejects guest W+X flags");
    Check(Has(valid.report, "bounded supplied-image startup trace"), "trace identifies supplied-image scope");
    auto unknown = original;
    unknown[0x4b2] = 'X'; // GetLastError -> XetLastError, first unsupported call reached.
    const auto stopped = Trace(unknown);
    Check(!stopped.passed && Has(stopped.report, "Missing Win32 API: KERNEL32.dll!XetLastError"), "unknown called API stops at its own gate");
    Check(Has(stopped.report, "SetLastError") && Has(stopped.report, "Guest last error: 0x1234"), "startup executes preceding supported code before stopping");
    Check(!xcdm::runtime::ExecuteWin32IntegerImage(unknown.data(), unknown.size()).passed, "strict fixture profile still rejects unresolved imports");
    Bytes code; Exit(code, 0);
    auto unused = Program(code);
    unused[0x4c2] = 'X'; // Uncalled OutputDebugStringA replacement.
    Check(Trace(unused).passed, "an uncalled missing import does not prevent bounded startup tracing");
    Check(!Trace(original, 1).passed && Has(Trace(original, 1).report, "instruction budget exhausted"), "startup enforces supplied instruction budget");
    Check(!Trace(original, 0).passed && !Trace(original, 1000001).passed, "startup rejects invalid budgets");

    code = {0x64, 0xc7, 0x05, 0x34, 0x00, 0x00, 0x00}; Dword(code, 0x12345678); // mov fs:[LastErrorValue], imm32
    Call(code, 0x402064); CompareAndExit(code, 0x12345678);
    Check(Trace(Program(code)).passed, "GetLastError observes emulated TEB writes by guest code");
    code = {0x68}; Dword(code, 0x89abcdef); Call(code, 0x402060);
    code.insert(code.end(), {0x64, 0xa1, 0x34, 0x00, 0x00, 0x00}); CompareAndExit(code, 0x89abcdef);
    Check(Trace(Program(code)).passed, "SetLastError is visible at the guest TEB offset");
    code = {0x64, 0xa1, 0x30, 0x00, 0x00, 0x00, 0x8b, 0x40, 0x08}; // eax=fs:[PEB]->ImageBase
    CompareAndExit(code, 0x400000);
    Check(Trace(Program(code)).passed, "virtual PEB points to the mapped guest image base");
    code = {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00}; CompareAndExit(code, 0xffffffff);
    Check(Trace(Program(code)).passed, "initial x86 SEH chain contains the terminal sentinel");
    code = {0xd9, 0xe8}; Exit(code, 0); // fld1 must not silently pass through.
    Check(!Trace(Program(code)).passed, "unsupported floating-point startup cannot report successful execution");
    code = {0xb8}; Dword(code, 0x80000000); code.insert(code.end(), {0x99, 0x89, 0xd0}); // cdq; mov eax,edx
    CompareAndExit(code, 0xffffffff);
    Check(Trace(Program(code)).passed, "CDQ preserves negative 32-bit sign extension under MSVC SDL rules");
    code = {0xb8}; Dword(code, 0x80000000); code.insert(code.end(), {0xf7, 0xd8}); // neg eax
    CompareAndExit(code, 0x80000000);
    Check(Trace(Program(code)).passed, "NEG uses modulo-32-bit subtraction for the minimum signed operand");
    const auto bad = xcdm::runtime::TraceWin32Startup32(nullptr, 100);
    Check(!bad.passed && Has(bad.report, "Loader stopped"), "startup refuses invalid file input with a useful report");
    GetVersionExTests();
    GetModuleHandleTests();
    ScalarStartupApis();
    ErrorModeTests();
    WideModuleTests();
    std::cout << checks << " bounded Win32 startup checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
