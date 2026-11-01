#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xcdm::runtime {

struct PeSection {
    std::uint32_t virtualAddress = 0;
    std::uint32_t virtualSize = 0;
    std::uint32_t rawOffset = 0;
    std::uint32_t rawSize = 0;
    std::uint32_t characteristics = 0;
};

struct PeImage {
    std::uint32_t imageSize = 0;
    std::uint32_t headerSize = 0;
    std::uint32_t entryPointRva = 0;
    std::uint64_t preferredBase = 0;
    std::vector<PeSection> sections;
};

struct PeParseResult {
    bool ok = false;
    PeImage image;
    std::string message;
};

// A deliberately narrow parser for the diagnostic's PE32+ AMD64 profile.
// No imports, relocations, TLS, exception metadata, CLR, or other directories.
// Successful parsing does not prove that arbitrary machine code is safe,
// position independent, or compatible with UWP. The probe executes ONLY its
// embedded, self-authored fixture, never a caller-provided image.
PeParseResult ParseMinimalPe(const std::uint8_t* bytes, std::size_t size) noexcept;

} // namespace xcdm::runtime
