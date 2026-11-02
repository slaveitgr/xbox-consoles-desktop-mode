#include "runtime/LowAddressProbe.h"
#include <iostream>

int main() {
    using namespace xcdm::runtime;
    const auto first = RunLowAddressProbe();
    std::cout << first.message << " (" << first.errorCode << ")\n";
    if (first.message.empty()) return 1;
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    if (first.status == ProbeStatus::Passed) {
        // An immediate second exact reservation checks that success released
        // all three simultaneously owned regions instead of leaking them.
        const auto second = RunLowAddressProbe();
        return first.errorCode == 0 && second.status == ProbeStatus::Passed &&
            second.errorCode == 0 && !second.message.empty() ? 0 : 1;
    }
    // Address availability is a capability result on this process, not a
    // parser/CPU regression. A refusal must identify the failed operation.
    return first.status == ProbeStatus::Failed && first.message.find("failed") != std::string::npos ? 0 : 1;
#else
    return first.status == ProbeStatus::Unsupported && first.errorCode == 0 ? 0 : 1;
#endif
}
