#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xcdm::runtime {

enum class Pe32Profile { Fixture, StartupTrace };

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
    bool delayed = false;
};

struct Pe32Image {
    std::uint32_t base = 0;
    std::uint32_t entry = 0;
    std::uint32_t headerSize = 0;
    // SECURITY uses a file offset, not an RVA. These fields only describe a
    // structurally checked table; its payload is never mapped or trusted.
    std::uint32_t certificateFileOffset = 0;
    std::uint32_t certificateSize = 0;
    std::uint32_t certificateCount = 0;
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
// executable memory. Both profiles reject DLLs, TLS, relocations and CLR.
// Fixture rejects certificates, delay imports and writable/executable sections.
// StartupTrace permits initial binding into readable IAT sections, preserving
// guest write permissions. It retains W+X flags only on interpreted byte data and
// collects bounded normal and delay imports, retaining original IAT bytes.
// It accepts up to 256 structurally bounded WIN_CERTIFICATE revision 0x0200,
// PKCS_SIGNED_DATA type 0x0002 records outside headers/section raw data.
// Certificate bytes stay in the input file; Authenticode is not verified.
// Imports are not resolved to host pointers. This is not a general NT loader:
// dependencies are not loaded and DLL initialization is not performed.
Pe32LoadResult LoadPe32Image(const std::uint8_t* bytes, std::size_t size,
    Pe32Profile profile = Pe32Profile::Fixture) noexcept;

} // namespace xcdm::runtime
