// SPDX-License-Identifier: GPL-2.0-only
#include "CoreDiagnostics.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

using namespace protondesktop;
namespace
{
    unsigned checks = 0;
    void Require(bool condition, const char* message)
    {
        ++checks;
        if (!condition) throw std::runtime_error(message);
    }
    const std::string selfTest = "=== CPU64 self-test ===\n=== self-test summary: 907 passed, 0 failed ===\n";
    const std::string positive =
        "[guest fd=1 pid=0 ?] ProtonDesktop guest-x64 interpreter: PASS\n"
        "CPU64: exit syscall, status=0\n"
        "--x64-run-elf: stopped after 20 instructions (yield=1 RIP=0x40104b RAX=0x0)\n"
        "--x64-run-elf: exit reached cleanly\n";
    const std::string negative =
        "CPU64: exit syscall, status=9\n"
        "--x64-run-elf: stopped after 7 instructions (yield=1 RIP=0x401057 RAX=0x0)\n"
        "--x64-run-elf: exit reached cleanly\n";
    std::string Replaced(std::string value, const std::string& from, const std::string& to)
    {
        const auto location = value.find(from);
        if (location == std::string::npos) throw std::runtime_error("bad test replacement");
        value.replace(location, from.size(), to);
        return value;
    }
    void ParserChecks()
    {
        const auto self = ParseCoreSelfTestLog(selfTest, 0);
        Require(self.accepted && self.passed == 907 && self.failed == 0 && self.summaryOccurrences == 1,
                "actual selftest summary counts");
        Require(!ParseCoreSelfTestLog(selfTest, 1).accepted, "nonzero selftest core return fails");
        Require(!ParseCoreSelfTestLog(selfTest + selfTest, 0).accepted, "duplicated selftest summaries rejected");
        Require(!ParseCoreSelfTestLog("self-test summary: 907 passed, 0 failed", 0).accepted,
                "malformed selftest line rejected");
        Require(!ParseCoreSelfTestLog(Replaced(selfTest, "907 passed", "0 passed"), 0).accepted, "zero passes fail");
        Require(!ParseCoreSelfTestLog(Replaced(selfTest, "0 failed", "1 failed"), 0).accepted, "reported failure rejected");
        Require(!ParseCoreSelfTestLog(Replaced(selfTest, "907", "18446744073709551616"), 0).accepted,
                "overflowing selftest count rejected");
        Require(!ParseCoreSelfTestLog("", 0).accepted, "missing selftest log rejected");
        Require(!ParseCoreSelfTestLog(selfTest + '\0', 0).accepted, "embedded NUL selftest rejected");
        Require(!ParseCoreSelfTestLog(std::string(CoreDiagnosticLogLimit + 1, 'x'), 0).accepted,
                "oversized log rejected");
        const auto good = ParseCoreGuestLog(positive, GuestDiagnosticControl::Positive, 0);
        Require(good.accepted && good.guestExit == 0 && good.instructions == 20 &&
                good.exitOccurrences == 1 && good.markerOccurrences == 1, "positive actual guest evidence");
        const auto badControl = ParseCoreGuestLog(negative, GuestDiagnosticControl::Negative, 0);
        Require(badControl.accepted && badControl.guestExit == 9 && badControl.instructions == 7,
                "negative guest exit is nine despite zero core return");
        Require(!ParseCoreGuestLog(negative, GuestDiagnosticControl::Positive, 0).accepted,
                "zero host return does not turn guest exit nine into success");
        Require(!ParseCoreGuestLog(positive, GuestDiagnosticControl::Negative, 0).accepted,
                "positive payload cannot pass negative control");
        Require(!ParseCoreGuestLog(positive, GuestDiagnosticControl::Positive, 5).accepted, "nonzero core return fails");
        Require(!ParseCoreGuestLog(positive + positive, GuestDiagnosticControl::Positive, 0).accepted,
                "concatenated guest logs rejected");
        Require(!ParseCoreGuestLog(Replaced(positive, "status=0", "status=9"), GuestDiagnosticControl::Positive, 0).accepted,
                "wrong syscall status rejected");
        Require(!ParseCoreGuestLog(Replaced(positive, "status=0", "status=00"), GuestDiagnosticControl::Positive, 0).accepted,
                "noncanonical exit spelling rejected like the reference parser");
        Require(!ParseCoreGuestLog(Replaced(positive, "status=0", "status=-0"), GuestDiagnosticControl::Positive, 0).accepted,
                "negative zero does not replace exact guest exit zero");
        Require(!ParseCoreGuestLog(Replaced(positive, "yield=1", "yield=0"), GuestDiagnosticControl::Positive, 0).accepted,
                "non-yielded stop rejected");
        Require(!ParseCoreGuestLog(Replaced(positive, "20 instructions", "0 instructions"), GuestDiagnosticControl::Positive, 0).accepted,
                "zero instructions rejected");
        Require(!ParseCoreGuestLog(Replaced(positive, "20 instructions", "10000 instructions"), GuestDiagnosticControl::Positive, 0).accepted,
                "instruction boundary is strict");
        Require(ParseCoreGuestLog(Replaced(positive, "20 instructions", "9999 instructions"), GuestDiagnosticControl::Positive, 0).accepted,
                "last permitted instruction count accepted");
        Require(!ParseCoreGuestLog(Replaced(positive, "20 instructions", "18446744073709551616 instructions"),
                                  GuestDiagnosticControl::Positive, 0).accepted, "instruction count overflow rejected");
        Require(!ParseCoreGuestLog(Replaced(positive, "--x64-run-elf: exit reached cleanly\n", ""),
                                  GuestDiagnosticControl::Positive, 0).accepted, "missing clean marker rejected");
        Require(!ParseCoreGuestLog(Replaced(positive, "ProtonDesktop guest-x64 interpreter: PASS", ""),
                                  GuestDiagnosticControl::Positive, 0).accepted, "missing positive marker rejected");
        Require(!ParseCoreGuestLog(negative + "ProtonDesktop guest-x64 interpreter: PASS\n",
                                  GuestDiagnosticControl::Negative, 0).accepted, "negative marker must be absent");
        Require(!ParseCoreGuestLog(positive + "CPU64: exit syscall, status=0\n",
                                  GuestDiagnosticControl::Positive, 0).accepted, "duplicate syscall evidence rejected");
        Require(!ParseCoreGuestLog(positive + "--x64-run-elf: instruction cap hit\n",
                                  GuestDiagnosticControl::Positive, 0).accepted, "contradictory cap evidence rejected");
        Require(ParseCoreGuestLog(Replaced(positive, "exit syscall", "exit_group syscall"),
                                 GuestDiagnosticControl::Positive, 0).accepted, "real exit_group spelling supported");
        Require(!ParseCoreGuestLog(positive, static_cast<GuestDiagnosticControl>(99), 0).accepted, "invalid control rejected");
        Require(!ParseCoreGuestLog(positive + '\0', GuestDiagnosticControl::Positive, 0).accepted, "NUL guest log rejected");
        std::string crlf;
        for (const auto byte : positive) { if (byte == '\n') crlf += '\r'; crlf += byte; }
        Require(ParseCoreGuestLog(crlf, GuestDiagnosticControl::Positive, 0).accepted, "Windows CRLF output accepted");
        Require(IsCoreDiagnosticAbsolutePath(L"D:\\LocalState\\logs"), "drive-absolute native directory");
        Require(IsCoreDiagnosticAbsolutePath(L"C:/Package/guest64-smoke.elf"), "absolute slash-separated path");
        Require(IsCoreDiagnosticAbsolutePath(L"C:\\"), "drive root is absolute");
        Require(!IsCoreDiagnosticAbsolutePath(L"C:guest.elf"), "drive-relative path rejected");
        Require(!IsCoreDiagnosticAbsolutePath(L"guest.elf"), "relative path rejected");
        Require(!IsCoreDiagnosticAbsolutePath(L"\\\\server\\share\\guest.elf"), "UNC paths outside contract rejected");
        Require(!IsCoreDiagnosticAbsolutePath(L"C:\\state\\..\\guest.elf"), "parent traversal rejected");
        Require(!IsCoreDiagnosticAbsolutePath(L"C:\\state\\.\\guest.elf"), "dot component rejected");
        Require(!IsCoreDiagnosticAbsolutePath(L"C:\\state\\guest.elf:stream"), "alternate stream rejected");
        Require(!IsCoreDiagnosticAbsolutePath(L"C:\\state\\*.elf"), "wildcard rejected");
        Require(!IsCoreDiagnosticAbsolutePath(std::wstring(L"C:\\state\0x", 10)), "embedded NUL path rejected");
        Require(!IsCoreDiagnosticAbsolutePath(L"C:\\" + std::wstring(32760, L'x')), "oversized native path rejected");
    }
    std::string ReadLog(const std::string& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read known artifact " + path);
        std::string result((std::istreambuf_iterator<char>(input)), {});
        if (result.size() > CoreDiagnosticLogLimit) throw std::runtime_error("known artifact too large");
        return result;
    }
    void ArtifactChecks(const std::string& directory)
    {
        const auto self = ParseCoreSelfTestLog(ReadLog(directory + "/guest64-selftest.log"), 0);
        Require(self.accepted && self.passed == 907 && self.failed == 0,
                "known real artifact has 907 passing upstream checks");
        Require(ParseCoreGuestLog(ReadLog(directory + "/guest64-smoke.log"), GuestDiagnosticControl::Positive, 0).accepted,
                "known real positive ELF artifact passes strict parser");
        Require(ParseCoreGuestLog(ReadLog(directory + "/guest64-negative.log"), GuestDiagnosticControl::Negative, 0).accepted,
                "known real negative ELF artifact passes strict parser");
    }
    void JsonChecks()
    {
        CoreDiagnosticReport report;
        const auto running = CoreDiagnosticReportJson(report, true);
        Require(running.find("\"status\":\"running\"") != std::string::npos,
                "new run serializes running instead of a previous result");
        Require(running.find("\"passed_checks\":null") != std::string::npos &&
                running.find("\"core_return_code\":null") != std::string::npos &&
                running.find("\"guest_exit\":null") != std::string::npos,
                "not-run phases do not invent zero statuses or test counts");
        report.selfTest = ParseCoreSelfTestLog(selfTest, 0);
        report.positive = ParseCoreGuestLog(positive, GuestDiagnosticControl::Positive, 0);
        report.negative = ParseCoreGuestLog(negative, GuestDiagnosticControl::Negative, 0);
        report.status = CoreDiagnosticStatus::Passed;
        const auto passed = CoreDiagnosticReportJson(report);
        Require(passed.find("\"status\":\"passed\"") != std::string::npos &&
                passed.find("\"passed_checks\":907") != std::string::npos &&
                passed.find("\"guest_exit\":9") != std::string::npos,
                "report preserves actual parsed counts and negative exit");
        Require(passed.find("58ecb0fffdd11e05bb62e7f0e80dd1af7c6402a4") != std::string::npos &&
                passed.find("\"wine_executed\":false") != std::string::npos &&
                passed.find("\"xbox_qualification\":\"not_inferred\"") != std::string::npos,
                "report pins source and limits qualification");
        report.status = CoreDiagnosticStatus::Failed;
        report.error = "bad \"input\"\\path\nnext";
        report.selfTestPhase.logPath = L"D:\\LocalState\\\u03b4.log";
        const auto failed = CoreDiagnosticReportJson(report);
        Require(failed.find("\"status\":\"failed\"") != std::string::npos &&
                failed.find("bad \\\"input\\\"\\\\path\\u000anext") != std::string::npos,
                "JSON escapes error quotes, backslashes and control characters");
        Require(failed.find("D:\\\\LocalState\\\\\\u03b4.log") != std::string::npos,
                "JSON preserves a Unicode native log path without an ACP conversion");
    }
}
int main(int argc, char** argv)
{
    try
    {
        ParserChecks();
        JsonChecks();
        for (int i = 1; i < argc; ++i) ArtifactChecks(argv[i]);
        std::printf("Core diagnostic parser: %u checks passed (no core or guest executed)\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Core diagnostic parser check %u failed: %s\n", checks, error.what());
        return 1;
    }
}
