// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace protondesktop
{
    constexpr std::size_t CoreDiagnosticLogLimit = 1024 * 1024;
    enum class GuestDiagnosticControl { Positive, Negative };
    enum class CoreDiagnosticStatus { Incomplete, Failed, Passed };

    struct SelfTestLogResult
    {
        bool accepted = false;
        bool summaryParsed = false;
        std::size_t summaryOccurrences = 0;
        std::uint64_t passed = 0, failed = 0;
        std::string error;
    };
    struct GuestDiagnosticLogResult
    {
        bool accepted = false;
        bool exitParsed = false, stopParsed = false;
        std::size_t exitOccurrences = 0, stopOccurrences = 0;
        std::size_t cleanOccurrences = 0, markerOccurrences = 0;
        std::int64_t guestExit = 0;
        std::uint64_t instructions = 0;
        std::string error;
    };

    // Portable validators for the actual pinned CPU64 output, including CRLF.
    // A zero host return alone never qualifies a guest result.
    SelfTestLogResult ParseCoreSelfTestLog(std::string_view log, int coreReturnCode);
    GuestDiagnosticLogResult ParseCoreGuestLog(std::string_view log,
        GuestDiagnosticControl control, int coreReturnCode);
    // Lexical drive-absolute native path check. This does not prove ownership,
    // package membership, file provenance, existence or app access permissions.
    bool IsCoreDiagnosticAbsolutePath(std::wstring_view path) noexcept;

    struct CoreDiagnosticPaths
    {
        // Resolve these using ApplicationData::LocalFolder and Package location
        // on the host. The ELF inputs must be the two packaged, self-authored
        // smoke controls, never arbitrary user programs or Wine/rootfs files.
        std::wstring localStateDirectory, positiveElfPath, negativeElfPath;
    };
    struct CoreDiagnosticPhaseReport
    {
        bool attempted = false, coreReturned = false, logComplete = false;
        int coreReturnCode = 0; // meaningful only when coreReturned is true
        std::wstring logPath;
        std::string error;
    };
    struct CoreDiagnosticReport
    {
        CoreDiagnosticStatus status = CoreDiagnosticStatus::Incomplete;
        CoreDiagnosticPhaseReport selfTestPhase, positivePhase, negativePhase;
        SelfTestLogResult selfTest;
        GuestDiagnosticLogResult positive, negative;
        std::string error;
    };

    // ASCII JSON with escaped paths/errors. This serializes evidence supplied
    // by a real run; calling it or compiling the app does not run the core.
    std::string CoreDiagnosticReportJson(const CoreDiagnosticReport& report, bool running = false);

#ifdef _WIN32
    // Synchronous: call only on a host-owned worker; caller owns its join and
    // publishes the returned value through its own synchronized state. There
    // are no threads, raw App callbacks, cancellation or process launches here.
    // At most one sequence is attempted per process, with a nonblocking mutex
    // guard against reentry. Other users of process-global stdout/core state
    // must be idle. stdout is reopened for each phase and deliberately left
    // associated with the final capture; it is never closed/restored to the
    // invalid GUI console. Core crashes/hangs are not converted to exceptions.
    // The existing char* ELF loader requires a lossless native-codepage path;
    // unrepresentable UTF-16 input fails explicitly before guest execution.
    // Success qualifies these diagnostics only, never Wine or the backend.
    CoreDiagnosticReport RunCoreDiagnostics(const CoreDiagnosticPaths& paths);
    // Synchronous worker-only stream I/O. A failed write is reported separately;
    // it does not convert a parsed diagnostic failure into success.
    bool WriteCoreDiagnosticReport(const std::wstring& absolutePath,
        const CoreDiagnosticReport& report, bool running, std::string& error);
#endif
}
