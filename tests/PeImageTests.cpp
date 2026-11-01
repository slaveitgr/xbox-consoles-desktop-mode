#include "runtime/PeImage.h"
#include "samples/Return42Fixture.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t kOptional = 0x98;
constexpr std::size_t kSection = kOptional + 240;
int failures = 0;
std::size_t assertions = 0;

void Check(bool condition, const std::string& message) {
    ++assertions;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

Bytes Fixture() {
    const auto& source = xcdm::samples::Return42PeBytes();
    return {source.begin(), source.end()};
}

void Put16(Bytes& bytes, std::size_t offset, std::uint16_t value) {
    for (unsigned i = 0; i != 2; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void Put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i != 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void Rejects(const Bytes& bytes, const std::string& label) {
    const auto result = xcdm::runtime::ParseMinimalPe(bytes.data(), bytes.size());
    Check(!result.ok, label + " rejected");
    Check(!result.message.empty(), label + " reports a reason");
    Check(result.image.sections.empty(), label + " exposes no partial image");
}

void Mutation32(std::size_t offset, std::uint32_t value, const std::string& label) {
    auto bytes = Fixture(); Put32(bytes, offset, value); Rejects(bytes, label);
}

void Mutation16(std::size_t offset, std::uint16_t value, const std::string& label) {
    auto bytes = Fixture(); Put16(bytes, offset, value); Rejects(bytes, label);
}

Bytes TwoSections() {
    auto bytes = Fixture();
    bytes.resize(1536);
    Put16(bytes, 0x86, 2);
    Put32(bytes, kOptional + 56, 12288);
    std::copy(bytes.begin() + kSection, bytes.begin() + kSection + 40,
              bytes.begin() + kSection + 40);
    Put32(bytes, kSection + 40 + 8, 16);
    Put32(bytes, kSection + 40 + 12, 8192);
    Put32(bytes, kSection + 40 + 20, 1024);
    Put32(bytes, kSection + 40 + 36, 0xc0000040U);
    return bytes;
}

void CheckValidImages() {
    const auto bytes = Fixture();
    const auto result = xcdm::runtime::ParseMinimalPe(bytes.data(), bytes.size());
    Check(result.ok, "self-authored fixture accepted");
    Check(result.image.imageSize == 8192 && result.image.headerSize == 512,
          "image/header sizes decoded");
    Check(result.image.entryPointRva == 4096 && result.image.preferredBase == 0x140000000ULL,
          "entry point and 64-bit preferred base decoded");
    Check(result.image.sections.size() == 1, "one section decoded");
    if (result.image.sections.size() == 1) {
        const auto& section = result.image.sections[0];
        Check(section.rawOffset == 512 && section.rawSize == 512 && section.virtualSize == 6,
              "section file and memory ranges decoded");
    }
    auto two = TwoSections();
    Check(xcdm::runtime::ParseMinimalPe(two.data(), two.size()).ok,
          "separate code/data pages accepted");
    two.resize(1024);
    Put32(two, kSection + 40 + 8, 4096);
    Put32(two, kSection + 40 + 16, 0);
    Put32(two, kSection + 40 + 20, 0);
    Check(xcdm::runtime::ParseMinimalPe(two.data(), two.size()).ok,
          "zero-filled data section accepted without file access");
}

void CheckTruncation() {
    const auto bytes = Fixture();
    Check(!xcdm::runtime::ParseMinimalPe(nullptr, bytes.size()).ok, "null buffer rejected");
    for (std::size_t size = 0; size != bytes.size(); ++size) {
        const auto result = xcdm::runtime::ParseMinimalPe(bytes.data(), size);
        Check(!result.ok, "truncated fixture at byte " + std::to_string(size));
    }
    auto overlay = bytes;
    overlay.push_back(0);
    Rejects(overlay, "file overlay");
}

void CheckHeaders() {
    Mutation16(0, 0, "DOS signature");
    Mutation32(0x3c, 0, "PE header points inside DOS header");
    Mutation32(0x3c, 0xfffffff0U, "overflowing PE header offset");
    Mutation32(0x80, 0, "PE signature");
    Mutation16(0x84, 0x14c, "x86 architecture");
    Mutation16(0x84, 0xaa64, "ARM64 architecture");
    Mutation16(0x86, 0, "no sections");
    Mutation16(0x86, 17, "excessive section count");
    Mutation32(0x8c, 512, "COFF symbol table");
    Mutation32(0x90, 1, "COFF symbol count");
    Mutation16(0x94, 239, "small optional header");
    Mutation16(0x94, 65535, "large optional header");
    Mutation16(0x96, 0, "non-executable COFF image");
    Mutation16(0x96, 0x2023, "DLL image");
    Mutation16(kOptional, 0x10b, "PE32 optional header");
    for (auto alignment : {0U, 1U, 512U, 4095U, 0x80000000U}) {
        Mutation32(kOptional + 32, alignment, "invalid section alignment");
    }
    for (auto alignment : {0U, 256U, 513U, 131072U}) {
        Mutation32(kOptional + 36, alignment, "invalid file alignment");
    }
    Mutation32(kOptional + 36, 8192, "file alignment exceeds section alignment");
    for (auto imageSize : {0U, 4096U, 8193U, 0xfffff000U}) {
        Mutation32(kOptional + 56, imageSize, "invalid image size");
    }
    for (auto headerSize : {0U, 256U, 513U, 1536U, 0xfffffe00U}) {
        Mutation32(kOptional + 60, headerSize, "invalid header size");
    }
    Mutation32(kOptional + 104, 1, "loader flags");
    Mutation32(kOptional + 108, 15, "too few data directories");
    Mutation32(kOptional + 108, 17, "too many data directories");
    for (std::size_t directory = 0; directory != 16; ++directory) {
        const auto offset = kOptional + 112 + directory * 8;
        Mutation32(offset, 4096, "directory address " + std::to_string(directory));
        Mutation32(offset + 4, 8, "directory size " + std::to_string(directory));
    }
}

void CheckSections() {
    Mutation32(kSection + 8, 0xffffffffU, "overflowing virtual size");
    for (auto address : {0U, 512U, 4097U, 8192U, 0xfffff000U}) {
        Mutation32(kSection + 12, address, "invalid section RVA");
    }
    for (auto size : {1U, 513U, 1024U, 0xfffffe00U}) {
        Mutation32(kSection + 16, size, "invalid section raw size");
    }
    for (auto offset : {0U, 1U, 513U, 1024U, 0xfffffe00U}) {
        Mutation32(kSection + 20, offset, "invalid section raw offset");
    }
    Mutation32(kSection + 36, 0xe0000020U, "writable executable section");
    Mutation32(kSection + 36, 0x80000040U, "unreadable section");
    Mutation32(kSection + 36, 0x40000040U, "non-executable entry point");
    Mutation32(kSection + 24, 512, "section relocation pointer");
    Mutation32(kSection + 28, 512, "section line-number pointer");
    Mutation16(kSection + 32, 1, "section relocation count");
    Mutation16(kSection + 34, 1, "section line-number count");
    for (auto entry : {0U, 1U, 0x1006U, 0x1200U, 0x2000U, 0xffffffffU}) {
        Mutation32(kOptional + 16, entry, "invalid entry point");
    }
    auto empty = Fixture();
    Put32(empty, kSection + 8, 0); Put32(empty, kSection + 16, 0);
    Put32(empty, kSection + 20, 0);
    Rejects(empty, "empty section");
    auto overlap = TwoSections();
    Put32(overlap, kSection + 40 + 12, 4096);
    Rejects(overlap, "overlapping virtual sections");
    overlap = TwoSections();
    Put32(overlap, kSection + 40 + 20, 512);
    Rejects(overlap, "overlapping raw sections");
    auto tableOutside = Fixture();
    Put16(tableOutside, 0x86, 4);
    Rejects(tableOutside, "section table exceeds SizeOfHeaders");
}

void CheckDeterministicAdversarialInputs() {
    // Fixed-seed mutation corpus exercises the byte reader under ASan/UBSan.
    // Randomly changed machine code may remain a valid PE; parsing is never
    // treated as authorization to execute those bytes.
    std::uint32_t random = 0x5843444d;
    for (std::size_t iteration = 0; iteration != 6000; ++iteration) {
        auto bytes = Fixture();
        for (unsigned mutation = 0; mutation != 4; ++mutation) {
            random = random * 1664525U + 1013904223U;
            const std::size_t offset = random % bytes.size();
            random = random * 1664525U + 1013904223U;
            bytes[offset] = static_cast<std::uint8_t>(random >> 24);
        }
        const auto result = xcdm::runtime::ParseMinimalPe(bytes.data(), bytes.size());
        Check(!result.ok || (result.image.imageSize <= 64U * 1024U * 1024U &&
                            !result.image.sections.empty()),
              "mutation result stays within bounded profile");
    }
}

} // namespace

int main() {
    CheckValidImages();
    CheckTruncation();
    CheckHeaders();
    CheckSections();
    CheckDeterministicAdversarialInputs();
    std::cout << assertions << " parser checks; " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
