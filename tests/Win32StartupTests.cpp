#include "runtime/Win32Probe32.h"
#include "samples/X86Win32Fixture.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
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
void Exit(Bytes& bytes, std::uint8_t code) { bytes.insert(bytes.end(), {0x6a, code}); Call(bytes, 0x40206c); bytes.insert(bytes.end(), {0xeb, 0xfe}); }
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
constexpr std::uint32_t SavedEsp = 0x402100;
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
    Bytes Finish() {
        Exit(code, 0);
        const auto failure = code.size();
        Exit(code, 1);
        for (auto at : failures) Put32(code, at, static_cast<std::uint32_t>(failure - (at + 4)));
        return code;
    }
};

void Push(Bytes& code, std::uint32_t value) { code.push_back(0x68); Dword(code, value); }
void BeginVersionCall(GuestAssertions& guest, std::uint32_t pointer) {
    Push(guest.code, InitialError); Call(guest.code, 0x402060);
    for (const auto reg : {3U, 5U, 6U, 7U}) {
        guest.code.push_back(static_cast<std::uint8_t>(0xb8U + reg));
        Dword(guest.code, 0x11223300U + reg);
    }
    guest.code.insert(guest.code.end(), {0x89, 0x25}); Dword(guest.code, SavedEsp); // mov [saved],esp
    Push(guest.code, pointer); Call(guest.code, VersionIat);
}
void CheckVersionReturn(GuestAssertions& guest, bool success, std::uint32_t error) {
    guest.Eax(success ? 1 : 0);
    guest.code.insert(guest.code.end(), {0x3b, 0x25}); Dword(guest.code, SavedEsp); // cmp esp,[saved]
    guest.Equal();
    for (const auto reg : {3U, 5U, 6U, 7U}) guest.Register(reg, 0x11223300U + reg);
    Call(guest.code, 0x402064); guest.Eax(error);
}
Bytes VersionProgram(Bytes code) {
    auto image = Program(std::move(code));
    const std::string name = "GetVersionExA";
    std::fill(image.begin() + 0x4c2, image.begin() + 0x4e0, 0);
    std::copy(name.begin(), name.end(), image.begin() + 0x4c2);
    return image;
}
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
    GuestAssertions guest; BeginVersionCall(guest, pointer);
    CheckVersionReturn(guest, true, InitialError);
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
    GuestAssertions guest; BeginVersionCall(guest, VersionBuffer);
    CheckVersionReturn(guest, false, 122); // ERROR_INSUFFICIENT_BUFFER, virtual profile policy
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
    GuestAssertions shortSize; BeginVersionCall(shortSize, 0x4031fc);
    CheckVersionReturn(shortSize, false, 122);
    shortSize.code.push_back(0xa1); Dword(shortSize.code, 0x4031fc); shortSize.Eax(147);
    auto image = VersionProgram(shortSize.Finish()); Put32(image, 0x7fc, 147);
    Check(Trace(image).passed, "GetVersionExA wrong-size failure needs only four readable bytes and performs no write");

    for (const auto pointer : {0U, 0x20000000U, 0xfffffffeU, 0x4031feU}) {
        GuestAssertions guest; BeginVersionCall(guest, pointer); Exit(guest.code, 0);
        VersionFault(VersionProgram(guest.code), "GetVersionExA faults on null, unmapped or wrapping size-field address");
    }
    for (const auto pointer : {0x403020U, 0x400040U, 0x4021fcU}) {
        GuestAssertions guest; BeginVersionCall(guest, pointer); Exit(guest.code, 0);
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
    std::cout << checks << " bounded Win32 startup checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
