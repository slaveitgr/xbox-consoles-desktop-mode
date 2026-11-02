#include "runtime/GamePreflight.h"
#include "samples/Return42Fixture.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t kOptional = 0x98;
int failures = 0;
std::size_t checks = 0;

void Check(bool condition, const std::string& name) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}

void Put16(Bytes& bytes, std::size_t at, std::uint16_t value) {
    for (unsigned i = 0; i < 2; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
void Put64(Bytes& bytes, std::size_t at, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
void Text(Bytes& bytes, std::size_t at, const std::string& value) {
    std::copy(value.begin(), value.end(), bytes.begin() + at);
    bytes[at + value.size()] = 0;
}
std::uint32_t Rva(std::size_t file) { return 0x1000U + static_cast<std::uint32_t>(file - 0x200); }
std::size_t Section(bool is64) { return kOptional + (is64 ? 240 : 224); }
void Directory(Bytes& bytes, bool is64, std::size_t index, std::uint32_t address, std::uint32_t size) {
    const auto at = kOptional + (is64 ? 112 : 96) + index * 8;
    Put32(bytes, at, address); Put32(bytes, at + 4, size);
}
void Thunk(Bytes& bytes, bool is64, std::size_t at, std::uint64_t value) {
    if (is64) Put64(bytes, at, value); else Put32(bytes, at, static_cast<std::uint32_t>(value));
}

Bytes Image(bool is64 = false) {
    Bytes bytes(0x600);
    Put16(bytes, 0, 0x5a4d); Put32(bytes, 0x3c, 0x80);
    Put32(bytes, 0x80, 0x4550); Put16(bytes, 0x84, is64 ? 0x8664 : 0x14c);
    Put16(bytes, 0x86, 1); Put16(bytes, 0x94, is64 ? 240 : 224); Put16(bytes, 0x96, 0x22);
    Put16(bytes, kOptional, is64 ? 0x20b : 0x10b); Put32(bytes, kOptional + 16, 0x1000);
    if (is64) Put64(bytes, kOptional + 24, 0x140000000ULL); else Put32(bytes, kOptional + 28, 0x400000);
    Put32(bytes, kOptional + 32, 0x1000); Put32(bytes, kOptional + 36, 0x200);
    Put32(bytes, kOptional + 56, 0x2000); Put32(bytes, kOptional + 60, 0x200);
    Put16(bytes, kOptional + 68, 2); Put32(bytes, kOptional + (is64 ? 108 : 92), 16);
    const auto section = Section(is64);
    Text(bytes, section, ".text");
    Put32(bytes, section + 8, 0x400); Put32(bytes, section + 12, 0x1000);
    Put32(bytes, section + 16, 0x400); Put32(bytes, section + 20, 0x200);
    Put32(bytes, section + 36, 0x60000020); bytes[0x200] = 0xc3;
    return bytes;
}

Bytes Imports(bool is64 = false, bool delayed = false, bool legacy = false) {
    auto bytes = Image(is64);
    const auto width = is64 ? 8 : 4;
    Directory(bytes, is64, 1, Rva(0x240), 40);
    Put32(bytes, 0x240, Rva(0x280)); Put32(bytes, 0x24c, Rva(0x2d0)); Put32(bytes, 0x250, Rva(0x2a0));
    const auto ordinal = is64 ? 0x8000000000000007ULL : 0x80000007ULL;
    Thunk(bytes, is64, 0x280, Rva(0x300)); Thunk(bytes, is64, 0x280 + width, ordinal);
    Thunk(bytes, is64, 0x2a0, Rva(0x300)); Thunk(bytes, is64, 0x2a0 + width, ordinal);
    Text(bytes, 0x2d0, "D3D9.dll"); Text(bytes, 0x302, "Direct3DCreate9");
    if (delayed) {
        const std::uint32_t base = legacy ? 0x400000 : 0;
        Directory(bytes, is64, 13, Rva(0x340), 64);
        Put32(bytes, 0x340, legacy ? 0 : 1);
        Put32(bytes, 0x344, base + Rva(0x3f0));
        Put32(bytes, 0x34c, base + Rva(0x3c0)); Put32(bytes, 0x350, base + Rva(0x3a0));
        Thunk(bytes, is64, 0x3a0, base + Rva(0x420)); Thunk(bytes, is64, 0x3c0, base + Rva(0x420));
        Text(bytes, 0x3f0, "KERNEL32.dll"); Text(bytes, 0x422, "LoadLibraryA");
    }
    return bytes;
}

auto Inspect(const Bytes& bytes) { return xcdm::runtime::InspectGamePe(bytes.data(), bytes.size()); }
bool Has(const std::string& text, const std::string& find) { return text.find(find) != std::string::npos; }

void Rejects(const Bytes& bytes, const std::string& label) {
    const auto result = Inspect(bytes);
    Check(!result.ok, label + " rejected");
    Check(Has(result.summary, "failed"), label + " has useful failure summary");
    Check(Has(result.report, "No guest code was executed"), label + " does not imply execution");
}

void ValidMetadata() {
    for (bool is64 : {false, true}) {
        const auto result = Inspect(Imports(is64, true));
        Check(result.ok, "normal and delay imports accepted for both widths");
        Check(Has(result.summary, is64 ? "PE32+ x64" : "PE32 x86"), "architecture decoded");
        Check(Has(result.report, "Windows GUI"), "subsystem decoded");
        Check(Has(result.report, is64 ? "0x140000000" : "0x400000"), "preferred base decoded");
        Check(Has(result.report, "Entry point RVA: 0x1000"), "entry decoded");
        Check(Has(result.report, "DLL D3D9.dll") && Has(result.report, "Direct3DCreate9"), "normal DLL and symbol decoded");
        Check(Has(result.report, "ordinal #7"), "ordinal decoded");
        Check(Has(result.report, "KERNEL32.dll (delay-loaded)") && Has(result.report, "LoadLibraryA"), "delay DLL and symbol decoded");
        Check(Has(result.report, "Dynamic loading API imports were found"), "dynamic dependency caveat supported by imports");
        Check(Has(result.report, "Graphics indicators (imports only): Direct3D 9"), "D3D9 identified from case-insensitive DLL");
        Check(Has(result.report, "no image entry point was invoked"), "success explicitly non-executing");
        Check(Has(result.report, "Direct3D 9 imports require a guest D3D9 implementation"), "concrete graphics gap");
        if (!is64) Check(Has(result.report, "integer-only; FPU/MMX/SSE"), "concrete x86 game execution gap");
    }
    Check(Inspect(Imports(false, true, true)).ok, "legacy VA-based PE32 delay imports accepted");
    const auto& minimal = xcdm::samples::Return42PeBytes();
    const auto result = xcdm::runtime::InspectGamePe(minimal.data(), minimal.size());
    Check(result.ok, "existing minimal diagnostic fixture remains inspectable");
    Check(Has(result.report, "none declared") && Has(result.report, "dynamically loaded graphics remain unknown"), "absence of imports is not proof of no dependencies");
    auto noEntry = Image(); Put32(noEntry, kOptional + 16, 0);
    Check(Inspect(noEntry).ok, "metadata inspection can describe an image with no entry point");
    auto dirs = Image(true);
    Directory(dirs, true, 9, Rva(0x480), 40); Directory(dirs, true, 5, Rva(0x4c0), 8);
    Check(Has(Inspect(dirs).report, "TLS directory: present") && Has(Inspect(dirs).report, "Base relocation directory: present"), "TLS and reloc presence recorded without invoking them");
    auto certificate = Image(); certificate.resize(0x610);
    Directory(certificate, false, 4, 0x600, 16);
    Check(Inspect(certificate).ok, "certificate data directory uses file offset rather than RVA");
    auto fallback = Imports(); Put32(fallback, 0x240, 0);
    Check(Inspect(fallback).ok, "unbound IAT used when original thunk table absent");
    Put32(fallback, 0x244, 1); Put32(fallback, 0x2a0, 0x7ff01234); Put32(fallback, 0x2a4, 0x7ff05678);
    const auto bound = Inspect(fallback);
    Check(bound.ok && Has(bound.report, "symbol names are unavailable"), "bound IAT not misinterpreted as RVAs");
}

void InvalidHeadersAndSections() {
    Check(!xcdm::runtime::InspectGamePe(nullptr, 128).ok, "null input rejected");
    auto bytes = Image();
    Check(!xcdm::runtime::InspectGamePe(bytes.data(), 64U * 1024U * 1024U + 1).ok, "oversize rejected before reading input");
    const auto mutate32 = [](std::size_t offset, std::uint32_t value, const std::string& name) {
        auto b = Image(); Put32(b, offset, value); Rejects(b, name);
    };
    mutate32(0x3c, 0xfffffff0U, "overflowing header location");
    mutate32(0x3c, 0, "header points into DOS header");
    mutate32(0x80, 0, "bad PE signature");
    mutate32(kOptional + 56, 0, "empty image");
    mutate32(kOptional + 60, 0xffffffffU, "header size overflow");
    mutate32(kOptional + 32, 0, "zero alignment");
    mutate32(kOptional + 92, 17, "too many directories");
    mutate32(kOptional + 16, 0xffffffffU, "entry RVA outside image");
    mutate32(Section(false) + 8, 0xffffffffU, "section virtual range overflow");
    mutate32(Section(false) + 12, 0xfffff000U, "section RVA overflow");
    mutate32(Section(false) + 20, 0xfffffe00U, "raw file offset overflow");
    mutate32(Section(false) + 16, 0xffffffffU, "raw data size overflow");
    mutate32(Section(false) + 20, 0, "raw data overlaps headers");
    bytes = Image(); Put16(bytes, 0x84, 0x8664); Rejects(bytes, "machine/header mismatch");
    bytes = Image(); Put16(bytes, 0x94, 90); Rejects(bytes, "short standard optional header");
    bytes = Image(); Put16(bytes, 0x94, 100); Rejects(bytes, "directories exceed optional header");
    bytes = Image(); Put16(bytes, 0x86, 97); Rejects(bytes, "section cap");
    bytes = Image(true); Put64(bytes, kOptional + 24, 0xfffffffffffff000ULL); Rejects(bytes, "64-bit image-base addition overflow");
    bytes = Image(); Put32(bytes, kOptional + 28, 0xfffff000U); Rejects(bytes, "32-bit preferred image exceeds address space");
    bytes = Image(); Put16(bytes, 0x86, 2);
    std::copy(bytes.begin() + Section(false), bytes.begin() + Section(false) + 40,
              bytes.begin() + Section(false) + 40);
    Rejects(bytes, "overlapping section mappings");
    const auto original = Image();
    for (std::size_t size = 0; size < original.size(); ++size)
        Check(!xcdm::runtime::InspectGamePe(original.data(), size).ok, "truncated metadata at " + std::to_string(size));
}

void InvalidImports() {
    auto bytes = Imports(); Directory(bytes, false, 1, 0xfffffff0U, 40); Rejects(bytes, "import directory RVA overflow");
    bytes = Imports(); Directory(bytes, false, 1, Rva(0x5f0), 40); Rejects(bytes, "import directory crosses raw boundary");
    bytes = Imports(); Directory(bytes, false, 1, Rva(0x240), 20); Rejects(bytes, "descriptor missing null terminator");
    bytes = Imports(); Put32(bytes, 0x24c, 0); Rejects(bytes, "descriptor missing DLL name");
    bytes = Imports(); Put32(bytes, 0x240, 0x1f00); Rejects(bytes, "lookup points into virtual-only tail");
    bytes = Imports(); Put32(bytes, 0x250, 0xffffffffU); Rejects(bytes, "invalid IAT RVA");
    bytes = Imports(); Put32(bytes, 0x280, 0xffffffffU); Rejects(bytes, "ordinal contains reserved bits");
    bytes = Imports(); Put32(bytes, 0x280, 0x1fff); Rejects(bytes, "name hint outside file-backed bytes");
    bytes = Imports(); Put32(bytes, 0x24c, Rva(0x5f8));
    std::fill(bytes.begin() + 0x5f8, bytes.end(), 'A'); Rejects(bytes, "unterminated DLL string at file boundary");
    bytes = Imports(); bytes[0x2d0] = '\n'; Rejects(bytes, "control characters in metadata");
    bytes = Imports(); bytes[0x2d0] = 0; Rejects(bytes, "empty DLL name");
    bytes = Imports(); Put32(bytes, 0x24c, Rva(0x300));
    std::fill(bytes.begin() + 0x300, bytes.begin() + 0x500, 'A'); Rejects(bytes, "overlong import name bounded");
    bytes = Imports(); Put32(bytes, 0x240, Rva(0x5f0));
    for (std::size_t at = 0x5f0; at < bytes.size(); at += 4) Put32(bytes, at, 0x80000001);
    Rejects(bytes, "unterminated lookup table");
    bytes = Imports(true); Put64(bytes, 0x280, 0x100001100ULL); Rejects(bytes, "PE32+ named thunk exceeds RVA width");
    bytes = Imports(false, true); Put32(bytes, 0x340, 3); Rejects(bytes, "delay attributes reserved bits");
    bytes = Imports(false, true); Put32(bytes, 0x350, 0); Rejects(bytes, "missing delay name table");
    bytes = Imports(false, true, true); Put32(bytes, 0x344, 0x1000); Rejects(bytes, "legacy delay VA below image base");
    bytes = Imports(false, true); Directory(bytes, false, 13, Rva(0x340), 32); Rejects(bytes, "delay descriptors missing terminator");
    bytes = Image(); Directory(bytes, false, 9, Rva(0x480), 4); Rejects(bytes, "truncated TLS directory");
    bytes = Image(); Directory(bytes, false, 5, Rva(0x5fc), 8); Rejects(bytes, "truncated relocation data");
}

void AdversarialMutations() {
    // Header/import mutations are also run under the host sanitizers. This
    // corpus adds pointer/bounds combinations beyond the explicit regressions.
    std::uint32_t random = 0x47544153;
    for (unsigned i = 0; i < 3000; ++i) {
        auto bytes = Imports((i & 1) != 0, true);
        for (unsigned j = 0; j < 4; ++j) {
            random = random * 1664525U + 1013904223U;
            const auto at = random % bytes.size();
            random = random * 1664525U + 1013904223U;
            bytes[at] = static_cast<std::uint8_t>(random >> 24);
        }
        const auto result = Inspect(bytes);
        Check(result.report.size() < 513U * 1024U &&
              (!result.ok || Has(result.report, "No guest code was executed")), "bounded non-executing mutation result");
    }
}

} // namespace

int main() {
    ValidMetadata();
    InvalidHeadersAndSections();
    InvalidImports();
    AdversarialMutations();
    std::cout << checks << " game preflight checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
