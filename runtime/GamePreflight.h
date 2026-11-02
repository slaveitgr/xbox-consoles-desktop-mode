#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace xcdm::runtime {

struct GamePreflightResult {
    // True means the bounded metadata inspection succeeded. It does not mean
    // the image is executable, safe, fully validated or compatible with Xbox.
    bool ok = false;
    std::string summary;
    std::string report;
};

// Inspects an on-disk PE32/PE32+ file of at most 64 MiB, without loading DLLs,
// mapping executable memory or calling any entry point. Reports direct and
// delay imports; dynamically loaded dependencies cannot be inferred this way.
GamePreflightResult InspectGamePe(const std::uint8_t* bytes, std::size_t size) noexcept;

} // namespace xcdm::runtime
