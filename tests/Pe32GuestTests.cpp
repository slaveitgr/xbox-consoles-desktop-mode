#include "runtime/Pe32Image.h"
#include "runtime/Win32Probe32.h"
#include "samples/X86Win32Fixture.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t Optional = 0x98;
constexpr std::size_t Sections = Optional + 224;
constexpr std::size_t TextFile = 0x200, ImportFile = 0x400, MessageFile = 0x600;
constexpr std::uint32_t Base = 0x400000, StackBase = 0x10000000, StackSize = 0x10000;
constexpr std::uint32_t ExitIat = Base + 0x206c;
int failures = 0;
std::size_t checks = 0;

void Check(bool condition, const std::string& label) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
bool Has(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}
void Put16(Bytes& bytes, std::size_t at, std::uint16_t value) {
    for (unsigned n = 0; n < 2; ++n) bytes.at(at + n) = static_cast<std::uint8_t>(value >> (8 * n));
}
void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) bytes.at(at + n) = static_cast<std::uint8_t>(value >> (8 * n));
}
void Append32(Bytes& bytes, std::uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * n)));
}
Bytes Fixture() { return xcdm::samples::X86Win32PeBytes(); }
auto Load(const Bytes& bytes) { return xcdm::runtime::LoadPe32Image(bytes.data(), bytes.size()); }
auto Run(const Bytes& bytes, std::uint64_t budget = 100000) {
    return xcdm::runtime::ExecuteWin32IntegerImage(bytes.data(), bytes.size(), budget);
}
void Directory(Bytes& bytes, unsigned index, std::uint32_t rva, std::uint32_t size) {
    Put32(bytes, Optional + 96 + index * 8, rva);
    Put32(bytes, Optional + 100 + index * 8, size);
}
void Rejects(const Bytes& bytes, const std::string& label) {
    const auto result = Load(bytes);
    Check(!result.ok, label + " rejected by loader");
    Check(!result.message.empty(), label + " includes a diagnostic");
    Check(result.image.bytes.empty(), label + " does not expose a partial mapped image");
}
void AppendExit(Bytes& code, std::uint8_t value = 0) {
    code.insert(code.end(), {0x6a, value, 0xff, 0x15}); // push exit code; call [ExitProcess]
    Append32(code, ExitIat);
    code.insert(code.end(), {0xeb, 0xfe}); // Incorrectly returning ExitProcess must not pass.
}
Bytes Program(const Bytes& code) {
    auto bytes = Fixture();
    std::fill(bytes.begin() + TextFile, bytes.begin() + ImportFile, 0xcc);
    std::copy(code.begin(), code.end(), bytes.begin() + TextFile);
    Put32(bytes, Sections + 8, static_cast<std::uint32_t>(code.size()));
    return bytes;
}

void MappingContract() {
    const auto original = Fixture();
    const auto result = Load(original);
    Check(result.ok, "self-authored PE32 fixture maps");
    if (!result.ok) return;
    const auto& image = result.image;
    Check(image.base == Base && image.entry == Base + 0x1000, "guest virtual base and entry preserved");
    Check(image.bytes.size() == 0x4000 && image.headerSize == 0x200, "bounded virtual image and headers mapped");
    Check(image.sections.size() == 3, "three independently protected sections retained");
    Check(std::equal(original.begin(), original.begin() + 0x200, image.bytes.begin()), "DOS/PE headers copied");
    Check(std::equal(original.begin() + TextFile, original.begin() + ImportFile,
                     image.bytes.begin() + 0x1000), "code copied at RVA rather than raw offset");
    Check(std::all_of(image.bytes.begin() + 0x1200, image.bytes.begin() + 0x2000,
                      [](std::uint8_t value) { return value == 0; }), "unbacked image gap remains zero");
    if (image.sections.size() == 3) {
        Check(image.sections[0].readable && image.sections[0].executable && !image.sections[0].writable,
              "code is readable/executable and not writable");
        Check(image.sections[1].readable && image.sections[1].writable && !image.sections[1].executable,
              "IAT section is readable/writable and not executable");
        Check(image.sections[2].readable && !image.sections[2].writable && !image.sections[2].executable,
              "message section is read-only");
    }
    Check(image.imports.size() == 4, "four explicit imports collected");
    const std::vector<std::string> expected{"SetLastError", "GetLastError", "OutputDebugStringA", "ExitProcess"};
    if (image.imports.size() == expected.size()) {
        for (std::size_t n = 0; n < expected.size(); ++n) {
            const auto& item = image.imports[n];
            Check(item.library == "KERNEL32.dll" && item.name == expected[n] && !item.byOrdinal,
                  "import identity " + expected[n]);
            Check(item.iatRva == 0x2060 + n * 4, "PE32 IAT uses four-byte slots");
        }
    }
    auto tail = Fixture();
    Put32(tail, Sections + 2 * 40 + 8, 0x800);
    const auto expanded = Load(tail);
    Check(expanded.ok, "virtual section tail larger than raw file data maps");
    if (expanded.ok)
        Check(std::all_of(expanded.image.bytes.begin() + 0x3200, expanded.image.bytes.begin() + 0x3800,
                          [](std::uint8_t value) { return value == 0; }), "virtual tail is zero-filled");
    auto zeroRaw = Fixture();
    Put32(zeroRaw, Sections + 2 * 40 + 16, 0);
    Put32(zeroRaw, Sections + 2 * 40 + 20, 0);
    const auto bss = Load(zeroRaw);
    Check(bss.ok && bss.image.bytes[0x3000] == 0, "a section without raw bytes is zero-initialized");
    auto fallback = Fixture();
    Put32(fallback, ImportFile, 0);
    Check(Load(fallback).ok, "unbound IAT doubles as lookup table when OriginalFirstThunk is absent");
}

void RejectedImages() {
    Check(!xcdm::runtime::LoadPe32Image(nullptr, 2048).ok, "null file pointer rejected");
    const auto original = Fixture();
    Check(!xcdm::runtime::LoadPe32Image(original.data(), 64U * 1024U * 1024U + 1).ok,
          "oversized advertised input rejected before reading beyond the supplied fixture");
    bool allTruncatedRejected = true;
    for (std::size_t size = 0; size < original.size(); ++size) {
        if (xcdm::runtime::LoadPe32Image(original.data(), size).ok) {
            allTruncatedRejected = false;
            std::cerr << "Accepted truncated PE32 length " << size << '\n';
            break;
        }
    }
    Check(allTruncatedRejected, "every truncation of required headers or section data is rejected");

    auto bytes = Fixture(); bytes[0] = 0; Rejects(bytes, "invalid DOS signature");
    bytes = Fixture(); Put32(bytes, 0x3c, 0xfffffff0U); Rejects(bytes, "overflowing PE header offset");
    bytes = Fixture(); Put16(bytes, 0x84, 0x8664); Rejects(bytes, "AMD64 machine in PE32 profile");
    bytes = Fixture(); Put16(bytes, Optional, 0x20b); Rejects(bytes, "PE32+ header in x86 profile");
    bytes = Fixture(); Put16(bytes, 0x94, 95); Rejects(bytes, "optional header too short");
    bytes = Fixture(); Put16(bytes, 0x96, 0x2103); Rejects(bytes, "DLL image");
    bytes = Fixture(); Put32(bytes, Optional + 92, 17); Rejects(bytes, "directory array exceeds optional header");
    bytes = Fixture(); Put32(bytes, Optional + 56, 0xfffff000U); Rejects(bytes, "unbounded virtual image allocation");
    bytes = Fixture(); Put32(bytes, Optional + 28, 0xffff0000U); Rejects(bytes, "guest virtual address overflow");
    bytes = Fixture(); Put32(bytes, Optional + 16, 0x3000); Rejects(bytes, "entry in non-executable data");
    bytes = Fixture(); Put32(bytes, Sections + 36, 0xe0000020); Rejects(bytes, "writable/executable code section");
    bytes = Fixture(); Put32(bytes, Sections + 40 + 12, 0x1000); Rejects(bytes, "overlapping virtual sections");
    bytes = Fixture(); Put32(bytes, Sections + 80 + 20, 0x400); Rejects(bytes, "overlapping raw sections");
    bytes = Fixture(); Put32(bytes, Sections + 20, 0xfffffe00U); Rejects(bytes, "raw file offset outside input");
    bytes = Fixture(); Put32(bytes, Sections + 8, 0xffffffffU); Rejects(bytes, "section virtual range overflow");
    for (unsigned index : {5U, 9U, 13U, 14U}) {
        bytes = Fixture(); Directory(bytes, index, 0x2000, 32);
        Rejects(bytes, "unsupported execution-affecting directory " + std::to_string(index));
    }
}

void ImportBoundaries() {
    auto bytes = Fixture();
    Put32(bytes, ImportFile + 16, 0x3000);
    Rejects(bytes, "IAT in read-only data");
    bytes = Fixture(); Put32(bytes, ImportFile + 16, 0x21fe);
    Rejects(bytes, "IAT entry crosses section end");
    bytes = Fixture(); Put32(bytes, ImportFile, 0xfffffffcU);
    Rejects(bytes, "lookup RVA near integer wraparound");
    bytes = Fixture(); Put32(bytes, ImportFile + 12, 0x2200);
    Rejects(bytes, "DLL name points into an unbacked gap");
    bytes = Fixture(); bytes[ImportFile + 0x80] = 0;
    Rejects(bytes, "empty DLL name");
    bytes = Fixture(); Put32(bytes, ImportFile + 0x40, 0x21ff);
    Rejects(bytes, "import hint crosses section end");
    bytes = Fixture(); Put32(bytes, ImportFile + 0x40, 0x21ee);
    std::fill(bytes.begin() + ImportFile + 0x1f0, bytes.begin() + MessageFile, 'A');
    Rejects(bytes, "import name has no terminator before section end");
    bytes = Fixture(); bytes[ImportFile + 0xa2] = 0x1b;
    Rejects(bytes, "control character in import name");
    bytes = Fixture(); Directory(bytes, 1, 0x2000, 20);
    Rejects(bytes, "import descriptor directory lacks null terminator");
    bytes = Fixture(); Put32(bytes, ImportFile, 0x21f0);
    for (unsigned n = 0; n < 4; ++n) Put32(bytes, ImportFile + 0x1f0 + n * 4, 0x80000007);
    Rejects(bytes, "lookup table has no terminator before section end");
    bytes = Fixture();
    std::copy_n(bytes.begin() + ImportFile, 20, bytes.begin() + ImportFile + 20);
    Directory(bytes, 1, 0x2000, 60);
    Rejects(bytes, "two descriptors bind the same IAT slots");
    bytes = Fixture(); Put32(bytes, ImportFile + 0x40, 0x80000007U);
    const auto ordinal = Load(bytes);
    Check(ordinal.ok && ordinal.image.imports.size() == 4 && ordinal.image.imports[0].byOrdinal &&
          ordinal.image.imports[0].ordinal == 7 && ordinal.image.imports[0].name.empty(),
          "valid ordinal import retains its identity without fabricating a name");
    Check(!Run(bytes).passed, "ordinal import is rejected by the narrower host-call profile");
    bytes = Fixture(); Put32(bytes, ImportFile + 0x40, 0x80010007U);
    Rejects(bytes, "ordinal import contains reserved high bits");
}

void SuccessfulAndFailedCalls() {
    const auto expected = xcdm::runtime::RunWin32Probe32();
    Check(expected.passed, "embedded x86/Win32 smoke probe passes");
    if (!expected.passed) std::cerr << expected.report;
    Check(Has(expected.report, "Guest exit code: 0") && Has(expected.report, "Guest last error: 0x1234") &&
          Has(expected.report, "Debug output: x86 Win32 host calls passed"),
          "smoke probe confirms state, string marshalling and guest ExitProcess");
    auto bytes = Fixture();
    const auto baseline = Run(bytes);
    Check(baseline.passed, "public bounded guest runner executes valid fixture");
    bytes[0x212] = 0x35; // cmp eax,0x1235 while SetLastError still receives 0x1234.
    const auto mismatch = Run(bytes);
    Check(!mismatch.passed && Has(mismatch.report, "Guest exit code: 1") &&
          !Has(mismatch.report, "x86 Win32 host calls passed"), "conditional mismatch takes ExitProcess(1) branch");
    const auto exhausted = Run(Fixture(), 1);
    Check(!exhausted.passed && Has(exhausted.report, "Guest ExitProcess reached: no"),
          "small instruction budget cannot accidentally report a successful exit");
    for (const auto limit : {std::uint64_t{0}, std::uint64_t{1000001}, std::numeric_limits<std::uint64_t>::max()}) {
        const auto invalid = Run(Fixture(), limit);
        Check(!invalid.passed && Has(invalid.summary, "Invalid x86 instruction budget"), "invalid budget fails before execution");
    }
    bytes = Fixture(); bytes[ImportFile + 0xb2] = 'X';
    const auto unsupported = Run(bytes);
    Check(!unsupported.passed && Has(unsupported.summary, "execution was not started") &&
          Has(unsupported.report, "Unresolved import"), "unknown named import stops before guest execution");
    bytes = Fixture(); bytes[ImportFile + 0x80] = 'X';
    Check(!Run(bytes).passed, "same symbol in an unapproved DLL cannot bind by name alone");
    bytes = Fixture(); Put32(bytes, Optional + 28, StackBase);
    const auto collision = Run(bytes);
    Check(!collision.passed && Has(collision.summary, "overlaps the reserved guest stack"),
          "image/stack collision is rejected before binding or executing guest addresses");
    bytes = Fixture();
    std::transform(bytes.begin() + ImportFile + 0x80, bytes.begin() + ImportFile + 0x8c,
                   bytes.begin() + ImportFile + 0x80,
                   [](std::uint8_t c) { return static_cast<std::uint8_t>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c); });
    Check(Run(bytes).passed, "approved DLL identity is case-insensitive");

    // Locate the message push by its encoding instead of coupling the test to
    // the exact instruction offset chosen by the fixture generator.
    bytes = Fixture();
    const Bytes pushMessage{0x68, 0x00, 0x30, 0x40, 0x00};
    const auto push = std::search(bytes.begin() + TextFile, bytes.begin() + ImportFile,
                                  pushMessage.begin(), pushMessage.end());
    Check(push != bytes.begin() + ImportFile, "message-pointer instruction located");
    if (push != bytes.begin() + ImportFile) {
        const auto operand = static_cast<std::size_t>(push - bytes.begin()) + 1;
        for (std::uint32_t pointer : {0U, 0xfffffff0U, Base + 0x2200}) {
            auto invalid = bytes; Put32(invalid, operand, pointer);
            const auto stopped = Run(invalid);
            Check(!stopped.passed && Has(stopped.report, "readable bounded ASCII guest string"),
                  "unmapped debug string pointer fails in host marshalling");
        }
    }
    bytes = Fixture(); std::fill(bytes.begin() + MessageFile, bytes.end(), 'A');
    Check(!Run(bytes).passed, "unterminated debug text cannot read beyond its 512-byte bound");
    bytes = Fixture(); bytes[MessageFile] = 0x1b;
    Check(!Run(bytes).passed, "unsupported debug text does not pass as an empty success");
}

void RuntimeMemoryPermissions() {
    // A positive RW control makes these tests distinguish access enforcement
    // from an engine that simply does not implement MOV r/m32,imm32.
    for (const auto address : {Base + 0x2100, Base + 0x3000, Base + 0x1000, Base}) {
        Bytes code{0xc7, 0x05}; // mov dword ptr [absolute address],42
        Append32(code, address); Append32(code, 42); AppendExit(code);
        const auto result = Run(Program(code));
        Check(result.passed == (address == Base + 0x2100),
              "guest stores require writable memory at " + std::to_string(address));
    }
    for (const auto stack : {StackBase + 8, StackBase + 2}) {
        Bytes code{0xbc}; // mov esp, imm32
        Append32(code, stack); AppendExit(code);
        Check(Run(Program(code)).passed == (stack == StackBase + 8),
              "stack writes enforce the lower mapped boundary");
    }
    Bytes code{0xbc}; Append32(code, StackBase + StackSize - 4);
    code.insert(code.end(), {0xff, 0x25}); Append32(code, Base + 0x2060); // jmp [SetLastError]
    const auto badCallStack = Run(Program(code));
    Check(!badCallStack.passed && Has(badCallStack.report, "stdcall stack is outside guest memory"),
          "host call validates the argument slot as well as the return slot");
    code = {0x68}; Append32(code, 0x1234); // push argument
    code.insert(code.end(), {0x6a, 0x00, 0xff, 0x25}); Append32(code, Base + 0x2060);
    const auto badReturn = Run(Program(code));
    Check(!badReturn.passed && Has(badReturn.report, "return address is outside executable guest memory"),
          "host call cannot return to a null or data address");
    code = {0xb8}; Append32(code, Base + 0x3000); code.insert(code.end(), {0xff, 0xe0});
    auto nx = Program(code);
    Bytes dataCode; AppendExit(dataCode);
    std::copy(dataCode.begin(), dataCode.end(), nx.begin() + MessageFile);
    Check(!Run(nx).passed, "valid instructions in a non-executable data section cannot run");
    Put32(nx, Sections + 2 * 40 + 36, 0x60000040);
    Check(Run(nx).passed, "same instructions execute when the section explicitly grants execute access");
    code = {0xa1}; Append32(code, Base + 0x2060); // mov eax,[SetLastError IAT]
    code.insert(code.end(), {0x40, 0xff, 0xe0}); // inc eax; jmp eax (one byte inside a host gate)
    const auto invalidGate = Run(Program(code));
    Check(!invalidGate.passed && Has(invalidGate.report, "Invalid Win32 guest import gate"),
          "a guest cannot enter the middle of a host-call gate");
    code = {0xe6, 0x80}; AppendExit(code); // out 0x80,al followed by an otherwise valid success.
    Check(!Run(Program(code)).passed, "guest port I/O cannot be silently accepted");
    Check(!Run(Program(Bytes{0xf4})).passed, "HLT without guest ExitProcess is not a successful run");
}
} // namespace

int main() {
    MappingContract();
    RejectedImages();
    ImportBoundaries();
    SuccessfulAndFailedCalls();
    RuntimeMemoryPermissions();
    std::cout << checks << " PE32 loader and Win32 execution checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
