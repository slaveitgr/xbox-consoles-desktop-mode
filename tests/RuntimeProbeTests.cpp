#include "runtime/RuntimeProbe.h"

#include <iostream>

int main() {
    const auto executable = xcdm::runtime::RunExecutableMemoryProbe();
    const auto pe = xcdm::runtime::RunMinimalPeProbe();
    std::cout << "Executable memory: " << executable.message << " (" << executable.errorCode << ")\n"
              << "Minimal PE: " << pe.message << " (" << pe.errorCode << ")\n";
#if !defined(_WIN32) || !(defined(_M_X64) || defined(__x86_64__))
    // A Linux parser pass must never be reported as successful native execution.
    return executable.status == xcdm::runtime::ProbeStatus::Unsupported &&
           pe.status == xcdm::runtime::ProbeStatus::Unsupported &&
           executable.errorCode == 0 && pe.errorCode == 0 &&
           !executable.message.empty() && !pe.message.empty() ? 0 : 1;
#elif defined(_MSC_VER)
    // CMake builds an ordinary Windows host process, not an AppContainer.
    // Both diagnostics must really execute and return 42 on that host; a
    // diagnosed failure is still a failing regression test. The packaged
    // UWP app records console-specific denials separately.
    return executable.status == xcdm::runtime::ProbeStatus::Passed &&
           pe.status == xcdm::runtime::ProbeStatus::Passed &&
           executable.errorCode == 0 && pe.errorCode == 0 &&
           !executable.message.empty() && !pe.message.empty() ? 0 : 1;
#else
    return executable.status == xcdm::runtime::ProbeStatus::Unsupported &&
           pe.status == xcdm::runtime::ProbeStatus::Unsupported ? 0 : 1;
#endif
}
