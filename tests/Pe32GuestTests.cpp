#include "runtime/Pe32Image.h"
#include "runtime/Win32Probe32.h"
#include "samples/X86Win32Fixture.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
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
auto LoadStartup(const Bytes& bytes) {
    return xcdm::runtime::LoadPe32Image(bytes.data(), bytes.size(), xcdm::runtime::Pe32Profile::StartupTrace);
}
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
void RejectsStartup(const Bytes& bytes, const std::string& label) {
    const auto result = LoadStartup(bytes);
    Check(!result.ok, label + " rejected by startup loader");
    Check(!result.message.empty() && result.image.bytes.empty(), label + " fails with a diagnostic and no partial image");
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

Bytes DelayFixture(bool legacyVa = false) {
    auto bytes = Fixture();
    const auto address = [legacyVa](std::uint32_t rva) { return legacyVa ? Base + rva : rva; };
    Directory(bytes, 13, 0x2100, 64);
    const auto descriptor = ImportFile + 0x100;
    Put32(bytes, descriptor, legacyVa ? 0 : 1);
    Put32(bytes, descriptor + 4, address(0x2190));
    Put32(bytes, descriptor + 8, address(0x2180));
    Put32(bytes, descriptor + 12, address(0x2160));
    Put32(bytes, descriptor + 16, address(0x2140));
    Put32(bytes, ImportFile + 0x140, address(0x21a0));
    Put32(bytes, ImportFile + 0x144, 0x80000007U);
    // Delay IAT values are linker helper VAs, not copies of the name table.
    Put32(bytes, ImportFile + 0x160, Base + 0x1000);
    Put32(bytes, ImportFile + 0x164, Base + 0x1000);
    const std::string library = "D3D9.dll", symbol = "Direct3DCreate9";
    std::copy(library.begin(), library.end(), bytes.begin() + ImportFile + 0x190);
    std::copy(symbol.begin(), symbol.end(), bytes.begin() + ImportFile + 0x1a2);
    return bytes;
}

std::size_t ImportRaw(std::uint32_t rva) { return ImportFile + rva - 0x2000; }
Bytes LargeImportImage() {
    auto bytes = Fixture();
    bytes.resize(ImportFile + 0x18000 + 0x200, 0);
    std::fill(bytes.begin() + ImportFile, bytes.end(), 0);
    Put32(bytes, Sections + 40 + 8, 0x18000);
    Put32(bytes, Sections + 40 + 16, 0x18000);
    Put32(bytes, Sections + 80 + 12, 0x30000);
    Put32(bytes, Sections + 80 + 20, ImportFile + 0x18000);
    Put32(bytes, Optional + 56, 0x40000);
    Directory(bytes, 1, 0, 0); Directory(bytes, 12, 0, 0);
    const std::string library = "KERNEL32.dll";
    std::copy(library.begin(), library.end(), bytes.begin() + ImportRaw(0x2100));
    return bytes;
}

void StartupDelayImports() {
    const auto plain = LoadStartup(Fixture());
    Check(plain.ok && plain.image.imports.size() == 4 && !plain.image.imports[0].delayed,
          "startup profile preserves normal import identities");
    for (const bool legacy : {false, true}) {
        const auto bytes = DelayFixture(legacy);
        Check(!Load(bytes).ok, "fixture profile still rejects delay imports");
        const auto result = LoadStartup(bytes);
        Check(result.ok && result.image.imports.size() == 6, "startup profile collects normal plus RVA/VA delay imports");
        if (result.ok && result.image.imports.size() == 6) {
            const auto& named = result.image.imports[4];
            const auto& ordinal = result.image.imports[5];
            Check(named.delayed && named.library == "D3D9.dll" && named.name == "Direct3DCreate9" &&
                  !named.byOrdinal && named.iatRva == 0x2160, "delay name and PE32 IAT RVA normalized");
            Check(ordinal.delayed && ordinal.byOrdinal && ordinal.ordinal == 7 && ordinal.name.empty(),
                  "delay ordinal flag is not mistaken for a legacy VA");
            Check(std::equal(bytes.begin() + ImportFile + 0x160, bytes.begin() + ImportFile + 0x16c,
                             result.image.bytes.begin() + 0x2160), "loader preserves linker delay helper addresses and terminator");
            Check(std::all_of(result.image.imports.begin(), result.image.imports.begin() + 4,
                              [](const auto& imported) { return !imported.delayed; }), "normal imports are not marked delayed");
        }
    }
    auto bytes = DelayFixture();
    Directory(bytes, 1, 0, 0);
    Check(LoadStartup(bytes).ok && LoadStartup(bytes).image.imports.size() == 2,
          "delay imports are inspected even when the normal directory is absent");
    bytes = DelayFixture();
    std::fill(bytes.begin() + ImportFile + 0x100, bytes.begin() + ImportFile + 0x140, 0);
    Check(LoadStartup(bytes).ok, "an empty bounded delay descriptor table is accepted");
    bytes = DelayFixture(); Directory(bytes, 13, 0x2100, 31);
    RejectsStartup(bytes, "truncated delay descriptor");
    bytes = DelayFixture(); Directory(bytes, 13, 0x2100, 32);
    RejectsStartup(bytes, "delay descriptors lack null terminator");
    bytes = DelayFixture(); Directory(bytes, 13, 0x21f0, 64);
    RejectsStartup(bytes, "delay descriptor directory crosses section boundary");
    bytes = DelayFixture(); Directory(bytes, 13, 0x2010, 64);
    RejectsStartup(bytes, "normal and delay descriptor directories overlap");
    bytes = DelayFixture(); Put32(bytes, ImportFile + 0x100, 2);
    RejectsStartup(bytes, "reserved delay descriptor attribute bits");
    for (const auto field : {4U, 12U, 16U}) {
        bytes = DelayFixture(); Put32(bytes, ImportFile + 0x100 + field, 0);
        RejectsStartup(bytes, "missing required delay pointer " + std::to_string(field));
    }
    for (const auto field : {4U, 8U, 12U, 16U, 20U, 24U}) {
        bytes = DelayFixture(true); Put32(bytes, ImportFile + 0x100 + field, Base - 4);
        RejectsStartup(bytes, "legacy delay pointer below image base " + std::to_string(field));
    }
    bytes = DelayFixture(true); Put32(bytes, ImportFile + 0x140, Base - 4);
    RejectsStartup(bytes, "legacy delay import-by-name VA below image base");
    bytes = DelayFixture(); Put32(bytes, ImportFile + 0x140, 0x7ffffffeU);
    RejectsStartup(bytes, "delay import-by-name hint outside guest image");
    bytes = DelayFixture(); Put32(bytes, ImportFile + 0x144, 0x80010007U);
    RejectsStartup(bytes, "delay ordinal contains reserved high bits");
    bytes = DelayFixture(); Put32(bytes, ImportFile + 0x110, 0x21f8);
    Put32(bytes, ImportFile + 0x1f8, 0x80000001); Put32(bytes, ImportFile + 0x1fc, 0x80000002);
    RejectsStartup(bytes, "delay lookup lacks terminator before section end");
    bytes = DelayFixture(); Put32(bytes, ImportFile + 0x114, 0x21fc);
    RejectsStartup(bytes, "delay ancillary table is too short for its import count and terminator");
    bytes = DelayFixture(); Put32(bytes, ImportFile + 0x108, 0x3000);
    RejectsStartup(bytes, "delay module handle pointer is not writable");
    bytes = DelayFixture(); Put32(bytes, ImportFile + 0x10c, 0x3000);
    const auto readOnlyDelay = LoadStartup(bytes);
    Check(readOnlyDelay.ok && readOnlyDelay.image.imports[4].iatRva == 0x3000 &&
          !readOnlyDelay.image.sections[2].writable,
          "startup permits loader initialization of a readable delay IAT without broadening its runtime permissions");
    for (std::uint32_t iat : {0x2060U, 0x2061U, 0x2070U}) {
        bytes = DelayFixture(); Put32(bytes, ImportFile + 0x10c, iat);
        RejectsStartup(bytes, "delay IAT overlaps normal slot, partial slot or terminator " + std::to_string(iat));
    }
    bytes = DelayFixture(); Put32(bytes, ImportFile + 0x140, 0x21ee);
    std::fill(bytes.begin() + ImportFile + 0x1f0, bytes.begin() + MessageFile, 'A');
    RejectsStartup(bytes, "delay symbol name is unterminated at section end");
    bytes = DelayFixture();
    Put32(bytes, ImportFile + 0x114, 0x21c0); Put32(bytes, ImportFile + 0x118, 0x21d0);
    Check(LoadStartup(bytes).ok, "bounded ancillary delay tables may be declared without being executed");
    for (unsigned index : {5U, 9U, 14U}) {
        bytes = DelayFixture(); Directory(bytes, index, 0x2000, 32);
        RejectsStartup(bytes, "startup profile still rejects execution-affecting directory " + std::to_string(index));
    }
    bytes = DelayFixture(); Put32(bytes, Sections + 36, 0xe0000020U);
    const auto writableCode = LoadStartup(bytes);
    Check(writableCode.ok, "startup profile accepts interpreted writable/executable guest bytes");
    Check(writableCode.ok && writableCode.image.sections.front().writable &&
          writableCode.image.sections.front().executable,
          "startup preserves guest permissions without native executable allocation");
    Check(!xcdm::runtime::LoadPe32Image(bytes.data(), bytes.size(), static_cast<xcdm::runtime::Pe32Profile>(99)).ok,
          "unrecognized profile cannot broaden loader behavior");
}

void ReadOnlyStartupIats() {
    auto readOnly = Fixture();
    Put32(readOnly, Sections + 40 + 36, 0x40000040); // .idata readable, no guest writes.
    const auto strict = Load(readOnly);
    Check(!strict.ok && Has(strict.message, "lookup source RVA 0x2040") &&
          Has(strict.message, "IAT target RVA 0x2060") && Has(strict.message, "requires writable"),
          "strict fixture retains writable-IAT requirement with exact source/target diagnostics");
    const auto startup = LoadStartup(readOnly);
    Check(startup.ok && startup.image.imports.size() == 4 && !startup.image.sections[1].writable,
          "startup accepts an IAT in a read-only guest section and preserves read-only metadata");
    if (startup.ok) {
        Check(std::equal(readOnly.begin() + ImportFile + 0x60, readOnly.begin() + ImportFile + 0x74,
                         startup.image.bytes.begin() + 0x2060),
              "loader inspection preserves original read-only IAT bytes and terminator");
    }
    const auto executed = xcdm::runtime::TraceWin32Startup32(readOnly.data(), readOnly.size());
    Check(executed.passed && Has(executed.report, "x86 Win32 host calls passed"),
          "loader-owned gate initialization lets the real four-call fixture execute with a read-only IAT");

    auto fallback = readOnly;
    Put32(fallback, ImportFile, 0); // OriginalFirstThunk absent: read the IAT itself.
    Check(LoadStartup(fallback).ok &&
          xcdm::runtime::TraceWin32Startup32(fallback.data(), fallback.size()).passed,
          "read-only IAT can also supply bounded lookup entries before initial binding");

    Bytes code{0xc7, 0x05};
    Append32(code, Base + 0x2060); Append32(code, 42); AppendExit(code);
    auto write = Program(code);
    Check(xcdm::runtime::TraceWin32Startup32(write.data(), write.size()).passed,
          "positive writable-IAT control executes the same guest MOV instruction");
    Put32(write, Sections + 40 + 36, 0x40000040);
    const auto refused = xcdm::runtime::TraceWin32Startup32(write.data(), write.size());
    Check(!refused.passed && Has(refused.report, "Guest data memory access was refused"),
          "guest stores still fault on the read-only IAT after privileged initial gate binding");

    auto invalid = readOnly;
    Put32(invalid, ImportFile, 0x2200);
    const auto badSource = LoadStartup(invalid);
    Check(!badSource.ok && badSource.image.bytes.empty() && Has(badSource.message, "lookup source RVA 0x2200") &&
          Has(badSource.message, "IAT target RVA 0x2060") && Has(badSource.message, "lookup requires a readable"),
          "unmapped lookup diagnostic identifies the source, target and failed read requirement");
    invalid = readOnly; Put32(invalid, ImportFile + 16, 0x21fe);
    const auto badTarget = LoadStartup(invalid);
    Check(!badTarget.ok && badTarget.image.bytes.empty() && Has(badTarget.message, "lookup source RVA 0x2040") &&
          Has(badTarget.message, "IAT target RVA 0x21fe") && Has(badTarget.message, "IAT target requires a readable"),
          "read-only startup allowance cannot cross the IAT section boundary");
    invalid = readOnly; Put32(invalid, ImportFile + 16, 0x21f0);
    const auto badTerminator = LoadStartup(invalid);
    Check(!badTerminator.ok && Has(badTerminator.message, "lookup source RVA 0x2050") &&
          Has(badTerminator.message, "IAT target RVA 0x2200"),
          "read-only IAT terminator must remain inside its declared readable section");
    invalid = readOnly; Put32(invalid, ImportFile + 16, 0xfffffffcU);
    const auto highTarget = LoadStartup(invalid);
    Check(!highTarget.ok && Has(highTarget.message, "IAT target RVA 0xfffffffc") &&
          Has(highTarget.message, "IAT target requires a readable"),
          "oversized IAT RVA is diagnosed before guest byte access");
}

Bytes WithCertificates(Bytes bytes, std::uint32_t offset, std::initializer_list<std::uint32_t> lengths) {
    Bytes table;
    for (const auto length : lengths) {
        const auto start = table.size();
        const auto padded = (length + 7U) & ~7U;
        table.resize(start + padded, 0);
        Put32(table, start, length); Put16(table, start + 4, 0x200); Put16(table, start + 6, 2);
        // Deliberately opaque bytes, not a valid PKCS7 document. Structural
        // acceptance must never be mistaken for signature verification.
        std::fill(table.begin() + start + 8, table.begin() + start + length, 0xa5);
    }
    bytes.resize(std::max(bytes.size(), static_cast<std::size_t>(offset) + table.size()), 0);
    std::copy(table.begin(), table.end(), bytes.begin() + offset);
    Directory(bytes, 4, offset, static_cast<std::uint32_t>(table.size()));
    return bytes;
}
Bytes CertificateFixture(std::uint32_t offset = 0x800, std::initializer_list<std::uint32_t> lengths = {16}) {
    return WithCertificates(Fixture(), offset, lengths);
}
void RejectsCertificate(const Bytes& bytes, const std::string& label) {
    const auto result = LoadStartup(bytes);
    Check(!result.ok && result.image.bytes.empty(), label + " rejects without exposing a partial image");
    Check(Has(result.message, "PE32 SECURITY directory 4"), label + " identifies the certificate directory");
}
void StartupCertificates() {
    const auto plain = LoadStartup(Fixture());
    Check(plain.ok && plain.image.certificateFileOffset == 0 && plain.image.certificateSize == 0 &&
          plain.image.certificateCount == 0, "absent certificate directory has no invented metadata");
    const auto bytes = CertificateFixture();
    const auto mapped = LoadStartup(bytes);
    Check(mapped.ok, "startup accepts an aligned bounded PKCS_SIGNED_DATA certificate structure without verifying its opaque payload");
    Check(!Load(bytes).ok, "strict fixture profile still rejects a valid certificate directory");
    if (mapped.ok) {
        Check(mapped.image.certificateFileOffset == 0x800 && mapped.image.certificateSize == 16 &&
              mapped.image.certificateCount == 1, "certificate file offset, table size and record count remain explicit metadata");
        if (plain.ok) {
            auto expected = plain.image.bytes;
            std::copy_n(bytes.begin() + Optional + 96 + 4 * 8, 8, expected.begin() + Optional + 96 + 4 * 8);
            Check(mapped.image.bytes == expected, "certificate overlay changes only its header directory and is never copied into mapped image bytes");
        }
        Check(mapped.image.bytes.size() == 0x4000 && mapped.image.imports.size() == 4,
              "certificate metadata neither enlarges virtual memory nor changes imports");
    }
    const auto executed = xcdm::runtime::TraceWin32Startup32(bytes.data(), bytes.size());
    Check(executed.passed && Has(executed.report, "x86 Win32 host calls passed"),
          "self-authored startup still executes through its real imports with a certificate overlay");
    Check(Has(executed.report, "Certificate bytes are not mapped; Authenticode signature/trust is not verified."),
          "startup report explicitly distinguishes structural certificate acceptance from signature trust");

    const auto chained = LoadStartup(CertificateFixture(0x800, {17, 8}));
    Check(chained.ok && chained.image.certificateSize == 32 && chained.image.certificateCount == 2,
          "WIN_CERTIFICATE records use dwLength plus zero alignment padding, including a minimal eight-byte record");
    auto trailing = bytes; trailing.resize(trailing.size() + 8, 0x5a);
    Check(LoadStartup(trailing).ok, "bounded certificate table need not occupy the final file bytes");

    const auto beyondImage = LoadStartup(CertificateFixture(0x5000));
    Check(beyondImage.ok && beyondImage.image.certificateFileOffset == 0x5000 &&
          beyondImage.image.bytes.size() == 0x4000,
          "SECURITY address is checked against file size and may lie beyond SizeOfImage");
    const auto sameAsRva = LoadStartup(CertificateFixture(0x3000));
    const std::string message = "x86 Win32 host calls passed";
    Check(sameAsRva.ok && std::equal(message.begin(), message.end(), sameAsRva.image.bytes.begin() + 0x3000),
          "a certificate file offset numerically equal to a section RVA does not overwrite that guest section");
    auto gap = bytes;
    gap.resize(0xc00, 0);
    std::copy_n(gap.begin() + MessageFile, 0x200, gap.begin() + 0xa00);
    Put32(gap, Sections + 80 + 20, 0xa00);
    std::copy_n(bytes.begin() + 0x800, 16, gap.begin() + MessageFile);
    Directory(gap, 4, MessageFile, 16);
    Check(LoadStartup(gap).ok, "certificate table may occupy an unmapped file gap before a later raw section");

    Bytes readCode{0xa1}; Append32(readCode, Base + 0x800); AppendExit(readCode);
    const auto readImage = WithCertificates(Program(readCode), 0x800, {16});
    const auto refused = xcdm::runtime::TraceWin32Startup32(readImage.data(), readImage.size());
    Check(!refused.passed && Has(refused.report, "Guest data memory access was refused"),
          "a certificate overlay grants no readable guest mapping at its file offset");

    for (const auto pair : {std::pair<std::uint32_t, std::uint32_t>{0, 16}, {0x800, 0}}) {
        auto invalid = bytes; Directory(invalid, 4, pair.first, pair.second);
        RejectsCertificate(invalid, "inconsistent certificate address/size pair");
    }
    for (const auto pair : {std::pair<std::uint32_t, std::uint32_t>{0x801, 16}, {0x800, 15},
                            {0xfffffff8U, 16}, {0x800, 0xfffffff8U}, {0x810, 8}, {0x808, 16}}) {
        auto invalid = bytes; Directory(invalid, 4, pair.first, pair.second);
        RejectsCertificate(invalid, "misaligned, overflowing or out-of-file certificate range");
    }
    auto invalid = bytes; invalid.pop_back();
    RejectsCertificate(invalid, "certificate table truncated in the input file");
    for (const auto offset : {0x40U, 0x1f8U, 0x200U, 0x400U, 0x600U, 0x7f8U}) {
        invalid = bytes;
        std::copy_n(bytes.begin() + 0x800, 16, invalid.begin() + offset);
        Directory(invalid, 4, offset, 16);
        RejectsCertificate(invalid, "certificate table overlaps mapped headers or a section raw range");
    }
    for (const auto length : {0U, 1U, 7U, 17U, 0xffffffffU}) {
        invalid = bytes; Put32(invalid, 0x800, length);
        RejectsCertificate(invalid, "WIN_CERTIFICATE length is too short, overlong or overflows alignment");
    }
    invalid = CertificateFixture(0x800, {9}); invalid[0x809] = 1;
    RejectsCertificate(invalid, "WIN_CERTIFICATE external alignment padding is nonzero");
    invalid = CertificateFixture(0x800, {16, 16}); Put32(invalid, 0x810, 0);
    RejectsCertificate(invalid, "a later malformed record cannot hide behind an earlier valid certificate");
    for (const auto revision : {0U, 0x100U, 0x201U}) {
        invalid = bytes; Put16(invalid, 0x804, static_cast<std::uint16_t>(revision));
        RejectsCertificate(invalid, "unsupported certificate revision");
    }
    for (const auto type : {0U, 1U, 3U, 4U, 0xffffU}) {
        invalid = bytes; Put16(invalid, 0x806, static_cast<std::uint16_t>(type));
        RejectsCertificate(invalid, "unsupported certificate type");
    }
    auto many = bytes;
    many.resize(0x800 + 257 * 8, 0);
    for (unsigned n = 0; n < 257; ++n) {
        Put32(many, 0x800 + n * 8, 8); Put16(many, 0x804 + n * 8, 0x200); Put16(many, 0x806 + n * 8, 2);
    }
    Directory(many, 4, 0x800, 256 * 8);
    const auto limit = LoadStartup(many);
    Check(limit.ok && limit.image.certificateCount == 256, "certificate record count accepts its exact bounded maximum");
    Directory(many, 4, 0x800, 257 * 8);
    RejectsCertificate(many, "certificate record count exceeds its bounded maximum");

    for (const auto index : {5U, 9U, 14U}) {
        invalid = bytes; Directory(invalid, index, 0x2000, 32);
        const auto result = LoadStartup(invalid);
        Check(!result.ok && result.image.bytes.empty() && Has(result.message, "directory " + std::to_string(index)) &&
              Has(result.message, "0x2000"),
              "certificate support does not enable TLS/relocation/CLR and reports the exact unsupported directory index and RVA");
    }
}

void SharedStartupLimits() {
    const auto libraries = [](unsigned normal, unsigned delay) {
        auto bytes = LargeImportImage();
        Directory(bytes, 1, 0x2000, (normal + 1) * 20);
        Directory(bytes, 13, 0x5000, (delay + 1) * 32);
        for (unsigned n = 0; n < normal + delay; ++n) {
            const std::uint32_t lookup = 0xa000 + n * 8, iat = 0xb000 + n * 8;
            Put32(bytes, ImportRaw(lookup), 0x80000007); Put32(bytes, ImportRaw(iat), 0x80000007);
            if (n < normal) {
                const auto at = ImportRaw(0x2000) + n * 20;
                Put32(bytes, at, lookup); Put32(bytes, at + 12, 0x2100); Put32(bytes, at + 16, iat);
            } else {
                const auto at = ImportRaw(0x5000) + (n - normal) * 32;
                Put32(bytes, at, 1); Put32(bytes, at + 4, 0x2100);
                Put32(bytes, at + 12, iat); Put32(bytes, at + 16, lookup);
            }
        }
        return bytes;
    };
    const auto exactLibraries = LoadStartup(libraries(1, 255));
    Check(exactLibraries.ok && exactLibraries.image.imports.size() == 256,
          "shared normal/delay library count accepts exactly 256");
    RejectsStartup(libraries(1, 256), "shared normal/delay library count exceeds 256");
    const auto symbols = [](unsigned normal, unsigned delay) {
        auto bytes = LargeImportImage();
        Directory(bytes, 1, 0x2000, 40); Directory(bytes, 13, 0x2040, 64);
        Put32(bytes, ImportRaw(0x2000), 0x4000); Put32(bytes, ImportRaw(0x200c), 0x2100);
        Put32(bytes, ImportRaw(0x2010), 0xc100);
        Put32(bytes, ImportRaw(0x2040), 1); Put32(bytes, ImportRaw(0x2044), 0x2100);
        Put32(bytes, ImportRaw(0x204c), 0x15100); Put32(bytes, ImportRaw(0x2050), 0x15000);
        for (unsigned n = 0; n < normal; ++n) {
            Put32(bytes, ImportRaw(0x4000 + n * 4), 0x80000007);
            Put32(bytes, ImportRaw(0xc100 + n * 4), 0x80000007);
        }
        for (unsigned n = 0; n < delay; ++n) {
            Put32(bytes, ImportRaw(0x15000 + n * 4), 0x80000007);
            Put32(bytes, ImportRaw(0x15100 + n * 4), 0x80000007);
        }
        return bytes;
    };
    const auto exactImports = LoadStartup(symbols(8191, 1));
    Check(exactImports.ok && exactImports.image.imports.size() == 8192 && exactImports.image.imports.back().delayed,
          "shared normal/delay symbol count accepts exactly 8192");
    RejectsStartup(symbols(8191, 2), "shared normal/delay symbol count exceeds 8192");
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
    StartupDelayImports();
    ReadOnlyStartupIats();
    StartupCertificates();
    SharedStartupLimits();
    RuntimeMemoryPermissions();
    std::cout << checks << " PE32 loader and Win32 execution checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
