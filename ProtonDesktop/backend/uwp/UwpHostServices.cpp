// SPDX-License-Identifier: GPL-2.0-only
#include "UwpHostServices.h"
#ifndef _WIN32
#error UwpHostServices.cpp requires the Windows SDK.
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <fileapifromapp.h>
#include <atomic>
#include <climits>
#include <cwchar>

namespace protondesktop::uwp
{
    namespace
    {
        struct CounterState
        {
            std::uint64_t frequency = 0;
            std::atomic<std::uint64_t> epoch{0};
            CounterState() noexcept
            {
                LARGE_INTEGER rate{}, now{};
                if (QueryPerformanceFrequency(&rate) && rate.QuadPart > 0 &&
                    QueryPerformanceCounter(&now) && now.QuadPart >= 0 &&
                    static_cast<std::uint64_t>(rate.QuadPart) <=
                        std::numeric_limits<std::uint64_t>::max() / 1000000ULL)
                {
                    frequency = static_cast<std::uint64_t>(rate.QuadPart);
                    epoch.store(static_cast<std::uint64_t>(now.QuadPart), std::memory_order_relaxed);
                }
            }
        };
        CounterState& Counter() noexcept { static CounterState state; return state; }
        bool Fail(DWORD error) noexcept { SetLastError(error); return false; }
        struct FindHandle
        {
            HANDLE value;
            ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
        };
    }
    bool ResetMicroCounter() noexcept
    {
        auto& counter = Counter();
        if (!counter.frequency) return Fail(ERROR_NOT_SUPPORTED);
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now)) return false;
        if (now.QuadPart < 0) return Fail(ERROR_INVALID_DATA);
        counter.epoch.store(static_cast<std::uint64_t>(now.QuadPart), std::memory_order_release);
        return true;
    }
    bool ReadMicroCounter(std::uint64_t* output) noexcept
    {
        if (!output) return Fail(ERROR_INVALID_PARAMETER);
        auto& counter = Counter();
        if (!counter.frequency) return Fail(ERROR_NOT_SUPPORTED);
        const auto epoch = counter.epoch.load(std::memory_order_acquire);
        LARGE_INTEGER now{};
        if (!QueryPerformanceCounter(&now)) return false;
        if (now.QuadPart < 0 || static_cast<std::uint64_t>(now.QuadPart) < epoch)
            return Fail(ERROR_INVALID_DATA);
        if (!CounterTicksToMicroseconds(static_cast<std::uint64_t>(now.QuadPart) - epoch,
                                       counter.frequency, output))
            return Fail(ERROR_ARITHMETIC_OVERFLOW);
        return true;
    }
    bool ReadUnixMicroseconds(std::uint64_t* output) noexcept
    {
        if (!output) return Fail(ERROR_INVALID_PARAMETER);
        FILETIME time{};
        GetSystemTimeAsFileTime(&time);
        const auto ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        if (!FileTimeToUnixMicroseconds(ticks, output)) return Fail(ERROR_INVALID_DATA);
        return true;
    }
    bool SleepNanoseconds(std::uint64_t nanoseconds) noexcept
    {
        if (!nanoseconds) { Sleep(0); return true; }
        // A private synchronization timer prevents DWORD millisecond truncation
        // and avoids spin-waiting. Real precision remains hardware-dependent.
        const auto timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer) return false;
        LARGE_INTEGER due{};
        due.QuadPart = RelativeTimerDueTime(nanoseconds);
        if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
        {
            const auto error = GetLastError();
            CloseHandle(timer);
            return Fail(error);
        }
        const auto wait = WaitForSingleObjectEx(timer, INFINITE, FALSE);
        const auto error = wait == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA;
        CloseHandle(timer);
        return wait == WAIT_OBJECT_0 ? true : Fail(error);
    }
    bool ReadCpuCount(std::uint32_t* output) noexcept
    {
        if (!output) return Fail(ERROR_INVALID_PARAMETER);
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        if (!info.dwNumberOfProcessors) return Fail(ERROR_INVALID_DATA);
        *output = info.dwNumberOfProcessors;
        return true;
    }
    bool Utf8ToWide(const std::string& source, std::wstring* output)
    {
        if (!output || source.size() > static_cast<std::size_t>(INT_MAX) ||
            source.find('\0') != std::string::npos) return Fail(ERROR_INVALID_PARAMETER);
        if (source.empty()) { output->clear(); return true; }
        const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            source.data(), static_cast<int>(source.size()), nullptr, 0);
        if (!count) return false;
        std::wstring result(static_cast<std::size_t>(count), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source.data(),
                static_cast<int>(source.size()), result.data(), count)) return false;
        *output = std::move(result);
        return true;
    }
    bool WideToUtf8(const std::wstring& source, std::string* output)
    {
        if (!output || source.size() > static_cast<std::size_t>(INT_MAX) ||
            source.find(L'\0') != std::wstring::npos) return Fail(ERROR_INVALID_PARAMETER);
        if (source.empty()) { output->clear(); return true; }
        const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            source.data(), static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr);
        if (!count) return false;
        std::string result(static_cast<std::size_t>(count), '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, source.data(),
                static_cast<int>(source.size()), result.data(), count, nullptr, nullptr)) return false;
        *output = std::move(result);
        return true;
    }
    bool EnumerateDirectory(const std::string& utf8Path, std::vector<DirectoryEntry>* output)
    {
        if (!output || utf8Path.empty() || utf8Path.find_first_of("*?") != std::string::npos)
            return Fail(ERROR_INVALID_PARAMETER);
        std::wstring pattern;
        if (!Utf8ToWide(utf8Path, &pattern)) return false;
        // An extended-length path prefix needs a separately reviewed
        // normalization policy; '?' is rejected above today.
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExFromAppW(pattern.c_str(), GetFileExInfoStandard, &attributes))
            return false;
        if (!(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            return Fail(ERROR_DIRECTORY);
        if (pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
        pattern += L'*';
        WIN32_FIND_DATAW data{};
        auto find = FindFirstFileExFromAppW(pattern.c_str(), FindExInfoBasic, &data,
                                          FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) { output->clear(); return true; }
            return false;
        }
        std::vector<DirectoryEntry> result;
        DWORD error = ERROR_SUCCESS;
        {
            FindHandle handle{find};
            do
            {
                if (std::wcscmp(data.cFileName, L".") && std::wcscmp(data.cFileName, L".."))
                {
                    std::string name;
                    if (!WideToUtf8(data.cFileName, &name)) { error = GetLastError(); break; }
                    result.push_back({std::move(name), (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
                }
                if (!FindNextFileW(handle.value, &data))
                {
                    error = GetLastError();
                    break;
                }
            } while (true);
        }
        if (error != ERROR_NO_MORE_FILES) return Fail(error);
        *output = std::move(result);
        return true;
    }
}
