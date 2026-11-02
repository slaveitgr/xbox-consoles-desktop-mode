#include "runtime/Win32Probe32.h"
#include "samples/X86Win32Fixture.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
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
}
int main() {
    const auto original = xcdm::samples::X86Win32PeBytes();
    const auto valid = Trace(original);
    Check(valid.passed && Has(valid.report, "Guest exit code: 0"), "known fixture also executes through startup profile");
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
    std::cout << checks << " bounded Win32 startup checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
