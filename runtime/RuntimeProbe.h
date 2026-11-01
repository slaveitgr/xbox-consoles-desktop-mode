#pragma once

#include <cstdint>
#include <string>

namespace xcdm::runtime {

enum class ProbeStatus { NotRun, Passed, Failed, Unsupported };

struct ProbeResult {
    ProbeStatus status;
    std::string message;
    std::uint32_t errorCode;
};

ProbeResult RunExecutableMemoryProbe() noexcept;
ProbeResult RunMinimalPeProbe() noexcept;

} // namespace xcdm::runtime
