#pragma once

#include "Win32Probe32.h"

namespace xcdm::runtime {

// Loads the packaged Win32Bridge DLL and calls its versioned C exports.
// A missing/invalid component is reported as failure; no direct-engine fallback.
Win32Probe32Result RunBridgeWin32Probe32() noexcept;
Win32Probe32Result TraceBridgeStartup32(const std::uint8_t* bytes, std::size_t size,
    std::uint64_t instructionLimit = 50000) noexcept;

} // namespace xcdm::runtime
