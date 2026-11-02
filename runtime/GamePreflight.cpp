#include "GamePreflight.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace xcdm::runtime {
namespace {

constexpr std::size_t kMaxFile = 64U * 1024U * 1024U;
constexpr std::size_t kMaxSections = 96;
constexpr std::size_t kMaxLibraries = 256;
constexpr std::size_t kMaxImports = 8192;
constexpr std::size_t kMaxName = 512;
constexpr std::size_t kMaxReport = 512U * 1024U;

struct ParseError { const char* message; };

[[noreturn]] void Fail(const char* message) { throw ParseError{message}; }

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

bool PowerOfTwo(std::uint32_t value) noexcept {
    return value && !(value & (value - 1U));
}

bool Overlap(std::uint64_t a, std::uint64_t an,
             std::uint64_t b, std::uint64_t bn) noexcept {
    return an && bn && a < b + bn && b < a + an;
}

std::string Hex(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    char buffer[18];
    std::size_t offset = sizeof(buffer);
    do { buffer[--offset] = digits[value & 15U]; value >>= 4U; } while (value);
    buffer[--offset] = 'x';
    buffer[--offset] = '0';
    return std::string(buffer + offset, sizeof(buffer) - offset);
}

std::string Lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}

const char* Subsystem(std::uint16_t value) noexcept {
    switch (value) {
    case 1: return "Native";
    case 2: return "Windows GUI";
    case 3: return "Windows console";
    case 9: return "Windows CE GUI";
    case 10: return "EFI application";
    case 11: return "EFI boot driver";
    case 12: return "EFI runtime driver";
    case 14: return "Xbox";
    case 16: return "Windows boot application";
    default: return "Other/unspecified";
    }
}

struct Section {
    std::uint32_t address;
    std::uint32_t virtualSize;
    std::uint32_t rawOffset;
    std::uint32_t rawSize;
};

struct Directory { std::uint32_t address = 0, size = 0; };
struct Mapping { const std::uint8_t* bytes; std::size_t available; };

class Inspector {
public:
    Inspector(const std::uint8_t* bytes, std::size_t size) : bytes_(bytes), size_(size) {}

    GamePreflightResult Run() {
        Headers();
        Line("Read-only PE metadata inspection. No guest code was executed.");
        Line("This is not a full PE loader, dependency resolver or compatibility test.");
        Line("File size: " + std::to_string(size_) + " bytes");
        Line("Format: " + std::string(is64_ ? "PE32+" : "PE32") + "; architecture: " + architecture_ +
             " (machine " + Hex(machine_) + ")");
        Line("Image type: " + std::string(isDll_ ? "DLL" : "executable"));
        Line("Subsystem: " + std::string(Subsystem(subsystem_)) + " (" + std::to_string(subsystem_) + ")");
        Line("Entry point RVA: " + Hex(entry_) + "; preferred image base: " + Hex(imageBase_));
        Line("Virtual image size: " + std::to_string(imageSize_) + "; sections: " + std::to_string(sections_.size()));
        Presence(9, is64_ ? 40 : 24, "TLS");
        Presence(5, 8, "Base relocation");
        if (directories_[14].address) Line("CLR metadata: present (not parsed).");
        Line("");
        Imports(false);
        Imports(true);
        Line("");
        Line("Imported libraries: " + std::to_string(libraryCount_) +
             "; named/ordinal imports inspected: " + std::to_string(importCount_));
        Line("Graphics indicators (imports only): " + Graphics());
        Line("LoadLibrary/GetProcAddress, plugins and forwarded/transitive DLL imports can add dependencies not listed here.");
        if (dynamicLoad_) Line("Dynamic loading API imports were found.");
        Line("");
        Line("Current Xbox host gaps:");
        if (machine_ == 0x14cU)
            Line("- This is 32-bit x86. The separate integer interpreter currently runs only embedded diagnostics, not this game.");
        else if (machine_ != 0x8664U)
            Line("- This architecture cannot be run by the current x64 diagnostic runtime.");
        else
            Line("- x64 metadata does not establish compatibility with the diagnostic's minimal execution profile.");
        Line("- A general Win32/CRT game API layer is not connected to this inspector.");
        if (d3d9_) Line("- Direct3D 9 imports require a guest D3D9 implementation; the current host does not provide one.");
        if (d3d8_ || d3d10_ || d3d11_ || d3d12_ || dxgi_ || openGl_ || directDraw_)
            Line("- Imported graphics APIs require their own guest API and presentation integration.");
        Line("- Audio, input, file access, threads and DLL dependencies still require runtime integration.");
        Line("No DLL was loaded, no TLS callback was called and no image entry point was invoked.");
        if (reportTruncated_) report_ += "\n[Report output truncated at the inspection limit; remaining imports were still checked.]\n";

        GamePreflightResult result;
        result.ok = true;
        result.summary = std::string(is64_ ? "PE32+ " : "PE32 ") + architecture_ +
                         " metadata inspected; no execution performed.";
        result.report = std::move(report_);
        return result;
    }

private:
    void Line(const std::string& text) {
        if (reportTruncated_) return;
        if (text.size() + 1 > kMaxReport - report_.size()) { reportTruncated_ = true; return; }
        report_ += text;
        report_ += '\n';
    }

    Mapping Map(std::uint32_t rva, std::size_t need) const {
        if (rva >= imageSize_ || need > static_cast<std::uint64_t>(imageSize_) - rva)
            Fail("Metadata RVA exceeds SizeOfImage.");
        if (rva < headerSize_) {
            const std::size_t available = std::min<std::size_t>(headerSize_ - rva, size_ - rva);
            if (need > available) Fail("Metadata crosses the file-backed header boundary.");
            return {bytes_ + rva, available};
        }
        for (const auto& section : sections_) {
            if (rva < section.address) continue;
            const std::uint64_t delta = static_cast<std::uint64_t>(rva) - section.address;
            if (delta >= section.rawSize) continue;
            const auto available = static_cast<std::size_t>(section.rawSize - delta);
            if (need > available) Fail("Metadata crosses the file-backed section boundary.");
            return {bytes_ + section.rawOffset + static_cast<std::size_t>(delta), available};
        }
        Fail("Metadata RVA has no file-backed section data.");
    }

    std::string Name(std::uint32_t rva, std::size_t skip = 0) const {
        const Mapping mapped = Map(rva, skip + 1);
        const std::size_t limit = std::min(kMaxName, mapped.available - skip);
        std::size_t length = 0;
        for (; length < limit; ++length) {
            const auto c = mapped.bytes[skip + length];
            if (!c) {
                if (!length) Fail("An import name is empty.");
                return std::string(reinterpret_cast<const char*>(mapped.bytes + skip), length);
            }
            if (c < 0x20 || c > 0x7e) Fail("An import name contains non-ASCII or control bytes.");
        }
        Fail("An import name is unterminated or exceeds the 511-character inspection limit.");
    }

    std::uint32_t AddRva(std::uint32_t address, std::size_t offset) const {
        if (offset > std::numeric_limits<std::uint32_t>::max() - address)
            Fail("Import table RVA arithmetic overflow.");
        return address + static_cast<std::uint32_t>(offset);
    }

    std::uint32_t AddressRva(std::uint64_t value, bool isVa) const {
        if (isVa) {
            if (value < imageBase_) Fail("A legacy delay-import VA is below the image base.");
            value -= imageBase_;
        }
        if (value > std::numeric_limits<std::uint32_t>::max()) Fail("Import address does not fit an RVA.");
        return static_cast<std::uint32_t>(value);
    }

    void Headers() {
        if (size_ > kMaxFile) Fail("Input exceeds the 64 MiB inspection limit.");
        if (!bytes_ || !Range(0, 64, size_)) Fail("Missing or truncated DOS header.");
        if (U16(bytes_) != 0x5a4dU) Fail("Invalid DOS signature.");
        const std::size_t pe = U32(bytes_ + 0x3c);
        if (pe < 64 || !Range(pe, 24, size_)) Fail("Invalid PE header offset.");
        if (U32(bytes_ + pe) != 0x4550U) Fail("Invalid PE signature.");
        const auto* coff = bytes_ + pe + 4;
        machine_ = U16(coff);
        const auto count = U16(coff + 2);
        const auto optionalSize = U16(coff + 16);
        const auto characteristics = U16(coff + 18);
        if (!(characteristics & 2U)) Fail("COFF header does not describe an executable image.");
        isDll_ = (characteristics & 0x2000U) != 0;
        if (!count || count > kMaxSections) Fail("Section count is outside the 1..96 inspection limit.");
        if (!Range(pe + 24, optionalSize, size_) || optionalSize < 2) Fail("Truncated optional header.");
        const auto* optional = bytes_ + pe + 24;
        const auto magic = U16(optional);
        if (magic != 0x10bU && magic != 0x20bU) Fail("Only PE32 and PE32+ optional headers can be inspected.");
        is64_ = magic == 0x20bU;
        const std::size_t directoryOffset = is64_ ? 112 : 96;
        if (optionalSize < directoryOffset) Fail("Optional header lacks required standard fields.");
        if ((machine_ == 0x14cU && is64_) || (machine_ == 0x8664U && !is64_))
            Fail("Machine architecture and optional-header width disagree.");
        switch (machine_) {
        case 0x14c: architecture_ = "x86"; break;
        case 0x8664: architecture_ = "x64"; break;
        case 0xaa64: architecture_ = "ARM64"; break;
        case 0x1c0: case 0x1c2: case 0x1c4: architecture_ = "ARM"; break;
        default: architecture_ = "other machine " + Hex(machine_); break;
        }
        entry_ = U32(optional + 16);
        imageBase_ = is64_ ? U64(optional + 24) : U32(optional + 28);
        imageSize_ = U32(optional + 56);
        headerSize_ = U32(optional + 60);
        subsystem_ = U16(optional + 68);
        const auto sectionAlignment = U32(optional + 32);
        const auto fileAlignment = U32(optional + 36);
        if (!PowerOfTwo(sectionAlignment) || !PowerOfTwo(fileAlignment) ||
            sectionAlignment < fileAlignment ||
            (sectionAlignment < 4096 ? sectionAlignment != fileAlignment :
             (fileAlignment < 512 || fileAlignment > 65536)))
            Fail("Invalid section/file alignment.");
        if (!imageSize_ || !headerSize_ || headerSize_ > size_ || headerSize_ > imageSize_ ||
            imageSize_ % sectionAlignment || headerSize_ % fileAlignment)
            Fail("Invalid image or header size.");
        if (imageBase_ > std::numeric_limits<std::uint64_t>::max() - imageSize_)
            Fail("Preferred image address range overflows.");
        if (!is64_ && (imageBase_ > 0xffffffffULL || imageSize_ > 0x100000000ULL - imageBase_))
            Fail("PE32 preferred image exceeds its 32-bit address space.");

        const std::size_t table = pe + 24 + optionalSize;
        const std::size_t tableSize = static_cast<std::size_t>(count) * 40;
        if (!Range(table, tableSize, headerSize_)) Fail("Section table exceeds SizeOfHeaders.");
        sections_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto* at = bytes_ + table + i * 40;
            const Section section{U32(at + 12), U32(at + 8), U32(at + 20), U32(at + 16)};
            const std::uint64_t span = std::max(section.virtualSize, section.rawSize);
            if (span && (section.address < headerSize_ || section.address % sectionAlignment ||
                         section.address >= imageSize_ || span > imageSize_ - section.address))
                Fail("Section virtual range is invalid or exceeds SizeOfImage.");
            if (section.rawSize && (section.rawOffset < headerSize_ ||
                                    section.rawOffset % fileAlignment ||
                                    !Range(section.rawOffset, section.rawSize, size_)))
                Fail("Section raw data is invalid or truncated.");
            for (const auto& previous : sections_) {
                if (Overlap(section.address, span, previous.address,
                            std::max(previous.virtualSize, previous.rawSize)) ||
                    Overlap(section.rawOffset, section.rawSize, previous.rawOffset, previous.rawSize))
                    Fail("Section virtual or raw ranges overlap.");
            }
            sections_.push_back(section);
        }
        if (entry_) (void)Map(entry_, 1);
        const auto directoryCount = U32(optional + directoryOffset - 4);
        if (directoryCount > directories_.size() ||
            directoryCount > (optionalSize - directoryOffset) / 8)
            Fail("Data-directory count exceeds the optional header or inspection limit.");
        for (std::size_t i = 0; i < directoryCount; ++i) {
            const auto* at = optional + directoryOffset + i * 8;
            directories_[i] = {U32(at), U32(at + 4)};
            const auto directory = directories_[i];
            if (!directory.address && directory.size) Fail("A data directory has a size but no address.");
            if (i == 4) { // The certificate directory stores a file offset, not an RVA.
                if (directory.size && !Range(directory.address, directory.size, size_))
                    Fail("Certificate directory exceeds the file.");
            } else if (directory.address &&
                       (directory.address >= imageSize_ || directory.size > imageSize_ - directory.address)) {
                Fail("A data directory exceeds SizeOfImage.");
            }
        }
    }

    void Presence(std::size_t index, std::size_t minimum, const char* name) {
        const auto directory = directories_[index];
        if (!directory.address) { Line(std::string(name) + " directory: absent."); return; }
        if (directory.size < minimum) Fail("TLS or relocation directory is truncated.");
        (void)Map(directory.address, directory.size);
        Line(std::string(name) + " directory: present (contents not executed or fully validated).");
    }

    void DetectLibrary(const std::string& name) {
        const std::string lower = Lower(name);
        d3d8_ |= lower == "d3d8.dll";
        d3d9_ |= lower == "d3d9.dll";
        d3d10_ |= lower == "d3d10.dll" || lower == "d3d10_1.dll" || lower == "d3d10core.dll";
        d3d11_ |= lower == "d3d11.dll";
        d3d12_ |= lower == "d3d12.dll";
        dxgi_ |= lower == "dxgi.dll";
        openGl_ |= lower == "opengl32.dll";
        directDraw_ |= lower == "ddraw.dll";
    }

    void Thunks(std::uint32_t lookup, std::uint32_t iat, bool legacyVa, bool boundIat) {
        if (!lookup || !iat) Fail("Import descriptor has no lookup or address table.");
        const std::size_t width = is64_ ? 8 : 4;
        const std::uint64_t ordinalFlag = is64_ ? 0x8000000000000000ULL : 0x80000000ULL;
        // Termination is checked inside file-backed section bounds as well as
        // this independent work limit, even for a bound IAT with no name table.
        for (std::size_t i = 0; i <= kMaxImports; ++i) {
            const auto* at = Map(AddRva(lookup, i * width), width).bytes;
            (void)Map(AddRva(iat, i * width), width);
            const auto value = is64_ ? U64(at) : U32(at);
            if (!value) return;
            if (importCount_ == kMaxImports) Fail("Import count exceeds the 8192-entry inspection limit.");
            ++importCount_;
            if (boundIat) continue;
            if (value & ordinalFlag) {
                if (value & ~(ordinalFlag | 0xffffULL)) Fail("Ordinal import contains reserved high bits.");
                Line("  ordinal #" + std::to_string(value & 0xffffU));
            } else {
                const auto name = Name(AddressRva(value, legacyVa), 2);
                Line("  " + name);
                const auto lower = Lower(name);
                dynamicLoad_ |= lower == "getprocaddress" || lower == "loadlibrarya" ||
                                lower == "loadlibraryw" || lower == "loadlibraryexa" || lower == "loadlibraryexw";
            }
        }
        Fail("Import lookup table has no bounded terminator.");
    }

    void Imports(bool delay) {
        const auto directory = directories_[delay ? 13 : 1];
        Line(delay ? "Delay imports:" : "Normal imports:");
        if (!directory.address) { Line("  none declared"); return; }
        const std::size_t width = delay ? 32 : 20;
        if (directory.size < width) Fail("Import directory is too small for a descriptor.");
        const Mapping mapped = Map(directory.address, directory.size);
        const std::size_t descriptors = directory.size / width;
        for (std::size_t i = 0; i < descriptors && i <= kMaxLibraries; ++i) {
            const auto* at = mapped.bytes + i * width;
            bool zero = true;
            for (std::size_t j = 0; j < width; ++j) zero &= at[j] == 0;
            if (zero) return;
            if (libraryCount_ == kMaxLibraries) Fail("Library count exceeds the 256-descriptor inspection limit.");
            ++libraryCount_;
            std::uint32_t nameRva, lookup, iat;
            bool legacyVa = false, boundIat = false;
            if (delay) {
                const auto attributes = U32(at);
                if (attributes & ~1U) Fail("Delay-import descriptor has reserved attribute bits.");
                legacyVa = !(attributes & 1U);
                nameRva = AddressRva(U32(at + 4), legacyVa);
                iat = AddressRva(U32(at + 12), legacyVa);
                lookup = AddressRva(U32(at + 16), legacyVa);
                // Ancillary delay tables are not followed, but their declared
                // pointer locations must remain in the image address space.
                for (const auto offset : {8U, 20U, 24U}) {
                    if (const auto address = U32(at + offset)) {
                        const auto rva = AddressRva(address, legacyVa);
                        if (rva >= imageSize_) Fail("Delay-import ancillary table exceeds SizeOfImage.");
                    }
                }
            } else {
                const auto original = U32(at);
                nameRva = U32(at + 12);
                iat = U32(at + 16);
                lookup = original ? original : iat;
                boundIat = !original && U32(at + 4) != 0;
            }
            if (!nameRva) Fail("Import descriptor has no DLL name.");
            const auto name = Name(nameRva);
            DetectLibrary(name);
            Line("  DLL " + name + (delay ? " (delay-loaded)" : ""));
            if (boundIat) Line("  bound IAT without a lookup table: symbol names are unavailable");
            Thunks(lookup, iat, legacyVa, boundIat);
        }
        Fail("Import descriptor table has no bounded null terminator.");
    }

    std::string Graphics() const {
        std::string result;
        const auto add = [&result](const char* name) { if (!result.empty()) result += ", "; result += name; };
        if (d3d8_) add("Direct3D 8");
        if (d3d9_) add("Direct3D 9");
        if (d3d10_) add("Direct3D 10");
        if (d3d11_) add("Direct3D 11");
        if (d3d12_) add("Direct3D 12");
        if (dxgi_) add("DXGI");
        if (openGl_) add("OpenGL");
        if (directDraw_) add("DirectDraw");
        return result.empty() ? "none found; dynamically loaded graphics remain unknown" : result;
    }

    const std::uint8_t* bytes_;
    std::size_t size_;
    std::vector<Section> sections_;
    std::array<Directory, 16> directories_{};
    std::string architecture_, report_;
    std::uint32_t imageSize_ = 0, headerSize_ = 0, entry_ = 0;
    std::uint64_t imageBase_ = 0;
    std::uint16_t machine_ = 0, subsystem_ = 0;
    std::size_t libraryCount_ = 0, importCount_ = 0;
    bool is64_ = false, isDll_ = false, reportTruncated_ = false, dynamicLoad_ = false;
    bool d3d8_ = false, d3d9_ = false, d3d10_ = false, d3d11_ = false, d3d12_ = false;
    bool dxgi_ = false, openGl_ = false, directDraw_ = false;
};

GamePreflightResult Rejected(const char* message) {
    GamePreflightResult result;
    result.summary = std::string("PE inspection failed: ") + message;
    result.report = result.summary + "\nNo guest code was executed and no DLL was loaded.\n";
    return result;
}

} // namespace

GamePreflightResult InspectGamePe(const std::uint8_t* bytes, std::size_t size) noexcept {
    try {
        return Inspector(bytes, size).Run();
    } catch (const ParseError& error) {
        try { return Rejected(error.message); } catch (...) {}
    } catch (...) {
        try { return Rejected("Unable to allocate bounded inspection state."); } catch (...) {}
    }
    return {};
}

} // namespace xcdm::runtime
