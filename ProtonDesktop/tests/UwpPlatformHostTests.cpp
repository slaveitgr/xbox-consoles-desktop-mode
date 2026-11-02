// SPDX-License-Identifier: GPL-2.0-only
#include "backend/uwp/UwpHostServices.h"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#include <fileapifromapp.h>
#endif
using namespace protondesktop::uwp;
namespace
{
    unsigned checks = 0;
    void Require(bool condition, const char* name)
    {
        ++checks;
        if (!condition) throw std::runtime_error(name);
    }
    void PolicyChecks()
    {
        std::uint64_t value = 77;
        Require(FileTimeToUnixMicroseconds(FileTimeUnixEpoch, &value) && value == 0,
                "Unix epoch conversion");
        Require(FileTimeToUnixMicroseconds(FileTimeUnixEpoch + 19, &value) && value == 1,
                "FILETIME fractional microseconds truncate");
        Require(!FileTimeToUnixMicroseconds(FileTimeUnixEpoch - 1, &value) && value == 1,
                "pre-epoch timestamps fail without unsigned wrap");
        Require(!FileTimeToUnixMicroseconds(FileTimeUnixEpoch, nullptr), "null wallclock output fails");
        Require(CounterTicksToMicroseconds(12345678, 10000000, &value) && value == 1234567,
                "QPC ticks with subsecond remainder");
        Require(CounterTicksToMicroseconds(5, 3, &value) && value == 1666666,
                "non-divisible QPC frequency");
        Require(!CounterTicksToMicroseconds(1, 0, &value) && value == 1666666,
                "zero QPC frequency fails without changing output");
        Require(!CounterTicksToMicroseconds(1, 1, nullptr), "null counter output fails");
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        Require(!CounterTicksToMicroseconds(maximum, 1, &value), "counter scaling overflow fails");
        Require(!CounterTicksToMicroseconds(0, maximum / 1000000 + 1, &value),
                "unrepresentable frequency scaling fails");
        Require(CounterTicksToMicroseconds(0, maximum / 1000000, &value) && value == 0,
                "maximum supported counter frequency boundary");
        Require(RelativeTimerDueTime(0) == 0, "zero duration handled by yield path");
        Require(RelativeTimerDueTime(1) == -1, "sub-100ns timer rounds up");
        Require(RelativeTimerDueTime(100) == -1, "exact 100ns timer unit");
        Require(RelativeTimerDueTime(101) == -2, "fractional 100ns timer rounds up");
        Require(RelativeTimerDueTime(maximum) == -184467440737095517LL,
                "maximum nanoseconds convert without overflow");
    }
#ifdef _WIN32
    struct Scratch
    {
        std::wstring root;
        ~Scratch()
        {
            DeleteFileFromAppW((root + L"\\file.txt").c_str());
            DeleteFileFromAppW((root + L"\\\u03b4\u03bf\u03ba\u03b9\u03bc\u03ae.txt").c_str());
            RemoveDirectoryFromAppW((root + L"\\folder").c_str());
            RemoveDirectoryFromAppW(root.c_str());
        }
    };
    void CreateTestFile(const std::wstring& path)
    {
        const auto file = CreateFileFromAppW(path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(file != INVALID_HANDLE_VALUE, "create file using app-aware API");
        Require(CloseHandle(file) != FALSE, "close scratch file handle");
    }
    bool Has(const std::vector<DirectoryEntry>& values, const std::string& name, bool directory)
    {
        return std::any_of(values.begin(), values.end(), [&](const auto& entry) {
            return entry.utf8Name == name && entry.isDirectory == directory;
        });
    }
    void NativeChecks(const char* scratchParent)
    {
        std::uint64_t before = 0, after = 0, wallclock = 0;
        std::uint32_t cpuCount = 0;
        Require(ResetMicroCounter(), "reset real QPC epoch");
        Require(ReadMicroCounter(&before), "read real QPC counter");
        Require(SleepNanoseconds(2000000), "real relative waitable timer completes");
        Require(ReadMicroCounter(&after) && after >= before, "QPC is monotonic across timer wait");
        Require(after > before, "timer wait advances the counter");
        Require(SleepNanoseconds(0), "zero nanosleep yields");
        Require(ReadUnixMicroseconds(&wallclock) && wallclock > 1577836800000000ULL,
                "wallclock is a plausible Unix microsecond timestamp");
        Require(ReadCpuCount(&cpuCount) && cpuCount > 0, "real host processor count");
        SetLastError(0);
        Require(!ReadMicroCounter(nullptr) && GetLastError() == ERROR_INVALID_PARAMETER,
                "null counter output retains explicit Win32 error");
        std::wstring wide;
        std::string utf8;
        const std::string unicode = "\xCE\xB4\xCE\xBF\xCE\xBA\xCE\xB9\xCE\xBC\xCE\xAE.txt";
        Require(Utf8ToWide(unicode, &wide) && WideToUtf8(wide, &utf8) && utf8 == unicode,
                "UTF-8 Greek filename round trip");
        Require(!Utf8ToWide(std::string("\xC0\xAF", 2), &wide), "overlong invalid UTF-8 is rejected");
        Require(!Utf8ToWide(std::string("x\0y", 3), &wide), "embedded UTF-8 NUL is rejected");
        Require(!WideToUtf8(std::wstring(1, wchar_t(0xD800)), &utf8), "unpaired UTF-16 surrogate is rejected");
        Require(Utf8ToWide(scratchParent, &wide), "scratch parent path conversion");
        LARGE_INTEGER nonce{};
        Require(QueryPerformanceCounter(&nonce) != FALSE, "unique scratch suffix from real QPC");
        auto scratchPath = wide + L"\\protondesktop-host-" + std::to_wstring(nonce.QuadPart);
        Require(CreateDirectoryFromAppW(scratchPath.c_str(), nullptr) != FALSE,
                "create app-aware test directory");
        Scratch scratch{std::move(scratchPath)};
        Require(WideToUtf8(scratch.root, &utf8), "scratch path UTF-8 conversion");
        std::vector<DirectoryEntry> entries{{"sentinel", true}};
        Require(EnumerateDirectory(utf8, &entries) && entries.empty(),
                "real empty directory returns an empty snapshot");
        CreateTestFile(scratch.root + L"\\file.txt");
        CreateTestFile(scratch.root + L"\\\u03b4\u03bf\u03ba\u03b9\u03bc\u03ae.txt");
        Require(CreateDirectoryFromAppW((scratch.root + L"\\folder").c_str(), nullptr) != FALSE,
                "create app-aware child directory");
        Require(EnumerateDirectory(utf8, &entries) && entries.size() == 3,
                "real directory enumeration omits dot entries");
        Require(Has(entries, "file.txt", false) && Has(entries, unicode, false) && Has(entries, "folder", true),
                "enumeration preserves Unicode names and directory attributes");
        Require(!EnumerateDirectory(utf8 + "\\missing", &entries) && entries.size() == 3,
                "missing path fails and preserves previous snapshot");
        Require(!EnumerateDirectory(utf8 + "\\file.txt", &entries) && entries.size() == 3,
                "file-as-directory fails and preserves previous snapshot");
        Require(!EnumerateDirectory(utf8 + "\\*", &entries) && entries.size() == 3,
                "caller wildcard rejected without partial results");
        Require(!EnumerateDirectory("", &entries), "empty native path rejected");
        Require(!EnumerateDirectory(utf8, nullptr), "null listing output rejected");
    }
#endif
}
int main(int argc, char** argv)
{
    try
    {
        PolicyChecks();
#ifdef _WIN32
        Require(argc == 2, "pass an existing writable scratch parent directory");
        NativeChecks(argv[1]);
        std::printf("UWP host services: %u checks passed (Windows APIs; not an Xbox run)\n", checks);
#else
        (void)argc; (void)argv;
        std::printf("UWP host conversion policy: %u checks passed (Windows APIs not executed)\n", checks);
#endif
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "UWP host check %u failed: %s\n", checks, error.what());
        return 1;
    }
}
