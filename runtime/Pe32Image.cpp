#include "Pe32Image.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace xcdm::runtime {
namespace {
constexpr std::size_t MaxImage = 64U * 1024U * 1024U;
struct Invalid { const char* text; };
[[noreturn]] void Fail(const char* text) { throw Invalid{text}; }
bool Range(std::size_t at, std::size_t length, std::size_t total) {
    return at <= total && length <= total - at;
}
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    std::uint16_t U16(std::size_t at) const {
        if (!Range(at, 2, size_)) Fail("Truncated PE32 word.");
        return static_cast<std::uint16_t>(data_[at] | (static_cast<unsigned>(data_[at + 1]) << 8));
    }
    std::uint32_t U32(std::size_t at) const {
        if (!Range(at, 4, size_)) Fail("Truncated PE32 dword.");
        return static_cast<std::uint32_t>(U16(at)) | (static_cast<std::uint32_t>(U16(at + 2)) << 16);
    }
private:
    const std::uint8_t* data_;
    std::size_t size_;
};

bool InSection(const Pe32Image& image, std::uint32_t rva, std::size_t size, bool writable = false) {
    if (!Range(rva, size, image.bytes.size())) return false;
    for (const auto& section : image.sections)
        if (rva >= section.rva && Range(rva - section.rva, size, section.size) &&
            section.readable && (!writable || section.writable)) return true;
    return false;
}

std::string Name(const Pe32Image& image, std::uint32_t rva) {
    std::string name;
    for (std::uint32_t n = 0; n < 512; ++n) {
        const auto at = static_cast<std::uint64_t>(rva) + n;
        if (at > UINT32_MAX || !InSection(image, static_cast<std::uint32_t>(at), 1))
            Fail("Import string is outside a readable guest section.");
        const auto c = image.bytes[static_cast<std::size_t>(at)];
        if (!c) { if (name.empty()) Fail("Empty PE32 import name."); return name; }
        if (c < 0x20 || c > 0x7e) Fail("Import name is not bounded printable ASCII.");
        name.push_back(static_cast<char>(c));
    }
    Fail("Import string exceeds 511 characters.");
}

Pe32Image Map(const std::uint8_t* data, std::size_t size) {
    if (!data || size < 64 || size > MaxImage) Fail("PE32 file size is outside the 64-byte..64-MiB profile.");
    const Reader file(data, size);
    if (file.U16(0) != 0x5a4d) Fail("Missing MZ signature.");
    const std::size_t pe = file.U32(0x3c);
    if (!Range(pe, 24, size) || file.U32(pe) != 0x4550) Fail("Missing or truncated PE signature.");
    if (file.U16(pe + 4) != 0x14c) Fail("The guest loader requires i386 PE32.");
    const auto count = file.U16(pe + 6);
    if (!count || count > 96) Fail("PE32 section count must be 1..96.");
    const auto characteristics = file.U16(pe + 22);
    if (!(characteristics & 2) || (characteristics & 0x2000)) Fail("Only executable PE32 images are supported, not DLLs.");
    const auto optionalSize = file.U16(pe + 20);
    const auto optional = pe + 24;
    if (optionalSize < 96 || !Range(optional, optionalSize, size) || file.U16(optional) != 0x10b)
        Fail("Missing or truncated PE32 optional header.");
    const auto directories = file.U32(optional + 92);
    if (directories > 16 || static_cast<std::size_t>(directories) * 8 > optionalSize - 96U)
        Fail("PE32 directory count exceeds its optional header.");
    if (file.U32(optional + 32) != 4096 || file.U32(optional + 36) != 512)
        Fail("This guest profile requires 4-KiB sections and 512-byte file alignment.");
    Pe32Image image;
    image.base = file.U32(optional + 28);
    image.headerSize = file.U32(optional + 60);
    const auto imageSize = file.U32(optional + 56);
    const auto entryRva = file.U32(optional + 16);
    if (image.base < 0x10000 || (image.base & 0xffff) || !imageSize || imageSize > MaxImage || (imageSize & 4095) ||
        static_cast<std::uint64_t>(image.base) + imageSize > 0x60000000ULL)
        Fail("PE32 image base or size is outside the bounded guest address profile.");
    if (!image.headerSize || (image.headerSize & 511) || image.headerSize > size || image.headerSize > imageSize)
        Fail("Invalid PE32 header size.");
    const auto table = optional + optionalSize;
    if (!Range(table, static_cast<std::size_t>(count) * 40, image.headerSize))
        Fail("PE32 section table is outside SizeOfHeaders.");
    std::uint32_t importRva = 0, importSize = 0;
    for (std::uint32_t n = 0; n < directories; ++n) {
        const auto rva = file.U32(optional + 96 + n * 8);
        const auto length = file.U32(optional + 100 + n * 8);
        if (!rva && !length) continue;
        if (!rva || !length) Fail("Inconsistent PE32 directory address and size.");
        if (n == 1) { importRva = rva; importSize = length; }
        else if (n != 2 && n != 6 && n != 12)
            Fail("PE32 directory is unsupported by this initial loader (including TLS, relocations and delay imports).");
        if (!Range(rva, length, imageSize)) Fail("PE32 directory exceeds SizeOfImage.");
    }
    struct Raw { std::uint32_t at, size; };
    std::vector<Raw> raw;
    for (std::uint32_t n = 0; n < count; ++n) {
        const auto at = table + n * 40;
        const auto virtualSize = file.U32(at + 8), rva = file.U32(at + 12);
        const auto rawSize = file.U32(at + 16), rawAt = file.U32(at + 20);
        const auto flags = file.U32(at + 36);
        const auto span = std::max(virtualSize, rawSize);
        if (!span || !rva || (rva & 4095) || rva < image.headerSize || !Range(rva, span, imageSize))
            Fail("Invalid PE32 section virtual range.");
        if (rawSize && ((rawAt & 511) || (rawSize & 511) || rawAt < image.headerSize || !Range(rawAt, rawSize, size)))
            Fail("Invalid PE32 section raw range.");
        Pe32Section section{rva, span, (flags & 0x40000000) != 0, (flags & 0x80000000) != 0, (flags & 0x20000000) != 0};
        if (!section.readable || (section.writable && section.executable))
            Fail("Guest sections require read access and cannot be writable and executable together.");
        for (std::size_t i = 0; i < image.sections.size(); ++i) {
            const auto& other = image.sections[i];
            if (static_cast<std::uint64_t>(rva) < static_cast<std::uint64_t>(other.rva) + other.size &&
                static_cast<std::uint64_t>(other.rva) < static_cast<std::uint64_t>(rva) + span)
                Fail("Overlapping PE32 virtual sections.");
            if (rawSize && raw[i].size && static_cast<std::uint64_t>(rawAt) < static_cast<std::uint64_t>(raw[i].at) + raw[i].size &&
                static_cast<std::uint64_t>(raw[i].at) < static_cast<std::uint64_t>(rawAt) + rawSize)
                Fail("Overlapping PE32 raw sections.");
        }
        image.sections.push_back(section);
        raw.push_back({rawAt, rawSize});
    }
    bool executableEntry = false;
    for (const auto& section : image.sections)
        if (section.executable && entryRva >= section.rva && entryRva - section.rva < section.size) executableEntry = true;
    if (!executableEntry) Fail("PE32 entry point is not in an executable guest section.");
    image.entry = image.base + entryRva;
    image.bytes.resize(imageSize, 0);
    std::memcpy(image.bytes.data(), data, image.headerSize);
    for (std::size_t i = 0; i < raw.size(); ++i)
        if (raw[i].size) std::memcpy(image.bytes.data() + image.sections[i].rva, data + raw[i].at, raw[i].size);
    if (!importRva) return image;
    if (importSize < 20 || !InSection(image, importRva, importSize)) Fail("Invalid PE32 import descriptor directory.");
    const Reader mapped(image.bytes.data(), image.bytes.size());
    std::vector<std::uint32_t> boundIats;
    bool ended = false;
    for (std::uint32_t n = 0; n < 257; ++n) {
        const auto offset = static_cast<std::size_t>(n) * 20;
        if (!Range(offset, 20, importSize)) break;
        const auto at = static_cast<std::size_t>(importRva) + offset;
        const auto lookup = mapped.U32(at), timestamp = mapped.U32(at + 4), chain = mapped.U32(at + 8);
        const auto libraryRva = mapped.U32(at + 12), iat = mapped.U32(at + 16);
        if (!(lookup | timestamp | chain | libraryRva | iat)) { ended = true; break; }
        if (n == 256) Fail("PE32 import library count exceeds 256.");
        if (!libraryRva || !iat) Fail("PE32 import descriptor has no library or IAT.");
        const auto library = Name(image, libraryRva);
        const auto tableRva = lookup ? lookup : iat;
        bool thunksEnded = false;
        for (std::uint32_t t = 0; t <= 8192; ++t) {
            const auto source = static_cast<std::uint64_t>(tableRva) + t * 4ULL;
            const auto target = static_cast<std::uint64_t>(iat) + t * 4ULL;
            if (source > UINT32_MAX || target > UINT32_MAX ||
                !InSection(image, static_cast<std::uint32_t>(source), 4) ||
                !InSection(image, static_cast<std::uint32_t>(target), 4, true))
                Fail("PE32 lookup or IAT leaves its guest section.");
            const auto value = mapped.U32(static_cast<std::size_t>(source));
            if (!value) { thunksEnded = true; break; }
            if (image.imports.size() >= 8192) Fail("PE32 import count exceeds 8192.");
            const auto target32 = static_cast<std::uint32_t>(target);
            if (std::find(boundIats.begin(), boundIats.end(), target32) != boundIats.end()) Fail("Duplicate PE32 import slot.");
            boundIats.push_back(target32);
            Pe32Import imported{library, {}, 0, (value & 0x80000000U) != 0, target32};
            if (imported.byOrdinal) {
                if (value & 0x7fff0000U) Fail("Malformed PE32 ordinal import.");
                imported.ordinal = static_cast<std::uint16_t>(value);
            } else {
                if (!InSection(image, value, 2) || value > UINT32_MAX - 2) Fail("Invalid PE32 import-by-name hint.");
                imported.name = Name(image, value + 2);
            }
            image.imports.push_back(std::move(imported));
        }
        if (!thunksEnded) Fail("Unterminated PE32 import thunk table.");
    }
    if (!ended) Fail("Unterminated PE32 import descriptor table.");
    return image;
}
} // namespace

Pe32LoadResult LoadPe32Image(const std::uint8_t* bytes, std::size_t size) noexcept {
    try { return {true, "PE32 guest image mapped; no code executed.", Map(bytes, size)}; }
    catch (const Invalid& error) {
        try { return {false, error.text, {}}; } catch (...) { return {}; }
    } catch (...) { return {}; }
}
} // namespace xcdm::runtime
