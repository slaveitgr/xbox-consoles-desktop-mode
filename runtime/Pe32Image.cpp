#include "Pe32Image.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

namespace xcdm::runtime {
namespace {
constexpr std::size_t MaxImage = 64U * 1024U * 1024U;
struct Invalid { std::string text; };
[[noreturn]] void Fail(std::string text) { throw Invalid{std::move(text)}; }
std::string HexRva(std::uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}
std::string DirectoryDescription(std::uint32_t index, std::uint32_t address,
                                 std::uint32_t size) {
    static constexpr const char* names[] = {
        "EXPORT", "IMPORT", "RESOURCE", "EXCEPTION", "SECURITY", "BASERELOC",
        "DEBUG", "ARCHITECTURE", "GLOBALPTR", "TLS", "LOAD_CONFIG",
        "BOUND_IMPORT", "IAT", "DELAY_IMPORT", "CLR", "RESERVED"
    };
    return "PE32 " + std::string(names[index]) + " directory " + std::to_string(index) +
        " (" + (index == 4 ? "file offset " : "RVA ") + HexRva(address) +
        ", size " + HexRva(size) + ")";
}
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
            Fail("PE32 import string byte RVA " + HexRva(at) + " (start " + HexRva(rva) +
                 ") is outside a readable guest section.");
        const auto c = image.bytes[static_cast<std::size_t>(at)];
        if (!c) { if (name.empty()) Fail("Empty PE32 import name."); return name; }
        if (c < 0x20 || c > 0x7e) Fail("Import name is not bounded printable ASCII.");
        name.push_back(static_cast<char>(c));
    }
    Fail("Import string exceeds 511 characters.");
}

class ImportCollector {
public:
    ImportCollector(Pe32Image& image, Pe32Profile profile)
        : image_(image), mapped_(image.bytes.data(), image.bytes.size()),
          requireWritableIat_(profile == Pe32Profile::Fixture) {}

    void Collect(std::uint32_t rva, std::uint32_t size, bool delayed) {
        if (!rva) return;
        const std::size_t width = delayed ? 32 : 20;
        if (size < width || !InSection(image_, rva, size))
            Fail(delayed ? "Invalid PE32 delay-import descriptor directory." : "Invalid PE32 import descriptor directory.");
        for (std::size_t offset = 0; Range(offset, width, size); offset += width) {
            const auto at = static_cast<std::size_t>(rva) + offset;
            std::uint32_t fields = 0;
            for (std::size_t n = 0; n < width; n += 4) fields |= mapped_.U32(at + n);
            if (!fields) return;
            if (libraries_ == 256) Fail("PE32 import library count exceeds the shared 256-descriptor limit.");
            ++libraries_;
            std::uint32_t libraryRva = 0, iat = 0, lookup = 0;
            bool legacyVa = false;
            if (delayed) {
                const auto attributes = mapped_.U32(at);
                if (attributes & ~1U) Fail("PE32 delay-import descriptor has reserved attribute bits.");
                legacyVa = (attributes & 1U) == 0;
                libraryRva = AddressRva(mapped_.U32(at + 4), legacyVa);
                iat = AddressRva(mapped_.U32(at + 12), legacyVa);
                lookup = AddressRva(mapped_.U32(at + 16), legacyVa);
                // A delay IAT initially contains linker-generated helper
                // addresses, so it cannot stand in for a missing name table.
                if (!libraryRva || !iat || !lookup)
                    Fail("PE32 delay-import descriptor requires a DLL name, lookup table and IAT.");
            } else {
                lookup = mapped_.U32(at);
                libraryRva = mapped_.U32(at + 12);
                iat = mapped_.U32(at + 16);
                if (!libraryRva || !iat) Fail("PE32 import descriptor has no library or IAT.");
                if (!lookup) lookup = iat;
            }
            const auto library = Name(image_, libraryRva);
            const auto tableSize = Thunks(lookup, iat, library, delayed, legacyVa);
            if (delayed) {
                // These tables are not followed or modified, but ensure every
                // declared pointer covers its bounded table in guest memory.
                const auto module = AddressRva(mapped_.U32(at + 8), legacyVa);
                if (module && !InSection(image_, module, 4, true))
                    Fail("PE32 delay-import module handle RVA " + HexRva(module) +
                         " requires a readable/writable 4-byte guest section range.");
                for (const auto field : {20U, 24U}) {
                    const auto table = AddressRva(mapped_.U32(at + field), legacyVa);
                    if (table && !InSection(image_, table, tableSize))
                        Fail("PE32 delay-import ancillary table RVA " + HexRva(table) +
                             " does not cover its table and terminator in readable guest memory.");
                }
            }
        }
        Fail(delayed ? "Unterminated PE32 delay-import descriptor table." : "Unterminated PE32 import descriptor table.");
    }

private:
    std::uint32_t AddressRva(std::uint32_t value, bool legacyVa) const {
        if (!value) return 0;
        if (legacyVa) {
            if (value < image_.base) Fail("PE32 legacy delay-import VA is below the image base.");
            value -= image_.base;
        }
        return value;
    }

    std::size_t Thunks(std::uint32_t lookup, std::uint32_t iat,
                       const std::string& library, bool delayed, bool legacyVa) {
        for (std::uint32_t t = 0; t <= 8192; ++t) {
            const auto source = static_cast<std::uint64_t>(lookup) + t * 4ULL;
            const auto target = static_cast<std::uint64_t>(iat) + t * 4ULL;
            const auto locations = [source, target] {
                return "lookup source RVA " + HexRva(source) + ", IAT target RVA " + HexRva(target);
            };
            if (source > UINT32_MAX || !InSection(image_, static_cast<std::uint32_t>(source), 4))
                Fail("PE32 " + locations() + ": lookup requires a readable 4-byte guest section range.");
            if (target > UINT32_MAX || !InSection(image_, static_cast<std::uint32_t>(target), 4))
                Fail("PE32 " + locations() + ": IAT target requires a readable 4-byte guest section range.");
            // A loader may initialize imports before the guest starts even
            // when the PE declares the IAT read-only. StartupTrace only owns
            // byte-vector data; accepting these slots never changes guest
            // store permissions or grants native executable memory access.
            if (requireWritableIat_ && !InSection(image_, static_cast<std::uint32_t>(target), 4, true))
                Fail("PE32 " + locations() + ": IAT target requires writable guest memory in the strict fixture profile.");
            const auto value = mapped_.U32(static_cast<std::size_t>(source));
            const auto target32 = static_cast<std::uint32_t>(target);
            for (const auto previous : boundIats_)
                if (target < static_cast<std::uint64_t>(previous) + 4 &&
                    previous < target + 4)
                    Fail("Overlapping PE32 import slots.");
            boundIats_.push_back(target32);
            if (!value) return static_cast<std::size_t>(t + 1) * 4;
            if (image_.imports.size() == 8192) Fail("PE32 import count exceeds the shared 8192-entry limit.");
            Pe32Import imported{library, {}, 0, (value & 0x80000000U) != 0, target32, delayed};
            if (imported.byOrdinal) {
                if (value & 0x7fff0000U) Fail("Malformed PE32 ordinal import.");
                imported.ordinal = static_cast<std::uint16_t>(value);
            } else {
                const auto nameRva = AddressRva(value, legacyVa);
                if (!InSection(image_, nameRva, 2) || nameRva > UINT32_MAX - 2)
                    Fail("PE32 import-by-name hint RVA " + HexRva(nameRva) +
                         " requires a readable 2-byte guest section range (" + locations() + ").");
                imported.name = Name(image_, nameRva + 2);
            }
            image_.imports.push_back(std::move(imported));
        }
        Fail("Unterminated PE32 import thunk table.");
    }

    Pe32Image& image_;
    Reader mapped_;
    bool requireWritableIat_;
    std::size_t libraries_ = 0;
    std::vector<std::uint32_t> boundIats_;
};

Pe32Image Map(const std::uint8_t* data, std::size_t size, Pe32Profile profile) {
    if (profile != Pe32Profile::Fixture && profile != Pe32Profile::StartupTrace)
        Fail("Unknown PE32 loading profile.");
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
    std::uint32_t importRva = 0, importSize = 0, delayRva = 0, delaySize = 0;
    for (std::uint32_t n = 0; n < directories; ++n) {
        const auto rva = file.U32(optional + 96 + n * 8);
        const auto length = file.U32(optional + 100 + n * 8);
        if (!rva && !length) continue;
        if (!rva || !length)
            Fail(DirectoryDescription(n, rva, length) + ": inconsistent address and size.");
        if (n == 4 && profile == Pe32Profile::StartupTrace) {
            // Unlike the other data directories, SECURITY refers to input
            // file bytes, never an RVA. Validate after section raw ranges are
            // known; do not apply SizeOfImage or copy this table into memory.
            image.certificateFileOffset = rva;
            image.certificateSize = length;
            continue;
        }
        if (n == 1) { importRva = rva; importSize = length; }
        else if (n == 13 && profile == Pe32Profile::StartupTrace) { delayRva = rva; delaySize = length; }
        else if (n != 2 && n != 6 && n != 12)
            Fail(DirectoryDescription(n, rva, length) + ": unsupported by the " +
                 (profile == Pe32Profile::Fixture ? "strict fixture" : "startup trace") + " profile.");
        if (!Range(rva, length, imageSize))
            Fail(DirectoryDescription(n, rva, length) + ": range exceeds SizeOfImage.");
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
        if (!section.readable)
            Fail("Guest section does not declare read access.");
        // Interpreter sections are ordinary host byte vectors. A guest W+X
        // flag never creates executable/writable host memory. Keep the stricter
        // fixture profile while allowing a real PE's self-modifying byte data
        // in the bounded interpreter; the native x64 path remains W^X.
        if (section.writable && section.executable && profile == Pe32Profile::Fixture)
            Fail("The strict fixture profile rejects writable/executable guest sections.");
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
    if (image.certificateSize) {
        const auto certificateAt = image.certificateFileOffset;
        const auto certificateSize = image.certificateSize;
        const auto description = DirectoryDescription(4, certificateAt, certificateSize);
        if ((certificateAt & 7U) || (certificateSize & 7U))
            Fail(description + ": file offset and size must be 8-byte aligned.");
        if (!Range(certificateAt, certificateSize, size))
            Fail(description + ": certificate file range exceeds the input file.");
        if (certificateAt < image.headerSize)
            Fail(description + ": certificate table overlaps PE headers.");
        const auto certificateEnd = static_cast<std::uint64_t>(certificateAt) + certificateSize;
        for (std::size_t n = 0; n < raw.size(); ++n) {
            if (raw[n].size && certificateAt < static_cast<std::uint64_t>(raw[n].at) + raw[n].size &&
                raw[n].at < certificateEnd)
                Fail(description + ": certificate table overlaps raw section " + std::to_string(n) +
                     " at file offset " + HexRva(raw[n].at) + ", size " + HexRva(raw[n].size) + ".");
        }
        std::size_t consumed = 0;
        while (consumed < certificateSize) {
            if (image.certificateCount == 256)
                Fail(description + ": certificate record count exceeds the 256-record limit.");
            const auto at = static_cast<std::size_t>(certificateAt) + consumed;
            const auto record = description + " record at file offset " + HexRva(at);
            if (!Range(consumed, 8, certificateSize))
                Fail(record + ": truncated WIN_CERTIFICATE header.");
            const auto length = file.U32(at);
            if (length < 8)
                Fail(record + ": WIN_CERTIFICATE dwLength must be at least 8 bytes.");
            const auto rounded = (static_cast<std::uint64_t>(length) + 7U) & ~std::uint64_t{7};
            if (rounded > certificateSize - consumed)
                Fail(record + ": rounded WIN_CERTIFICATE length exceeds the certificate table.");
            const auto revision = file.U16(at + 4), type = file.U16(at + 6);
            if (revision != 0x0200 || type != 0x0002)
                Fail(record + ": unsupported certificate revision " + HexRva(revision) +
                     " or type " + HexRva(type) + "; this structural profile accepts revision 0x200/type 0x2 only.");
            // The payload is deliberately opaque: this is a structural file
            // check, not PKCS#7 parsing, signature verification or trust.
            for (std::size_t n = length; n < rounded; ++n)
                if (data[at + n] != 0)
                    Fail(record + ": WIN_CERTIFICATE alignment padding must be zero.");
            consumed += static_cast<std::size_t>(rounded);
            ++image.certificateCount;
        }
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
    if (importRva && delayRva &&
        static_cast<std::uint64_t>(importRva) < static_cast<std::uint64_t>(delayRva) + delaySize &&
        static_cast<std::uint64_t>(delayRva) < static_cast<std::uint64_t>(importRva) + importSize)
        Fail("Normal and delay-import descriptor directories overlap.");
    ImportCollector imports(image, profile);
    imports.Collect(importRva, importSize, false);
    imports.Collect(delayRva, delaySize, true);
    return image;
}
} // namespace

Pe32LoadResult LoadPe32Image(const std::uint8_t* bytes, std::size_t size, Pe32Profile profile) noexcept {
    try { return {true, "PE32 guest image mapped; no code executed.", Map(bytes, size, profile)}; }
    catch (const Invalid& error) {
        try { return {false, error.text, {}}; } catch (...) { return {}; }
    } catch (...) { return {}; }
}
} // namespace xcdm::runtime
