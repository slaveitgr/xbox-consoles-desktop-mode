#include "PeImage.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace xcdm::runtime {
namespace {

constexpr std::uint32_t kMaxImageSize = 64U * 1024U * 1024U;
constexpr std::uint32_t kExecute = 0x20000000U;
constexpr std::uint32_t kRead = 0x40000000U;
constexpr std::uint32_t kWrite = 0x80000000U;

bool Range(std::size_t offset, std::size_t count, std::size_t size) noexcept {
    return offset <= size && count <= size - offset;
}

std::uint16_t U16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8U);
}

std::uint32_t U32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::uint64_t U64(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(U32(p)) |
           (static_cast<std::uint64_t>(U32(p + 4)) << 32U);
}

bool PowerOfTwo(std::uint32_t n) noexcept {
    return n != 0 && (n & (n - 1U)) == 0;
}

std::uint64_t AlignUp(std::uint64_t value, std::uint32_t alignment) noexcept {
    return (value + alignment - 1U) & ~(static_cast<std::uint64_t>(alignment) - 1U);
}

bool Overlap(std::uint64_t a, std::uint64_t an,
             std::uint64_t b, std::uint64_t bn) noexcept {
    return an != 0 && bn != 0 && a < b + bn && b < a + an;
}

PeParseResult Reject(const char* message) {
    PeParseResult result;
    result.message = message;
    return result;
}

PeParseResult Parse(const std::uint8_t* bytes, std::size_t size) {
    if (bytes == nullptr || !Range(0, 64, size)) {
        return Reject("Missing or truncated DOS header.");
    }
    if (U16(bytes) != 0x5a4dU) return Reject("Invalid DOS signature.");

    const std::size_t pe = U32(bytes + 0x3c);
    if (pe < 64 || !Range(pe, 24, size)) return Reject("Invalid PE header offset.");
    if (U32(bytes + pe) != 0x00004550U) return Reject("Invalid PE signature.");
    const auto* coff = bytes + pe + 4;
    if (U16(coff) != 0x8664U) return Reject("Only AMD64/x64 images are supported.");
    const std::uint16_t sectionCount = U16(coff + 2);
    if (sectionCount == 0 || sectionCount > 16) return Reject("Invalid section count.");
    const std::uint16_t optionalSize = U16(coff + 16);
    const std::uint16_t flags = U16(coff + 18);
    if (U32(coff + 8) != 0 || U32(coff + 12) != 0) {
        return Reject("COFF symbol tables are unsupported.");
    }
    if ((flags & 0x0002U) == 0 || (flags & 0x2000U) != 0) {
        return Reject("The diagnostic requires an executable image, not a DLL.");
    }
    if (optionalSize != 240) return Reject("Unsupported optional-header size.");

    const std::size_t optionalOffset = pe + 24;
    if (!Range(optionalOffset, optionalSize, size)) return Reject("Truncated optional header.");
    const auto* optional = bytes + optionalOffset;
    if (U16(optional) != 0x020bU) return Reject("Only PE32+ images are supported.");
    const std::uint32_t sectionAlignment = U32(optional + 32);
    const std::uint32_t fileAlignment = U32(optional + 36);
    if (!PowerOfTwo(sectionAlignment) || sectionAlignment < 4096 ||
        sectionAlignment > kMaxImageSize || !PowerOfTwo(fileAlignment) ||
        fileAlignment < 512 || fileAlignment > 65536 || sectionAlignment < fileAlignment) {
        return Reject("Unsupported section or file alignment.");
    }
    if (U32(optional + 108) != 16) return Reject("Unsupported data-directory count.");
    for (std::size_t directory = 0; directory != 16; ++directory) {
        const auto* entry = optional + 112 + directory * 8;
        if (U32(entry) != 0 || U32(entry + 4) != 0) {
            return Reject("Data directories are unsupported (including imports, relocations, TLS, CLR and exceptions).");
        }
    }
    if (U32(optional + 104) != 0) return Reject("Unsupported loader flags.");

    PeImage image;
    image.entryPointRva = U32(optional + 16);
    image.preferredBase = U64(optional + 24);
    image.imageSize = U32(optional + 56);
    image.headerSize = U32(optional + 60);
    if (image.imageSize == 0 || image.imageSize > kMaxImageSize ||
        image.imageSize % sectionAlignment != 0 || image.headerSize == 0 ||
        image.headerSize % fileAlignment != 0 || image.headerSize > image.imageSize ||
        image.headerSize > size) {
        return Reject("Invalid image or header size.");
    }
    const std::size_t table = optionalOffset + optionalSize;
    const std::size_t tableSize = static_cast<std::size_t>(sectionCount) * 40;
    if (!Range(table, tableSize, size) || table > image.headerSize ||
        tableSize > image.headerSize - table) {
        return Reject("Truncated section table or table outside headers.");
    }
    if (image.entryPointRva == 0 || image.entryPointRva >= image.imageSize) {
        return Reject("Invalid entry point.");
    }

    image.sections.reserve(sectionCount);
    bool executableEntry = false;
    std::uint64_t lastRawEnd = image.headerSize;
    for (std::size_t i = 0; i != sectionCount; ++i) {
        const auto* entry = bytes + table + i * 40;
        PeSection section;
        section.virtualSize = U32(entry + 8);
        section.virtualAddress = U32(entry + 12);
        section.rawSize = U32(entry + 16);
        section.rawOffset = U32(entry + 20);
        section.characteristics = U32(entry + 36);
        const std::uint64_t span = std::max(section.virtualSize, section.rawSize);
        const std::uint64_t protectedSpan = AlignUp(span, sectionAlignment);
        if (span == 0 || section.virtualAddress % sectionAlignment != 0 ||
            section.virtualAddress < AlignUp(image.headerSize, sectionAlignment) ||
            static_cast<std::uint64_t>(section.virtualAddress) + protectedSpan > image.imageSize) {
            return Reject("Invalid section virtual range.");
        }
        if (section.rawSize == 0) {
            if (section.rawOffset != 0) return Reject("Empty section has a raw-data offset.");
        } else {
            if (section.rawSize % fileAlignment != 0 ||
                section.rawOffset % fileAlignment != 0 || section.rawOffset < image.headerSize ||
                !Range(section.rawOffset, section.rawSize, size)) {
                return Reject("Invalid or truncated section raw-data range.");
            }
            lastRawEnd = std::max(lastRawEnd,
                static_cast<std::uint64_t>(section.rawOffset) + section.rawSize);
        }
        if ((section.characteristics & kRead) == 0 ||
            (section.characteristics & (kExecute | kWrite)) == (kExecute | kWrite)) {
            return Reject("Sections must be readable and cannot be writable and executable.");
        }
        // The diagnostic has no COFF relocations or line-number records.
        if (U32(entry + 24) != 0 || U32(entry + 28) != 0 ||
            U16(entry + 32) != 0 || U16(entry + 34) != 0) {
            return Reject("COFF section metadata is unsupported.");
        }
        for (const auto& previous : image.sections) {
            const auto previousSpan = AlignUp(std::max(previous.virtualSize, previous.rawSize), sectionAlignment);
            if (Overlap(section.virtualAddress, protectedSpan, previous.virtualAddress, previousSpan) ||
                Overlap(section.rawOffset, section.rawSize, previous.rawOffset, previous.rawSize)) {
                return Reject("Section virtual or raw ranges overlap.");
            }
        }
        const std::uint64_t entryOffset = image.entryPointRva >= section.virtualAddress
            ? static_cast<std::uint64_t>(image.entryPointRva) - section.virtualAddress
            : std::numeric_limits<std::uint64_t>::max();
        if ((section.characteristics & kExecute) != 0 && entryOffset < section.rawSize &&
            entryOffset < section.virtualSize) {
            executableEntry = true;
        }
        image.sections.push_back(section);
    }
    if (!executableEntry) return Reject("Entry point is not in executable, file-backed section data.");
    if (lastRawEnd != size) return Reject("File overlays are unsupported.");
    PeParseResult result;
    result.ok = true;
    result.image = std::move(image);
    result.message = "Valid minimal PE32+ x64 diagnostic profile.";
    return result;
}

} // namespace

PeParseResult ParseMinimalPe(const std::uint8_t* bytes, std::size_t size) noexcept {
    try {
        return Parse(bytes, size);
    } catch (...) {
        PeParseResult result;
        // Keep the noexcept boundary even when allocating an error string fails.
        try { result.message = "Unable to allocate parser state."; } catch (...) {}
        return result;
    }
}

} // namespace xcdm::runtime
