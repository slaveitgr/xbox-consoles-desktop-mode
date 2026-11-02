// SPDX-License-Identifier: GPL-2.0-only
#ifdef _WIN32
// Windows establishes SDK architecture macros; load all SDK/WinRT headers
// before X11's Status macro and SIMDe's intrinsic aliases enter this TU.
#include <windows.h>
#include <fileapifromapp.h>
#include <winrt/Windows.Storage.h>
#endif
#include "backend/runtime/WineSession.h"
#include "backend/uwp/UwpGuestWorkers.h"
#include "boxedwine.h"
#include "startupArgs.h"
#include "fszip.h"
#include "knativesystem.h"
#include "knativeinput.h"
#include "knativescreen.h"
#include "knativeaudio.h"
#include "devdsp.h"
#include "x11.h"
#include "x11wire/xwirepresent.h"
#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#ifndef BOXEDWINE_GUEST_X64
#error WineSession requires the actual pinned x64 guest interpreter.
#endif
#ifndef BOXEDWINE_ZLIB
#error WineSession requires genuine ZIP filesystem support.
#endif

void protonSetSdlOwnerThread(bool value);
void protonConfigureXWireDesktop(unsigned width, unsigned height);
U32 getNextTimer();
void runTimers();
bool flipFB();

namespace protondesktop::runtime
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        std::atomic<bool> sessionAttempted{false};
        constexpr const char* WineLoader = "/usr/lib/wine/wine64";
        constexpr const char* WineServer = "/usr/lib/wine/wineserver64";

        bool TextValid(const std::string& value)
        {
            return !value.empty() && value.size() <= 32768 &&
                value.find_first_of(std::string("\0\r\n", 3)) == std::string::npos;
        }
        void Require(bool condition, const char* message)
        { if (!condition) throw std::runtime_error(message); }

        void ValidateContract(const SessionRequest& request)
        {
            Require(TextValid(request.rootPath), "A native writable root path is required");
            Require(request.zipLayers.size() == 2, "Exactly the glibc and Wine64 base ZIPs are required");
            Require(request.zipLayers[0] != request.zipLayers[1], "Base ZIPs must be distinct");
            for (const auto& path : request.zipLayers) Require(TextValid(path), "A base ZIP path is invalid");
            Require(request.width >= 640 && request.width <= 3840 &&
                request.height >= 480 && request.height <= 2160, "Guest desktop dimensions are invalid");
            Require(request.argv.size() >= 2 && request.argv.size() <= 1024 && request.argv[0] == WineLoader,
                "The typed launch must use /usr/lib/wine/wine64 and a Windows program");
            for (const auto& arg : request.argv) Require(TextValid(arg), "A guest argument is invalid");
            std::unordered_set<std::string> keys;
            for (const auto& entry : request.environment)
            {
                Require(TextValid(entry), "A guest environment entry is invalid");
                const auto equals = entry.find('=');
                Require(equals != std::string::npos && equals > 0, "Guest environment entries need NAME=value");
                Require(keys.insert(entry.substr(0, equals)).second, "Duplicate guest environment keys are rejected");
            }
            for (const char* required : {"HOME=/home/username", "USER=username", "WINEARCH=win64",
                "WINEPREFIX=/home/username/.wine", "WINELOADER=/usr/lib/wine/wine64",
                "WINESERVER=/usr/lib/wine/wineserver64", "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine",
                "DISPLAY=:0", "WINEDLLOVERRIDES=mscoree,mshtml="})
                Require(std::find(request.environment.begin(), request.environment.end(), required) !=
                    request.environment.end(), "The required owned Wine64 environment is missing or inconsistent");
        }

#ifdef _WIN32
        std::wstring NativeWidePath(const std::string& value)
        {
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                value.data(), static_cast<int>(value.size()), nullptr, 0);
            Require(length > 0, "A native path is not valid UTF-8");
            std::wstring path(static_cast<std::size_t>(length), L'\0');
            Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), path.data(), length) == length, "Native path conversion failed");
            // Upstream mixes narrow CRT file I/O and our UTF-8 directory API.
            // With a legacy ACP there is no single lossless non-ASCII encoding.
            Require(GetACP() == CP_UTF8 || std::all_of(value.begin(), value.end(),
                [](unsigned char c) { return c < 128; }),
                "This system's narrow upstream filesystem requires ASCII native paths (or a UTF-8 ACP)");
            Require(path.size() > 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
                (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' &&
                (path[2] == L'\\' || path[2] == L'/'), "Native paths must be drive-absolute");
            std::replace(path.begin(), path.end(), L'/', L'\\');
            Require(path.find_first_of(L"*?\"<>|") == std::wstring::npos &&
                path.find(L':', 2) == std::wstring::npos, "Native paths cannot contain devices, wildcards or streams");
            std::size_t begin = 3;
            while (begin < path.size())
            {
                const auto end = path.find(L'\\', begin);
                const auto component = path.substr(begin, end == std::wstring::npos ? end : end - begin);
                Require(!component.empty() && component != L"." && component != L".." &&
                    component.back() != L'.' && component.back() != L' ', "Native path components are not canonical");
                if (end == std::wstring::npos) break;
                begin = end + 1;
            }
            Require(path.back() != L'\\', "Native file/root paths must not end in a separator");
            return path;
        }
        bool Attributes(const std::wstring& path, DWORD& attributes)
        {
            WIN32_FILE_ATTRIBUTE_DATA info{};
            if (!GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &info)) return false;
            attributes = info.dwFileAttributes;
            return true;
        }
        bool RegistryFile(const std::wstring& path)
        {
            DWORD attributes = 0;
            if (!Attributes(path, attributes) || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                return false;
            FILE* file = nullptr;
            if (_wfopen_s(&file, path.c_str(), L"rb") || !file) return false;
            constexpr char expected[] = "WINE REGISTRY Version 2\n";
            char header[sizeof(expected) - 1]{};
            const auto count = std::fread(header, 1, sizeof(header), file);
            std::fclose(file);
            return count == sizeof(header) && std::string(header, sizeof(header)) == expected;
        }
        bool PrefixFilesPresent(const std::wstring& root)
        {
            const auto prefix = root + L"\\home\\username\\.wine";
            DWORD attributes = 0;
            return RegistryFile(prefix + L"\\system.reg") && RegistryFile(prefix + L"\\user.reg") &&
                RegistryFile(prefix + L"\\userdef.reg") &&
                Attributes(prefix + L"\\drive_c\\windows", attributes) && (attributes & FILE_ATTRIBUTE_DIRECTORY);
        }
        std::wstring ValidateNativeFiles(const SessionRequest& request)
        {
            const auto root = NativeWidePath(request.rootPath);
            const auto localValue = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
            std::wstring local(localValue.c_str(), localValue.size());
            while (!local.empty() && local.back() == L'\\') local.pop_back();
            Require(root.size() > local.size() + 1 && root[local.size()] == L'\\' &&
                CompareStringOrdinal(root.data(), static_cast<int>(local.size()), local.data(),
                    static_cast<int>(local.size()), TRUE) == CSTR_EQUAL,
                "The writable root must be a descendant of this app's real LocalState directory");
            // Check every descendant component. Do not follow a reparse point
            // into another directory, even when the textual prefix looks owned.
            for (std::size_t end = local.size();;)
            {
                DWORD attributes = 0;
                Require(Attributes(root.substr(0, end), attributes) &&
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
                    "The writable root is inaccessible, not a directory, or crosses a reparse point");
                if (end == root.size()) break;
                end = root.find(L'\\', end + 1);
                if (end == std::wstring::npos) end = root.size();
            }
            for (const auto& zip : request.zipLayers)
            {
                DWORD attributes = 0;
                const auto path = NativeWidePath(zip);
                Require(Attributes(path, attributes) &&
                    !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)),
                    "A base ZIP is inaccessible, a directory or a reparse point");
            }
            // Prove actual create/write/delete access without overwriting data.
            const auto marker = root + L"\\.protondesktop-write-probe-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
            CREATEFILE2_EXTENDED_PARAMETERS parameters{};
            parameters.dwSize = sizeof(parameters);
            parameters.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
            parameters.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
            HANDLE handle = CreateFile2FromAppW(marker.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                0, CREATE_NEW, &parameters);
            Require(handle != INVALID_HANDLE_VALUE, "Creating a new app-owned writable probe file failed");
            const char byte = 'P'; DWORD written = 0;
            const bool wrote = WriteFile(handle, &byte, 1, &written, nullptr) && written == 1;
            const bool closed = CloseHandle(handle) != FALSE;
            Require(wrote && closed, "Writing/closing the app-owned storage probe failed");
            return root;
        }
#else
        using NativeRoot = std::string;
        NativeRoot ValidateNativeFiles(const SessionRequest&)
        { throw std::runtime_error("WineSession app-local ownership verification requires the Windows UWP host"); }
        bool PrefixFilesPresent(const NativeRoot&) { return false; }
#endif

        struct SessionState
        {
            const SessionObserver& observer;
            const std::shared_ptr<std::atomic<bool>>& externalStop;
            SessionResult result;
            SessionPhase phase = SessionPhase::Validation;
            std::atomic<bool> closeEvent{false};
            void Note(SessionPhase value, const std::string& text)
            {
                phase = value;
                if (observer) observer(value, text);
            }
            bool StopRequested() const
            { return closeEvent.load() || (externalStop && externalStop->load()); }
        };
        // Accessed only by the one SDL owner thread, including the real XWire
        // submit hook. Never retained or called by a guest worker.
        thread_local SessionState* activeSession = nullptr;

        int SDLCALL WatchLifecycle(void* userdata, SDL_Event* event)
        {
            if (event->type == SDL_QUIT || event->type == SDL_APP_TERMINATING)
                static_cast<SessionState*>(userdata)->closeEvent.store(true);
            return 1;
        }
        void PumpKernel()
        {
            auto screen = KNativeSystem::getScreen();
            if (!screen) { SDL_PumpEvents(); return; }
            if (XServer* server = XServer::getServer(true))
            { server->isDisplayDirty = true; server->draw(); }
            tickXWirePresent();
            flipFB();
            const U32 timer = getNextTimer();
            if (!timer) runTimers();
            KNativeSystem::tick();
            const auto input = KNativeSystem::getCurrentInput();
            input->waitForEvent(std::min<U32>(17, timer ? timer : 17));
            input->processEvents(); // sole platform SDL event pump
        }
        std::vector<BString> GuestStrings(const std::vector<std::string>& values)
        {
            std::vector<BString> result;
            result.reserve(values.size());
            for (const auto& value : values) result.push_back(BString::copy(value.c_str()));
            return result;
        }
        KProcessPtr Launch(GuestCommandResult& result, const std::vector<std::string>& argv,
                           const std::vector<BString>& environment)
        {
            result.attempted = true;
            auto process = KProcess::create();
            result.pid = process->id;
            // The checked KProcess overlay only returns nonnull after a true
            // ELF load AND successful admission of the owned host worker.
            const auto thread = process->startProcess(B("/home/username"), GuestStrings(argv),
                environment, 1000, 1000, 1000, 1000);
            result.loaded = thread != nullptr;
            Require(result.loaded, "Guest ELF loading or owned worker creation failed");
            return process;
        }
        bool ObserveExit(const KProcessPtr& process, GuestCommandResult& result)
        {
            // Caller reaps first and holds no kernel locks. A false per-PID
            // active result is an OS-thread JOIN barrier, not a guest count.
            if (uwp::ProcessHasActiveGuestWorkers(process->id)) return false;
            Require(process->isTerminated(), "Guest CPU worker ended without a recorded process exit");
            result.exited = true;
            result.exitCode = static_cast<std::int32_t>(process->exitCode);
            return true;
        }
        void Reap()
        {
            std::string error;
            Require(uwp::ReapCompletedGuestWorkers(error), error.c_str());
            const auto snapshot = uwp::GetGuestWorkerSnapshot();
            Require(!snapshot.failed, snapshot.error.empty() ? "An owned guest worker failed" : snapshot.error.c_str());
        }
    }

    void RecordGuestFrame(unsigned width, unsigned height)
    {
        if (!activeSession) return;
        auto& state = *activeSession;
        ++state.result.guestFramesSubmitted;
        if (state.result.guestFramesSubmitted == 1)
            state.Note(state.phase, "Real XWire guest frame passed through SDL; present call returned: " +
                std::to_string(width) + "x" + std::to_string(height) +
                "; physical presentation and Explorer interaction require device verification");
    }

    SessionResult RunWineSession(const SessionRequest& request, const SessionObserver& observer,
                                const std::shared_ptr<std::atomic<bool>>& stop)
    {
        SessionState state{observer, stop};
        bool sdl = false, kernel = false, workers = false, watch = false;
        std::vector<std::shared_ptr<FsZip>> openZips;
        KProcessPtr versionProcess, prefixProcess, requestedProcess, shutdownProcess;
        try
        {
            state.Note(SessionPhase::Validation, "Validating the real app-owned Wine64 session inputs");
            ValidateContract(request);
            const auto nativeRoot = ValidateNativeFiles(request);
            Require(!sessionAttempted.exchange(true), "Only one owned kernel session is allowed per process");
            std::string error;
            Require(uwp::BeginGuestWorkerSession(error), error.c_str());
            workers = true;
            activeSession = &state;
            protonSetSdlOwnerThread(true);
            // Configure root-coordinate composition before X11 connections or
            // Wine windows exist. The legacy host keeps its single-app mode.
            protonConfigureXWireDesktop(request.width, request.height);
            Platform::init();
            Require(KNativeSystem::init(VIDEO_NORMAL, false), "SDL/WinRT video initialization failed");
            sdl = true;
            SDL_AddEventWatch(WatchLifecycle, &state); watch = true;
            StartUpArgs startup;
            startup.setRoot(BString::copy(request.rootPath.c_str()));
            for (const auto& zip : request.zipLayers) startup.addZip(BString::copy(zip.c_str()));
            startup.addArgs(GuestStrings(request.argv));
            startup.envValues = GuestStrings(request.environment);
            startup.setWorkingDir(B("/home/username"));
            startup.setResolution(BString::copy((std::to_string(request.width) + "x" + std::to_string(request.height)).c_str()));
            startup.setBpp(32); startup.setScale(100); startup.setVsync(VSYNC_ENABLED);
            startup.userId = startup.groupId = startup.effectiveUserId = startup.effectiveGroupId = 1000;
            startup.soundEnabled = false;
            startup.title = B("ProtonDesktop");
#ifdef _WIN32
            startup.logPath = BString::copy((request.rootPath + "\\protondesktop-wine-guest.log").c_str());
#else
            startup.logPath = BString::copy((request.rootPath + "/protondesktop-wine-guest.log").c_str());
#endif
            kernel = true; // prepare starts KSystem before mounting any ZIP.
            Require(startup.prepareProtonSession(openZips), "Pinned kernel/VFS/ZIP initialization failed");
            Require(KNativeSystem::getScreen() != nullptr, "The native screen was not created");
            KNativeSystem::showScreen(true);
            const auto environment = startup.envValues; // exact env after absent-key defaults
            state.Note(SessionPhase::WineVersion, "Starting real /usr/lib/wine/wine64 --version");
            versionProcess = Launch(state.result.version, {WineLoader, "--version"}, environment);
            auto phaseStarted = Clock::now();
            bool requestedStarted = false;
            while (!state.StopRequested())
            {
                PumpKernel();
                Reap();
                if (!state.result.version.exited)
                {
                    if (!ObserveExit(versionProcess, state.result.version))
                    {
                        Require(Clock::now() - phaseStarted < std::chrono::minutes(2), "Real Wine --version exceeded its startup deadline");
                        continue;
                    }
                    Require(state.result.version.exitCode == 0, "Real Wine --version returned a failure status");
                    state.Note(SessionPhase::WineVersion, "Real Wine --version exited with status 0");
                    state.result.prefixFilesPresent = PrefixFilesPresent(nativeRoot);
                    if (request.initializePrefixIfNeeded)
                    {
                        // Existing hives may come from an interrupted first
                        // boot. Always let Wine run its own initialization
                        // path; file presence cannot establish completion.
                        state.Note(SessionPhase::PrefixInitialization, "Starting real wineboot.exe --init to initialize/check the existing prefix in the same owned kernel");
                        prefixProcess = Launch(state.result.prefix, {WineLoader, "wineboot.exe", "--init"}, environment);
                        phaseStarted = Clock::now();
                    }
                    else Require(state.result.prefixFilesPresent, "A Wine prefix is missing and initialization is disabled");
                }
                if (prefixProcess && !state.result.prefix.exited)
                {
                    if (!ObserveExit(prefixProcess, state.result.prefix))
                    {
                        Require(Clock::now() - phaseStarted < std::chrono::minutes(10), "Real wineboot exceeded its initialization deadline");
                        continue;
                    }
                    Require(state.result.prefix.exitCode == 0, "Real wineboot --init returned a failure status");
                    state.result.prefixFilesPresent = PrefixFilesPresent(nativeRoot);
                    state.Note(SessionPhase::PrefixInitialization, state.result.prefixFilesPresent ?
                        "wineboot exited 0; live native registry files are present, not a prepared snapshot" :
                        "wineboot exited 0; persistent server has not published every native hive yet; continuing Explorer in this same live kernel");
                }
                if (!requestedStarted)
                {
                    state.Note(SessionPhase::ExplorerDesktop, "Starting requested Wine Explorer desktop in the same owned kernel");
                    requestedProcess = Launch(state.result.requested, request.argv, environment);
                    requestedStarted = true;
                }
                if (!state.result.requested.exited && ObserveExit(requestedProcess, state.result.requested))
                {
                    state.Note(SessionPhase::ExplorerDesktop, "Requested launcher exited with status " +
                        std::to_string(state.result.requested.exitCode) + "; keeping the genuine session and event pump alive");
                    Require(state.result.requested.exitCode == 0, "Requested Wine desktop launcher failed");
                }
                Require(!state.result.requested.exited || uwp::GetGuestWorkerSnapshot().running != 0,
                    "The Wine launcher exited and no guest worker remains; a live Wine desktop was not established");
            }
            state.result.stopped = true;
            // This is a scoped stop of OUR current kernel/prefix, never an init
            // step or a claim that '-w' flushes registry data.
            state.Note(SessionPhase::Stopping, "Stopping the owned Wine prefix with wineserver64 -k; waiting for real worker quiescence");
            if (state.result.version.exited && state.result.version.exitCode == 0 &&
                uwp::GetGuestWorkerSnapshot().running != 0)
            {
                shutdownProcess = Launch(state.result.shutdown, {WineServer, "-k"}, environment);
                const auto deadline = Clock::now() + std::chrono::seconds(30);
                while (Clock::now() < deadline)
                {
                    PumpKernel(); Reap();
                    ObserveExit(shutdownProcess, state.result.shutdown);
                    if (uwp::GetGuestWorkerSnapshot().running == 0) break;
                }
                state.result.prefixFilesPresent = PrefixFilesPresent(nativeRoot);
                state.result.cleanShutdown = state.result.shutdown.exited &&
                    state.result.shutdown.exitCode == 0 && uwp::GetGuestWorkerSnapshot().running == 0 &&
                    state.result.prefixFilesPresent;
            }
            else
            {
                state.result.prefixFilesPresent = PrefixFilesPresent(nativeRoot);
                state.result.cleanShutdown = uwp::GetGuestWorkerSnapshot().running == 0 &&
                    state.result.prefixFilesPresent;
            }
        }
        catch (const std::exception& error) { state.result.error = error.what(); }
        catch (...) { state.result.error = "The native Wine session raised a nonstandard exception"; }
        if (!state.result.error.empty())
        {
            state.result.cleanShutdown = false;
            try { state.Note(SessionPhase::Stopping, "Wine session error; retaining state until owned shutdown completes: " + state.result.error); }
            catch (...) {}
        }

        // Retain every ZIP, process and SDL object while controller requests
        // termination and waits for OS thread joins. The owner keeps pumping
        // guest UI work; controller never clears kernel/VFS state under it.
        if (workers)
        {
            uwp::CloseGuestWorkerAdmission();
            std::atomic<bool> controllerDone{false};
            std::string controllerError;
            std::thread controller;
            try
            {
                controller = std::thread([&]
                {
                    try
                    {
                        if (kernel) KSystem::protonStopGuestProcesses();
                        if (!uwp::JoinGuestWorkers(controllerError) && controllerError.empty())
                            controllerError = "The guest worker join barrier failed";
                    }
                    catch (const std::exception& error) { controllerError = error.what(); }
                    catch (...) { controllerError = "The stop controller raised a nonstandard exception"; }
                    controllerDone.store(true, std::memory_order_release);
                });
            }
            catch (const std::exception& error)
            {
                // A live kernel cannot be released without a stop controller.
                // Preserve it and keep the sole event pump responsive rather
                // than destroy referenced state or report completion.
                controllerError = std::string("Creating the stop controller failed: ") + error.what();
                state.result.error = controllerError;
                try { state.Note(SessionPhase::Stopping, controllerError + "; keeping the owner event pump alive without releasing live kernel state"); }
                catch (...) {}
                for (;;)
                {
                    try { if (sdl) PumpKernel(); else std::this_thread::yield(); }
                    catch (...) { std::this_thread::yield(); }
                }
            }
            const auto stopStarted = Clock::now();
            bool warnedAboutStop = false;
            while (!controllerDone.load(std::memory_order_acquire))
            {
                if (!warnedAboutStop && Clock::now() - stopStarted > std::chrono::seconds(30))
                {
                    warnedAboutStop = true;
                    state.result.cleanShutdown = false;
                    if (state.result.error.empty()) state.result.error = "Owned kernel shutdown exceeded 30 seconds; workers remain retained";
                    try { state.Note(SessionPhase::Stopping, state.result.error + "; event pumping continues, no completed teardown or prepared prefix is claimed"); }
                    catch (...) {}
                }
                try { if (sdl) PumpKernel(); else std::this_thread::yield(); }
                catch (const std::exception& error)
                { if (state.result.error.empty()) state.result.error = error.what(); }
                catch (...) { if (state.result.error.empty()) state.result.error = "SDL pumping failed during owned shutdown"; }
            }
            controller.join();
            const auto snapshot = uwp::GetGuestWorkerSnapshot();
            if (!controllerError.empty() && state.result.error.empty()) state.result.error = controllerError;
            if (snapshot.running != 0)
            {
                // No free/return path exists while a worker can still use this
                // stack, SDL, a ZIP handle, or the guest kernel.
                for (;;) { try { PumpKernel(); } catch (...) { std::this_thread::yield(); } }
            }
            std::string error;
            if (!uwp::EndGuestWorkerSession(error) && state.result.error.empty()) state.result.error = error;
        }
        // No controller or guest worker remains: kernel ticks have stopped and
        // final destruction now runs solely on the SDL owner.
        versionProcess.reset(); prefixProcess.reset(); requestedProcess.reset(); shutdownProcess.reset();
        if (kernel) { KThread::setCurrentThread(nullptr); KSystem::destroy(); }
        if (sdl) { g_xwirePresentSink = nullptr; KNativeSystem::shutdown(); KNativeAudio::shutdown(); dspShutdown(); }
        openZips.clear();
        if (watch) SDL_DelEventWatch(WatchLifecycle, &state);
        activeSession = nullptr;
        protonSetSdlOwnerThread(false);
        if (sdl) KNativeSystem::cleanup();
        try
        {
            state.Note(SessionPhase::Finished, state.result.error.empty() ?
                (state.result.cleanShutdown ? "Owned Wine session stopped; all guest workers joined" :
                "Owned Wine session stopped with forced kernel cleanup; prefix is not qualified as prepared") :
                "Wine session failed: " + state.result.error);
        }
        catch (...) { if (state.result.error.empty()) state.result.error = "Final session observer failed"; }
        return state.result;
    }
}

// Checked pinned XWire overlay calls this only for a real guest frame, after
// synthetic boot artwork has been excluded from this Wine runtime profile.
void protonWineGuestFrameSubmitted(unsigned width, unsigned height)
{ protondesktop::runtime::RecordGuestFrame(width, height); }
