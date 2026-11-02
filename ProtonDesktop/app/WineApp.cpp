// SPDX-License-Identifier: GPL-2.0-only
// Native SDL/WinRT owns CoreApplication, CoreWindow, presentation and input.
// This file does not create a second window or draw a substitute desktop.
#include "backend/runtime/WineSession.h"
#include <SDL.h>
#include <windows.h>
#include <fileapifromapp.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if !defined(__cplusplus_winrt)
#error The native SDL/WinRT application entry must compile with MSVC /ZW.
#endif
#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) || WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#error This application requires genuine UWP APP-family headers.
#endif
#if !defined(__WINRT__)
#error Select the pinned native SDL WinRT headers, not the desktop SDL profile.
#endif
#pragma warning(disable:4447) // SDL_WinRTRunApp initializes the real WinRT MTA.

namespace
{
    using namespace protondesktop::runtime;

    class FileHandle
    {
    public:
        explicit FileHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
        ~FileHandle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;
        FileHandle(FileHandle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
        FileHandle& operator=(FileHandle&&) = delete;
        HANDLE Get() const noexcept { return value_; }
    private:
        HANDLE value_;
    };

    [[noreturn]] void FileFailure(const char* operation)
    {
        throw std::runtime_error(std::string(operation) + " failed; Win32 error " + std::to_string(GetLastError()));
    }

    std::string Utf8(const std::wstring& value)
    {
        if (value.empty()) return {};
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::runtime_error("Native path is too long to convert without truncation");
        const auto count = static_cast<int>(value.size());
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), count, nullptr, 0, nullptr, nullptr);
        if (!bytes) FileFailure("Strict UTF-16 to UTF-8 conversion");
        std::string result(static_cast<std::size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), count,
                               result.data(), bytes, nullptr, nullptr) != bytes)
            FileFailure("Strict UTF-16 to UTF-8 conversion");
        return result;
    }

    std::wstring NativePath(Platform::String^ value)
    {
        if (!value || !value->Length()) throw std::runtime_error("The package or app-local path is empty");
        return std::wstring(value->Data(), value->Length());
    }

    HANDLE OpenFile(const std::wstring& path, DWORD access, DWORD sharing, DWORD disposition)
    {
        CREATEFILE2_EXTENDED_PARAMETERS parameters{};
        parameters.dwSize = sizeof(parameters);
        parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        const auto result = CreateFile2FromAppW(path.c_str(), access, sharing, disposition, &parameters);
        if (result == INVALID_HANDLE_VALUE) FileFailure("Opening an app-local file");
        return result;
    }

    void WriteBytes(HANDLE file, const std::string& value)
    {
        std::size_t written = 0;
        while (written < value.size())
        {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(value.size() - written, 1024 * 1024));
            DWORD actual = 0;
            if (!WriteFile(file, value.data() + written, chunk, &actual, nullptr)) FileFailure("Writing session evidence");
            if (!actual) throw std::runtime_error("Writing session evidence made no progress");
            written += actual;
        }
        if (!FlushFileBuffers(file)) FileFailure("Flushing session evidence");
    }

    class SessionLog
    {
    public:
        explicit SessionLog(const std::wstring& path)
            : file_(OpenFile(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS)) {}
        void Line(const std::string& text) { WriteBytes(file_.Get(), text + "\r\n"); }
    private:
        FileHandle file_;
    };

    std::string JsonString(const std::string& value)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string result = "\"";
        for (const unsigned char ch : value)
        {
            if (ch == '\\' || ch == '"') { result += '\\'; result += static_cast<char>(ch); }
            else if (ch < 0x20) { result += "\\u00"; result += digits[ch >> 4]; result += digits[ch & 15]; }
            else result += static_cast<char>(ch);
        }
        return result + '"';
    }

    void WriteResult(const std::wstring& path, const char* state, const SessionResult* result,
                     const std::string& error)
    {
        std::ostringstream json;
        json << "{\n  \"status\": " << JsonString(state)
             << ",\n  \"entry\": \"native-SDL-WinRT\",\n  \"desktopPixelsVerified\": false,\n  \"error\": " << JsonString(error);
        if (result)
        {
            const auto command = [&](const char* name, const GuestCommandResult& value)
            {
                json << ",\n  \"" << name << "\": {\"attempted\":" << (value.attempted ? "true" : "false")
                     << ",\"loaded\":" << (value.loaded ? "true" : "false")
                     << ",\"exited\":" << (value.exited ? "true" : "false")
                     << ",\"pid\":" << value.pid << ",\"exitCode\":" << value.exitCode << '}';
            };
            command("wineVersion", result->version);
            command("wineboot", result->prefix);
            command("explorer", result->requested);
            json << ",\n  \"cleanShutdown\":" << (result->cleanShutdown ? "true" : "false")
                 << ",\n  \"stopped\":" << (result->stopped ? "true" : "false")
                 << ",\n  \"prefixFilesPresent\":" << (result->prefixFilesPresent ? "true" : "false");
        }
        json << "\n}\n";
        FileHandle file(OpenFile(path, GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS));
        WriteBytes(file.Get(), json.str());
    }

    void EnsureDirectory(const std::wstring& path)
    {
        if (!CreateDirectoryFromAppW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            FileFailure("Creating the app-owned writable overlay");
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &attributes))
            FileFailure("Checking the app-owned writable overlay");
        if (!(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("WinePrefix must be an ordinary app-owned directory");
    }

    struct HashAlgorithm
    {
        BCRYPT_ALG_HANDLE value = nullptr;
        ~HashAlgorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
    };
    struct HashState
    {
        BCRYPT_HASH_HANDLE value = nullptr;
        ~HashState() { if (value) BCryptDestroyHash(value); }
    };
    void CheckCng(NTSTATUS result)
    {
        if (result < 0) throw std::runtime_error("SHA-256 verification failed; CNG status " + std::to_string(result));
    }

    // Keep this read-only lease alive until the runtime stops. The same app's
    // download/upload path cannot replace/write the ZIP after its hash passes.
    FileHandle VerifyLayer(const std::wstring& path, std::uint64_t expectedBytes,
                           const char* expectedDigest)
    {
        FileHandle file(OpenFile(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING));
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file.Get(), &length)) FileFailure("Reading rootfs layer size");
        if (length.QuadPart < 0 || static_cast<std::uint64_t>(length.QuadPart) != expectedBytes)
            throw std::runtime_error("Rootfs layer size differs from the pinned local candidate");
        HashAlgorithm algorithm;
        CheckCng(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        DWORD objectBytes = 0, actual = 0;
        CheckCng(BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &actual, 0));
        if (actual != sizeof(objectBytes) || !objectBytes) throw std::runtime_error("Invalid CNG SHA-256 object size");
        std::vector<unsigned char> object(objectBytes);
        HashState hash;
        CheckCng(BCryptCreateHash(algorithm.value, &hash.value, object.data(), objectBytes, nullptr, 0, 0));
        std::vector<unsigned char> bytes(1024 * 1024);
        std::uint64_t total = 0;
        for (;;)
        {
            DWORD read = 0;
            if (!ReadFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
                FileFailure("Reading the pinned rootfs layer");
            if (!read) break;
            total += read;
            CheckCng(BCryptHashData(hash.value, bytes.data(), read, 0));
        }
        if (total != expectedBytes) throw std::runtime_error("The pinned rootfs layer read was incomplete");
        std::array<unsigned char, 32> digest{};
        CheckCng(BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0));
        static constexpr char digits[] = "0123456789abcdef";
        std::string actualDigest;
        for (const auto byte : digest) { actualDigest += digits[byte >> 4]; actualDigest += digits[byte & 15]; }
        if (actualDigest != expectedDigest) throw std::runtime_error("Rootfs layer SHA-256 differs from the pinned local candidate");
        return file;
    }

    const char* PhaseName(SessionPhase phase) noexcept
    {
        switch (phase)
        {
        case SessionPhase::Validation: return "validation";
        case SessionPhase::WineVersion: return "wine-version";
        case SessionPhase::PrefixInitialization: return "wineboot";
        case SessionPhase::ExplorerDesktop: return "explorer-desktop";
        case SessionPhase::Stopping: return "stopping";
        case SessionPhase::Finished: return "finished";
        }
        return "unknown";
    }

    int RunDesktop()
    {
        const auto local = NativePath(Windows::Storage::ApplicationData::Current->LocalFolder->Path);
        const auto installed = NativePath(Windows::ApplicationModel::Package::Current->InstalledLocation->Path);
        SessionLog log(local + L"\\protondesktop-wine-session.log");
        const auto resultPath = local + L"\\protondesktop-wine-result.json";
        try
        {
            WriteResult(resultPath, "validating", nullptr, "");
            log.Line("Native SDL/WinRT session entry; no replacement desktop or second CoreApplication.");
            log.Line("Package=" + Utf8(installed));
            log.Line("LocalState=" + Utf8(local));
            const auto runtimeDirectory = local + L"\\Runtimes";
            const auto glibcPath = runtimeDirectory + L"\\glibc-rootfs64.zip";
            const auto winePath = runtimeDirectory + L"\\wine64.zip";
            log.Line("Verifying the two fixed app-local rootfs candidate ZIPs before guest execution.");
            auto glibcLease = VerifyLayer(glibcPath, 2850458,
                "2507d717e4f860789ea54fa0d73a8db8b152827f74ee24d2665c81757b439822");
            auto wineLease = VerifyLayer(winePath, 276707863,
                "a2614a5fbda1e4783f30b2e0fb3a66759c2649a1d86250a386191ba43a8cc0ea");
            log.Line("Both exact rootfs layer sizes/SHA-256 hashes verified; read leases retained.");
            const auto overlay = local + L"\\WinePrefix";
            EnsureDirectory(overlay); // Preserve existing prefix data; never erase it.
            SessionRequest request;
            request.rootPath = Utf8(overlay);
            request.zipLayers = {Utf8(glibcPath), Utf8(winePath)};
            request.width = 1920;
            request.height = 1080;
            request.initializePrefixIfNeeded = true;
            request.environment = {"HOME=/home/username", "USER=username", "WINEARCH=win64",
                "WINEPREFIX=/home/username/.wine", "WINELOADER=/usr/lib/wine/wine64",
                "WINESERVER=/usr/lib/wine/wineserver64", "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine",
                "LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/wine/x86_64-unix:/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu",
                "DISPLAY=:0", "WINEDLLOVERRIDES=mscoree,mshtml="};
            request.argv = {"/usr/lib/wine/wine64", "explorer.exe", "/desktop=ProtonDesktop,1920x1080"};
            const auto stop = std::make_shared<std::atomic<bool>>(false);
            bool evidenceFailed = false;
            WriteResult(resultPath, "running", nullptr, "");
            const auto result = RunWineSession(request, [&](SessionPhase phase, const std::string& message) noexcept
            {
                try { log.Line(std::string(PhaseName(phase)) + ": " + message); }
                catch (...) { evidenceFailed = true; stop->store(true); OutputDebugStringW(L"Wine session evidence failed; stop requested.\n"); }
            }, stop);
            const auto error = evidenceFailed ? std::string("Session evidence failed; shutdown was requested") : result.error;
            WriteResult(resultPath, error.empty() && result.cleanShutdown ? "stopped-cleanly" : "failed", &result, error);
            log.Line("Session returned; cleanShutdown=" + std::to_string(result.cleanShutdown) + "; error=" + error);
            return error.empty() && result.cleanShutdown ? 0 : 1;
        }
        catch (const std::exception& error)
        {
            try { log.Line(std::string("FAILED: ") + error.what()); WriteResult(resultPath, "failed", nullptr, error.what()); }
            catch (...) { OutputDebugStringW(L"Failed session evidence could not be persisted.\n"); }
            return 1;
        }
        catch (Platform::Exception^ error)
        {
            try { const auto message = Utf8(NativePath(error->Message)); log.Line("FAILED: " + message); WriteResult(resultPath, "failed", nullptr, message); }
            catch (...) { OutputDebugStringW(L"Failed WinRT session evidence could not be persisted.\n"); }
            return 1;
        }
    }
}

extern "C" int SDL_main(int, char**)
{
    try { return RunDesktop(); }
    catch (...) { OutputDebugStringW(L"ProtonDesktop Wine app could not establish app-local session evidence.\n"); return 1; }
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // SDL initializes the MTA and runs its existing native IFrameworkView.
    // Never run the diagnostic App.cpp CoreApplication/swapchain alongside it.
    try { return SDL_WinRTRunApp(SDL_main, nullptr); }
    catch (Platform::Exception^ error) { OutputDebugStringW(error->Message->Data()); return static_cast<int>(error->HResult); }
    catch (...) { OutputDebugStringW(L"Native SDL/WinRT startup failed.\n"); return 1; }
}
