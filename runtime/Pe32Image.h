#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xcdm::runtime {

struct Pe32Section {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
    bool readable = false;
    bool writable = false;
    bool executable = false;
};

struct Pe32Import {
    std::string library;
    std::string name;
    std::uint16_t ordinal = 0;
    bool byOrdinal = false;
    std::uint32_t iatRva = 0;
};

struct Pe32Image {
    std::uint32_t base = 0;
    std::uint32_t entry = 0;
    std::uint32_t headerSize = 0;
    std::vector<std::uint8_t> bytes;
    std::vector<Pe32Section> sections;
    std::vector<Pe32Import> imports;
};

struct Pe32LoadResult {
    bool ok = false;
    std::string message;
    Pe32Image image;
};

// Maps a bounded PE32/i386 image into a guest byte vector, never native
// executable memory. This initial profile rejects DLLs, TLS, relocations,
// delay imports, CLR and writable/executable sections. Imports are collected,
// not resolved to host pointers. It is not a general Windows loader.
Pe32LoadResult LoadPe32Image(const std::uint8_t* bytes, std::size_t size) noexcept;

} // namespace xcdm::runtime
