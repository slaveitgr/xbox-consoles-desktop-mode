#pragma once

#include "RuntimeProbe.h"

namespace xcdm::runtime {

// Measures exact, simultaneous PE32 guest-data reservations. No code executes.
ProbeResult RunLowAddressProbe() noexcept;

} // namespace xcdm::runtime
