// SPDX-License-Identifier: GPL-2.0-only
#include "CoreDiagnostics.h"
#include <charconv>
#include <exception>
#include <limits>
#include <utility>
#if defined(_WIN32) && !defined(PROTON_CORE_DIAGNOSTIC_PARSER_ONLY)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cstdio>
#include <mutex>
extern int runX64SelfTest();
extern "C" int runX64RunElf(const char* path);
#endif

namespace protondesktop
{
    namespace
    {
        constexpr std::string_view SummaryTag = "self-test summary:";
        constexpr std::string_view Marker = "ProtonDesktop guest-x64 interpreter: PASS";
        bool Begins(std::string_view value, std::string_view prefix) noexcept
        { return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix; }
        bool Take(std::string_view& value, std::string_view prefix) noexcept
        {
            if (!Begins(value, prefix)) return false;
            value.remove_prefix(prefix.size());
            return true;
        }
        std::string_view TrimRight(std::string_view value) noexcept
        {
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
            return value;
        }
        template<class Integer> bool Number(std::string_view& value, Integer& result) noexcept
        {
            if (value.empty()) return false;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr == value.data()) return false;
            value.remove_prefix(static_cast<std::size_t>(parsed.ptr - value.data()));
            return true;
        }
        std::size_t Occurrences(std::string_view value, std::string_view needle) noexcept
        {
            std::size_t count = 0, offset = 0;
            while ((offset = value.find(needle, offset)) != std::string_view::npos)
            { ++count; offset += needle.size(); }
            return count;
        }
        template<class Visitor> void Lines(std::string_view log, Visitor visit)
        {
            while (!log.empty())
            {
                const auto newline = log.find('\n');
                auto line = log.substr(0, newline);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                visit(line);
                if (newline == std::string_view::npos) break;
                log.remove_prefix(newline + 1);
            }
        }
        bool ValidLog(std::string_view log) noexcept
        { return !log.empty() && log.size() <= CoreDiagnosticLogLimit && log.find('\0') == std::string_view::npos; }
    }

    SelfTestLogResult ParseCoreSelfTestLog(std::string_view log, int coreReturnCode)
    {
        SelfTestLogResult result;
        if (!ValidLog(log)) { result.error = "Self-test log is empty, oversized or contains NUL bytes"; return result; }
        result.summaryOccurrences = Occurrences(log, SummaryTag);
        bool parsedSummary = false;
        Lines(log, [&](std::string_view line) {
            if (!Take(line, "=== self-test summary: ")) return;
            std::uint64_t passed = 0, failed = 0;
            if (Number(line, passed) && Take(line, " passed, ") && Number(line, failed) &&
                Take(line, " failed ===") && TrimRight(line).empty())
            {
                parsedSummary = true;
                result.summaryParsed = true;
                result.passed = passed;
                result.failed = failed;
            }
        });
        if (coreReturnCode != 0) result.error = "Self-test core call returned a nonzero status";
        else if (result.summaryOccurrences != 1 || !parsedSummary)
            result.error = "Self-test summary is missing, malformed or ambiguous";
        else if (!result.passed || result.failed) result.error = "Self-test summary contains no passes or reports failures";
        else result.accepted = true;
        return result;
    }

    GuestDiagnosticLogResult ParseCoreGuestLog(std::string_view log, GuestDiagnosticControl control,
                                               int coreReturnCode)
    {
        GuestDiagnosticLogResult result;
        if (control != GuestDiagnosticControl::Positive && control != GuestDiagnosticControl::Negative)
        { result.error = "Unknown guest diagnostic control"; return result; }
        if (!ValidLog(log)) { result.error = "Guest log is empty, oversized or contains NUL bytes"; return result; }
        result.markerOccurrences = Occurrences(log, Marker);
        bool malformed = false;
        Lines(log, [&](std::string_view line) {
            line = TrimRight(line);
            auto remainder = line;
            if (Take(remainder, "CPU64: exit syscall, status=") ||
                (remainder = line, Take(remainder, "CPU64: exit_group syscall, status=")))
            {
                ++result.exitOccurrences;
                const auto statusText = remainder;
                if (!Number(remainder, result.guestExit) || !remainder.empty() ||
                    statusText != std::to_string(result.guestExit)) malformed = true;
                else result.exitParsed = true;
            }
            else if (Begins(line, "CPU64: exit")) malformed = true;
            remainder = line;
            if (Take(remainder, "--x64-run-elf: stopped after "))
            {
                ++result.stopOccurrences;
                if (!Number(remainder, result.instructions) ||
                    !Take(remainder, " instructions (yield=1 ") ||
                    remainder.size() < 2 || remainder.back() != ')') malformed = true;
                else result.stopParsed = true;
            }
            else if (Begins(line, "--x64-run-elf: stopped")) malformed = true;
            if (line == "--x64-run-elf: exit reached cleanly") ++result.cleanOccurrences;
            else if (Begins(line, "--x64-run-elf: exit reached")) malformed = true;
            // A contradictory explicit failure must not be hidden by otherwise
            // valid markers appended to a corrupted or concatenated log.
            if (Begins(line, "--x64-run-elf: instruction cap hit") ||
                Begins(line, "--x64-run-elf: stopped without exit")) malformed = true;
        });
        const auto expectedExit = control == GuestDiagnosticControl::Positive ? 0 : 9;
        const auto expectedMarkers = control == GuestDiagnosticControl::Positive ? 1u : 0u;
        if (coreReturnCode != 0) result.error = "Guest core call returned a nonzero status";
        else if (malformed || result.exitOccurrences != 1 || result.stopOccurrences != 1 || result.cleanOccurrences != 1)
            result.error = "Guest exit, bounded stop or clean-exit evidence is missing, malformed or ambiguous";
        else if (result.guestExit != expectedExit) result.error = "Guest syscall status differs from its control";
        else if (!result.instructions || result.instructions >= 10000)
            result.error = "Guest did not stop within the smoke-control instruction bound";
        else if (result.markerOccurrences != expectedMarkers) result.error = "Guest success marker differs from its control";
        else result.accepted = true;
        return result;
    }

    bool IsCoreDiagnosticAbsolutePath(std::wstring_view path) noexcept
    {
        if (path.size() < 3 || path.size() > 32760 || path[1] != L':' ||
            !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
            (path[2] != L'\\' && path[2] != L'/')) return false;
        std::size_t component = 3;
        for (std::size_t i = 3; i <= path.size(); ++i)
        {
            if (i == path.size() || path[i] == L'\\' || path[i] == L'/')
            {
                const auto name = path.substr(component, i - component);
                if (name == L"." || name == L"..") return false;
                component = i + 1;
            }
            else if (path[i] < 32 || path[i] == L':' || path[i] == L'"' ||
                     path[i] == L'<' || path[i] == L'>' || path[i] == L'|' ||
                     path[i] == L'*' || path[i] == L'?') return false;
        }
        return true;
    }

    namespace
    {
        void JsonHex(std::string& output, unsigned value)
        {
            constexpr char digits[] = "0123456789abcdef";
            output += "\\u";
            for (int shift = 12; shift >= 0; shift -= 4) output += digits[(value >> shift) & 15];
        }
        std::string JsonString(std::string_view value)
        {
            std::string output = "\"";
            for (const unsigned char byte : value)
            {
                if (byte == '"' || byte == '\\') { output += '\\'; output += static_cast<char>(byte); }
                else if (byte < 32 || byte >= 127) JsonHex(output, byte);
                else output += static_cast<char>(byte);
            }
            output += '"';
            return output;
        }
        std::string JsonPath(std::wstring_view value)
        {
            std::string output = "\"";
            for (const wchar_t unit : value)
            {
                const auto scalar = static_cast<std::uint32_t>(unit);
                if (scalar == '"' || scalar == '\\') { output += '\\'; output += static_cast<char>(scalar); }
                else if (scalar >= 32 && scalar < 127) output += static_cast<char>(scalar);
                else if (scalar <= 0xFFFF) JsonHex(output, scalar);
                else if (scalar <= 0x10FFFF)
                {
                    JsonHex(output, 0xD800 + ((scalar - 0x10000) >> 10));
                    JsonHex(output, 0xDC00 + ((scalar - 0x10000) & 1023));
                }
                else JsonHex(output, 0xFFFD);
            }
            output += '"';
            return output;
        }
        const char* JsonBool(bool value) noexcept { return value ? "true" : "false"; }
        std::string PhaseJson(const CoreDiagnosticPhaseReport& phase)
        {
            return std::string("{\"attempted\":") + JsonBool(phase.attempted) +
                ",\"core_returned\":" + JsonBool(phase.coreReturned) +
                ",\"core_return_code\":" + (phase.coreReturned ? std::to_string(phase.coreReturnCode) : "null") +
                ",\"log_complete\":" + JsonBool(phase.logComplete) +
                ",\"log_path\":" + JsonPath(phase.logPath) + ",\"error\":" + JsonString(phase.error) + "}";
        }
        std::string GuestJson(const GuestDiagnosticLogResult& guest)
        {
            return std::string("{\"accepted\":") + JsonBool(guest.accepted) +
                ",\"guest_exit\":" + (guest.exitParsed && guest.exitOccurrences == 1 ? std::to_string(guest.guestExit) : "null") +
                ",\"instructions\":" + (guest.stopParsed && guest.stopOccurrences == 1 ? std::to_string(guest.instructions) : "null") +
                ",\"exit_occurrences\":" + std::to_string(guest.exitOccurrences) +
                ",\"stop_occurrences\":" + std::to_string(guest.stopOccurrences) +
                ",\"clean_occurrences\":" + std::to_string(guest.cleanOccurrences) +
                ",\"marker_occurrences\":" + std::to_string(guest.markerOccurrences) +
                ",\"error\":" + JsonString(guest.error) + "}";
        }
    }
    std::string CoreDiagnosticReportJson(const CoreDiagnosticReport& report, bool running)
    {
        const auto state = running ? "running" :
            report.status == CoreDiagnosticStatus::Passed ? "passed" :
            report.status == CoreDiagnosticStatus::Failed ? "failed" : "incomplete";
        const bool knownSummary = report.selfTest.summaryParsed && report.selfTest.summaryOccurrences == 1;
        return std::string("{\n\"status\":\"") + state +
            "\",\n\"source_commit\":\"58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4\"," +
            "\n\"scope\":\"in-process APP core diagnostics, not Wine or desktop qualification\"," +
            "\n\"software_interpreter\":true,\"wine_executed\":false,\"rootfs_executed\":false," +
            "\"native_backend_available\":false,\"xbox_qualification\":\"not_inferred\"," +
            "\n\"error\":" + JsonString(report.error) +
            ",\n\"selftest_phase\":" + PhaseJson(report.selfTestPhase) +
            ",\n\"positive_phase\":" + PhaseJson(report.positivePhase) +
            ",\n\"negative_phase\":" + PhaseJson(report.negativePhase) +
            ",\n\"selftest\":{\"accepted\":" + JsonBool(report.selfTest.accepted) +
            ",\"summary_occurrences\":" + std::to_string(report.selfTest.summaryOccurrences) +
            ",\"passed_checks\":" + (knownSummary ? std::to_string(report.selfTest.passed) : "null") +
            ",\"failed_checks\":" + (knownSummary ? std::to_string(report.selfTest.failed) : "null") +
            ",\"error\":" + JsonString(report.selfTest.error) + "}," +
            "\n\"positive\":" + GuestJson(report.positive) +
            ",\n\"negative\":" + GuestJson(report.negative) + "\n}\n";
    }

#if defined(_WIN32) && !defined(PROTON_CORE_DIAGNOSTIC_PARSER_ONLY)
    namespace
    {
        std::mutex diagnosticMutex;
        bool diagnosticAttempted = false;

        std::wstring LogPath(const std::wstring& directory, const wchar_t* name)
        {
            auto path = directory;
            if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
            path += name;
            return path;
        }
        bool NativeLoaderPath(const std::wstring& path, std::string& result)
        {
            // fopen(const char*) uses the native ANSI codepage, not UTF-8.
            // UTF-8 system ACP needs different flags and no default-char args.
            const auto codepage = GetACP();
            const bool utf8 = codepage == CP_UTF8;
            BOOL substituted = FALSE;
            const auto flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
            const auto bytes = WideCharToMultiByte(codepage, flags, path.c_str(),
                static_cast<int>(path.size()), nullptr, 0, nullptr, utf8 ? nullptr : &substituted);
            if (bytes <= 0 || substituted) return false;
            result.resize(static_cast<std::size_t>(bytes));
            substituted = FALSE;
            const auto converted = WideCharToMultiByte(codepage, flags, path.c_str(),
                static_cast<int>(path.size()), result.data(), bytes, nullptr, utf8 ? nullptr : &substituted);
            return converted == bytes && !substituted;
        }
        bool ReadCapturedStdout(std::string& text, std::string& error)
        {
            if (std::fflush(stdout) != 0 || std::ferror(stdout))
            { error = "Flushing captured stdout failed"; return false; }
            if (std::fseek(stdout, 0, SEEK_END) != 0)
            { error = "Seeking captured stdout failed"; return false; }
            const auto length = std::ftell(stdout);
            if (length < 0 || static_cast<unsigned long>(length) > CoreDiagnosticLogLimit)
            { error = "Captured stdout exceeds the bounded log size or has an invalid length"; return false; }
            if (std::fseek(stdout, 0, SEEK_SET) != 0)
            { error = "Rewinding captured stdout failed"; return false; }
            text.resize(static_cast<std::size_t>(length));
            const auto read = std::fread(text.data(), 1, text.size(), stdout);
            const bool complete = read == text.size() && !std::ferror(stdout);
            // An update stream needs an intervening seek before further writes.
            const bool repositioned = std::fseek(stdout, 0, SEEK_END) == 0;
            if (!complete || !repositioned)
            { error = "Reading captured stdout failed"; return false; }
            return true;
        }
        template<class CoreCall> bool CapturePhase(CoreDiagnosticPhaseReport& phase,
            const std::wstring& path, std::string& log, CoreCall coreCall)
        {
            phase.logPath = path;
            phase.attempted = true;
            FILE* capture = nullptr;
            const auto opened = _wfreopen_s(&capture, path.c_str(), L"w+b", stdout);
            if (opened != 0 || capture != stdout)
            {
                // freopen may have closed the previous stream on failure. Do
                // not retry, fclose stdout, or write diagnostics to it now.
                phase.error = "Opening app-local stdout capture failed (CRT status " +
                    std::to_string(opened) + ")";
                return false;
            }
            try
            {
                phase.coreReturnCode = coreCall();
                phase.coreReturned = true;
            }
            catch (const std::exception& error) { phase.error = std::string("Core exception: ") + error.what(); }
            catch (...) { phase.error = "Core raised a nonstandard C++ exception"; }
            std::string readError;
            phase.logComplete = ReadCapturedStdout(log, readError);
            if (!phase.logComplete)
            {
                if (!phase.error.empty()) phase.error += "; ";
                phase.error += readError;
            }
            return phase.coreReturned && phase.logComplete;
        }
    }

    CoreDiagnosticReport RunCoreDiagnostics(const CoreDiagnosticPaths& paths)
    {
        CoreDiagnosticReport report;
        std::unique_lock<std::mutex> runLock(diagnosticMutex, std::try_to_lock);
        if (!runLock.owns_lock()) { report.error = "A core diagnostic sequence is already running"; return report; }
        if (diagnosticAttempted) { report.error = "Core diagnostics were already attempted in this process"; return report; }
        try
        {
            if (!IsCoreDiagnosticAbsolutePath(paths.localStateDirectory) ||
                !IsCoreDiagnosticAbsolutePath(paths.positiveElfPath) ||
                !IsCoreDiagnosticAbsolutePath(paths.negativeElfPath) ||
                paths.positiveElfPath.back() == L'\\' || paths.positiveElfPath.back() == L'/' ||
                paths.negativeElfPath.back() == L'\\' || paths.negativeElfPath.back() == L'/' ||
                CompareStringOrdinal(paths.positiveElfPath.c_str(), -1,
                    paths.negativeElfPath.c_str(), -1, TRUE) == CSTR_EQUAL)
            { report.error = "Diagnostics require distinct drive-absolute packaged ELF paths and an app-local directory"; return report; }
            const auto selfLog = LogPath(paths.localStateDirectory, L"protondesktop-cpu64-selftest.log");
            const auto positiveLog = LogPath(paths.localStateDirectory, L"protondesktop-guest64-positive.log");
            const auto negativeLog = LogPath(paths.localStateDirectory, L"protondesktop-guest64-negative.log");
            if (!IsCoreDiagnosticAbsolutePath(selfLog) || !IsCoreDiagnosticAbsolutePath(positiveLog) ||
                !IsCoreDiagnosticAbsolutePath(negativeLog))
            { report.error = "App-local diagnostic log path is too long or invalid"; return report; }
            std::string positivePath, negativePath;
            if (!NativeLoaderPath(paths.positiveElfPath, positivePath) ||
                !NativeLoaderPath(paths.negativeElfPath, negativePath))
            { report.error = "The core fopen loader cannot represent a packaged ELF path losslessly in the native codepage"; return report; }

            diagnosticAttempted = true;
            report.status = CoreDiagnosticStatus::Failed;
            std::string log;
            if (!CapturePhase(report.selfTestPhase, selfLog, log, [] { return runX64SelfTest(); }))
            { report.error = report.selfTestPhase.error; return report; }
            report.selfTest = ParseCoreSelfTestLog(log, report.selfTestPhase.coreReturnCode);
            if (!report.selfTest.accepted) { report.error = report.selfTest.error; return report; }
            log.clear();
            if (!CapturePhase(report.positivePhase, positiveLog, log,
                              [&] { return runX64RunElf(positivePath.c_str()); }))
            { report.error = report.positivePhase.error; return report; }
            report.positive = ParseCoreGuestLog(log, GuestDiagnosticControl::Positive, report.positivePhase.coreReturnCode);
            if (!report.positive.accepted) { report.error = report.positive.error; return report; }
            log.clear();
            if (!CapturePhase(report.negativePhase, negativeLog, log,
                              [&] { return runX64RunElf(negativePath.c_str()); }))
            { report.error = report.negativePhase.error; return report; }
            report.negative = ParseCoreGuestLog(log, GuestDiagnosticControl::Negative, report.negativePhase.coreReturnCode);
            if (!report.negative.accepted) { report.error = report.negative.error; return report; }
            report.status = CoreDiagnosticStatus::Passed;
        }
        catch (const std::exception& error)
        { report.status = CoreDiagnosticStatus::Failed; report.error = std::string("Diagnostic exception: ") + error.what(); }
        catch (...)
        { report.status = CoreDiagnosticStatus::Failed; report.error = "Diagnostics raised a nonstandard C++ exception"; }
        return report;
    }

    bool WriteCoreDiagnosticReport(const std::wstring& absolutePath,
        const CoreDiagnosticReport& report, bool running, std::string& error)
    {
        if (!IsCoreDiagnosticAbsolutePath(absolutePath))
        { error = "Report path is not a drive-absolute app-local path"; return false; }
        const auto json = CoreDiagnosticReportJson(report, running);
        FILE* file = nullptr;
        const auto opened = _wfopen_s(&file, absolutePath.c_str(), L"wb");
        if (opened || !file)
        { error = "Opening diagnostic JSON failed (CRT status " + std::to_string(opened) + ")"; return false; }
        const bool written = std::fwrite(json.data(), 1, json.size(), file) == json.size();
        const bool flushed = std::fflush(file) == 0;
        const bool closed = std::fclose(file) == 0;
        if (!written || !flushed || !closed)
        { error = "Writing or flushing diagnostic JSON failed"; return false; }
        error.clear();
        return true;
    }
#endif
}
