// SPDX-License-Identifier: GPL-2.0-only
// Native UWP host. Its status screen is not a substitute Wine desktop.
#include "backend/NativeBackend.h"
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Foundation.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <mutex>
#if defined(PROTON_CORE_PROBE_ENABLED)
#include "CoreDiagnostics.h"
#include <winrt/Windows.Storage.h>
#include <thread>
#if !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) || WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#error The core probe requires the genuine UWP APP SDK partition.
#endif
#endif

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Graphics::Display;
using namespace Windows::UI::Core;

namespace
{
#if defined(PROTON_CORE_PROBE_ENABLED)
    struct CoreProbeState
    {
        std::mutex lock;
        bool finished = false;
        protondesktop::CoreDiagnosticReport report;
        std::string persistenceError;
    };

    std::wstring ProbeErrorText(const std::string& error)
    {
        if (error.empty()) return {};
        const auto size = static_cast<int>(std::min<std::size_t>(error.size(), 2048));
        const auto length = MultiByteToWideChar(CP_UTF8, 0, error.data(), size, nullptr, 0);
        if (!length) return L"Diagnostic error details could not be decoded.";
        std::wstring result(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, error.data(), size, result.data(), length);
        return result;
    }

    // The closure owns these values independently of App. There are no raw App
    // pointers, UI callbacks or detached threads, including on failure paths.
    void RunAppCoreProbe(const std::shared_ptr<CoreProbeState>& state,
                         const protondesktop::CoreDiagnosticPaths& paths,
                         const std::wstring& resultPath) noexcept
    {
        protondesktop::CoreDiagnosticReport report;
        std::string persistenceError;
        try
        {
            if (!protondesktop::WriteCoreDiagnosticReport(resultPath, report, true, persistenceError))
            {
                report.status = protondesktop::CoreDiagnosticStatus::Failed;
                report.error = "The running report could not be saved; core diagnostics were not started";
            }
            else
            {
                report = protondesktop::RunCoreDiagnostics(paths);
                if (report.status == protondesktop::CoreDiagnosticStatus::Incomplete)
                    report.status = protondesktop::CoreDiagnosticStatus::Failed;
                protondesktop::WriteCoreDiagnosticReport(resultPath, report, false, persistenceError);
            }
        }
        catch (const std::exception& error)
        {
            report.status = protondesktop::CoreDiagnosticStatus::Failed;
            report.error = std::string("Core probe worker exception: ") + error.what();
            try { protondesktop::WriteCoreDiagnosticReport(resultPath, report, false, persistenceError); }
            catch (...) { persistenceError = "Saving the failed report also raised an exception"; }
        }
        catch (...)
        {
            report.status = protondesktop::CoreDiagnosticStatus::Failed;
            report.error = "Core probe worker raised a nonstandard C++ exception";
            try { protondesktop::WriteCoreDiagnosticReport(resultPath, report, false, persistenceError); }
            catch (...) { persistenceError = "Saving the failed report also raised an exception"; }
        }
        std::lock_guard<std::mutex> guard(state->lock);
        state->report = std::move(report);
        state->persistenceError = std::move(persistenceError);
        state->finished = true;
    }
#endif
    bool DeviceWasLost(HRESULT result) noexcept
    {
        return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET;
    }

    struct App : implements<App, IFrameworkView>
    {
        CoreWindow window{nullptr};
        DisplayInformation display{nullptr};
        bool closed = false, visible = true, resizePending = true;
        std::atomic<bool> validateDevicePending{false};
        // Suspending can arrive off the window thread. Only GPU trimming runs
        // there; all creation, resizing and presentation stay on the window thread.
        std::mutex graphicsLock;
        CoreApplication::Suspending_revoker suspending;
        CoreApplication::Resuming_revoker resuming;
        CoreWindow::Closed_revoker windowClosed;
        CoreWindow::VisibilityChanged_revoker visibilityChanged;
        CoreWindow::SizeChanged_revoker sizeChanged;
        DisplayInformation::DpiChanged_revoker dpiChanged;
        com_ptr<ID3D11Device> device;
        com_ptr<ID3D11DeviceContext> context;
        com_ptr<IDXGISwapChain1> swapChain;
        com_ptr<ID2D1Factory> d2d;
        com_ptr<ID2D1RenderTarget> target;
        com_ptr<ID2D1SolidColorBrush> ink;
        com_ptr<IDWriteFactory> write;
        com_ptr<IDWriteTextFormat> title, body;
        std::unique_ptr<protondesktop::NativeBackend> backend;
#if defined(PROTON_CORE_PROBE_ENABLED)
        std::shared_ptr<CoreProbeState> coreProbeState;
        std::thread coreProbeWorker;
#endif

        ~App() { StopBackend(); JoinCoreProbe(); }

        void Initialize(const CoreApplicationView& view)
        {
            const auto weak = get_weak();
            view.Activated([weak](auto&&, auto&&)
            {
                if (auto self = weak.get()) self->window.Activate();
            });
            suspending = CoreApplication::Suspending(auto_revoke, [weak](auto&&, auto&&)
            {
                if (auto self = weak.get()) self->TrimDevice();
            });
            resuming = CoreApplication::Resuming(auto_revoke, [weak](auto&&, auto&&)
            {
                if (auto self = weak.get()) self->validateDevicePending.store(true);
            });
        }
        void SetWindow(const CoreWindow& value)
        {
            window = value;
            const auto weak = get_weak();
            windowClosed = window.Closed(auto_revoke, [weak](auto&&, auto&&)
            {
                if (auto self = weak.get()) self->closed = true;
            });
            visibilityChanged = window.VisibilityChanged(auto_revoke,
                [weak](auto&&, const VisibilityChangedEventArgs& args)
            {
                if (auto self = weak.get())
                {
                    self->visible = args.Visible();
                    if (self->visible) self->validateDevicePending.store(true);
                }
            });
            sizeChanged = window.SizeChanged(auto_revoke, [weak](auto&&, auto&&)
            {
                if (auto self = weak.get()) self->resizePending = true;
            });
            display = DisplayInformation::GetForCurrentView();
            dpiChanged = display.DpiChanged(auto_revoke, [weak](auto&&, auto&&)
            {
                if (auto self = weak.get()) self->resizePending = true;
            });
            check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.put()));
            check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write.put())));
            check_hresult(write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 32, L"en-US", title.put()));
            check_hresult(write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18, L"en-US", body.put()));
            std::lock_guard<std::mutex> guard(graphicsLock);
            CreateDeviceResources();
        }
        UINT PixelDimension(float dip) const
        {
            const float dpi = display ? display.LogicalDpi() : 96.0f;
            return static_cast<UINT>(std::lround(std::clamp(dip * dpi / 96.0f, 1.0f, 16384.0f)));
        }
        void CreateDeviceResources()
        {
            // Called under graphicsLock on the window thread, including after
            // DXGI device removal. The old swap chain must release its window.
            ink = nullptr;
            target = nullptr;
            if (context) { context->ClearState(); context->Flush(); }
            swapChain = nullptr;
            context = nullptr;
            device = nullptr;
            check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                device.put(), nullptr, context.put()));
            auto dxgi = device.as<IDXGIDevice>();
            com_ptr<IDXGIAdapter> adapter;
            check_hresult(dxgi->GetAdapter(adapter.put()));
            com_ptr<IDXGIFactory2> factory;
            check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
            const auto bounds = window.Bounds();
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = PixelDimension(bounds.Width);
            desc.Height = PixelDimension(bounds.Height);
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            check_hresult(factory->CreateSwapChainForCoreWindow(device.get(), winrt::get_unknown(window),
                &desc, nullptr, swapChain.put()));
            CreateRenderTarget();
            resizePending = false;
        }
        void CreateRenderTarget()
        {
            ink = nullptr;
            target = nullptr;
            com_ptr<IDXGISurface> surface;
            check_hresult(swapChain->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void()));
            const float dpi = display ? display.LogicalDpi() : 96.0f;
            const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
            check_hresult(d2d->CreateDxgiSurfaceRenderTarget(surface.get(), &properties, target.put()));
            check_hresult(target->CreateSolidColorBrush(D2D1::ColorF(0.91f, 0.95f, 1.0f), ink.put()));
        }
        void Resize()
        {
            ink = nullptr;
            target = nullptr;
            context->ClearState();
            const auto bounds = window.Bounds();
            const HRESULT result = swapChain->ResizeBuffers(2, PixelDimension(bounds.Width),
                PixelDimension(bounds.Height), DXGI_FORMAT_B8G8R8A8_UNORM, 0);
            if (DeviceWasLost(result)) { CreateDeviceResources(); return; }
            check_hresult(result);
            CreateRenderTarget();
            resizePending = false;
        }
        void TrimDevice() noexcept
        {
            try
            {
                std::lock_guard<std::mutex> guard(graphicsLock);
                if (!device) return;
                if (auto dxgi = device.try_as<IDXGIDevice3>()) dxgi->Trim();
            }
            catch (...) { OutputDebugStringW(L"ProtonDesktop GPU trim failed.\n"); }
        }
        void Load(hstring const&)
        {
            // A real adapter must validate rootfs provenance and mount app-local
            // writable storage before Start. No adapter means no guest launch.
            backend = protondesktop::CreateNativeBackend();
#if defined(PROTON_CORE_PROBE_ENABLED)
            StartCoreProbe();
#endif
        }

#if defined(PROTON_CORE_PROBE_ENABLED)
        void StartCoreProbe()
        {
            if (coreProbeState) return;
            coreProbeState = std::make_shared<CoreProbeState>();
            try
            {
                // Resolve WinRT storage while on the owning app thread. Only
                // plain immutable paths and shared result state cross threads.
                const auto installed = Windows::ApplicationModel::Package::Current().InstalledLocation().Path();
                const auto local = Windows::Storage::ApplicationData::Current().LocalFolder().Path();
                protondesktop::CoreDiagnosticPaths paths;
                paths.localStateDirectory.assign(local.c_str(), local.size());
                const std::wstring packageRoot(installed.c_str(), installed.size());
                paths.positiveElfPath = packageRoot + L"\\DiagnosticPayloads\\guest64-smoke.elf";
                paths.negativeElfPath = packageRoot + L"\\DiagnosticPayloads\\guest64-negative.elf";
                const auto resultPath = paths.localStateDirectory + L"\\protondesktop-core-result.json";
                coreProbeWorker = std::thread([state = coreProbeState, paths = std::move(paths), resultPath]
                {
                    RunAppCoreProbe(state, paths, resultPath);
                });
            }
            catch (const hresult_error& error)
            {
                std::lock_guard<std::mutex> guard(coreProbeState->lock);
                coreProbeState->report.status = protondesktop::CoreDiagnosticStatus::Failed;
                coreProbeState->report.error = "Resolving package/app-local paths failed: " + to_string(error.message());
                coreProbeState->finished = true;
            }
            catch (const std::exception& error)
            {
                std::lock_guard<std::mutex> guard(coreProbeState->lock);
                coreProbeState->report.status = protondesktop::CoreDiagnosticStatus::Failed;
                coreProbeState->report.error = std::string("Starting the core probe worker failed: ") + error.what();
                coreProbeState->finished = true;
            }
        }
        std::wstring CoreProbeStatus()
        {
            std::wstring text = L"UWP core diagnostics - opt-in probe\n\n";
            if (!coreProbeState) return text + L"Diagnostic worker has not started.";
            std::lock_guard<std::mutex> guard(coreProbeState->lock);
            if (!coreProbeState->finished)
                return text + L"Running the real CPU64 self-tests and two packaged ELF controls.\n"
                    L"Logs and result JSON are saved in LocalState.\n\n"
                    L"No Wine desktop has started.\nClosing waits for the worker; cancellation is not available.";
            const auto& report = coreProbeState->report;
            text += report.status == protondesktop::CoreDiagnosticStatus::Passed ?
                L"All three core diagnostics passed.\n\n" : L"Core diagnostics failed or did not complete.\n\n";
            if (report.selfTest.summaryParsed && report.selfTest.summaryOccurrences == 1)
                text += L"CPU64 self-test: " + std::to_wstring(report.selfTest.passed) +
                    L" passed, " + std::to_wstring(report.selfTest.failed) + L" failed\n";
            if (report.positive.exitParsed && report.positive.stopParsed)
                text += L"Positive ELF: guest exit " + std::to_wstring(report.positive.guestExit) +
                    L", " + std::to_wstring(report.positive.instructions) + L" instructions\n";
            if (report.negative.exitParsed && report.negative.stopParsed)
                text += L"Negative ELF: guest exit " + std::to_wstring(report.negative.guestExit) +
                    L", " + std::to_wstring(report.negative.instructions) + L" instructions\n";
            if (!report.error.empty()) text += L"\n" + ProbeErrorText(report.error) + L"\n";
            if (!coreProbeState->persistenceError.empty())
                text += L"\nReport storage error: " + ProbeErrorText(coreProbeState->persistenceError) + L"\n";
            text += L"\nLocalState: protondesktop-core-result.json and three phase logs\n"
                L"Wine/rootfs: not started. Desktop backend remains unavailable.\n"
                L"This result does not infer Xbox or Wine compatibility.";
            return text;
        }
#endif
        void JoinCoreProbe() noexcept
        {
#if defined(PROTON_CORE_PROBE_ENABLED)
            // The core exposes no cancellation operation. Joining retains the
            // app/core state until the synchronous worker has actually stopped.
            if (coreProbeWorker.joinable()) coreProbeWorker.join();
#endif
        }
        void Draw()
        {
            std::lock_guard<std::mutex> guard(graphicsLock);
            if (validateDevicePending.exchange(false) && FAILED(device->GetDeviceRemovedReason()))
                CreateDeviceResources();
            if (resizePending) Resize();
            target->BeginDraw();
            target->Clear(D2D1::ColorF(0.025f, 0.055f, 0.12f));
            const auto size = target->GetSize();
            const auto titleRect = D2D1::RectF(64, 58, size.width - 64, 115);
            target->DrawText(L"ProtonDesktop", 13, title.get(), titleRect, ink.get());
#if defined(PROTON_CORE_PROBE_ENABLED)
            const auto status = CoreProbeStatus();
#else
            const std::wstring status = L"Native UWP port starter\n\nBoxedwine64 UWP backend is not linked.\nNo Wine desktop has started.\n\nNext: interpreter + kernel port, real X11 pixels, validated Wine64 rootfs.";
#endif
            target->DrawText(status.c_str(), static_cast<UINT32>(status.size()), body.get(),
                D2D1::RectF(66, 140, size.width - 64, size.height - 64), ink.get());
            const HRESULT result = target->EndDraw();
            if (result == D2DERR_RECREATE_TARGET)
            {
                if (FAILED(device->GetDeviceRemovedReason())) CreateDeviceResources();
                else CreateRenderTarget();
                return;
            }
            if (DeviceWasLost(result)) { CreateDeviceResources(); return; }
            check_hresult(result);
            const HRESULT presented = swapChain->Present(1, 0);
            if (DeviceWasLost(presented)) { CreateDeviceResources(); return; }
            check_hresult(presented);
        }
        void StopBackend() noexcept
        {
            if (!backend) return;
            backend->RequestStop();
            backend->Join();
            backend.reset();
        }
        void Run()
        {
            while (!closed)
            {
                if (visible)
                {
                    window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                    if (!closed && visible) Draw();
                }
                else window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
            }
            StopBackend();
            JoinCoreProbe();
        }
        void Uninitialize()
        {
            suspending.revoke();
            resuming.revoke();
            dpiChanged.revoke();
            sizeChanged.revoke();
            visibilityChanged.revoke();
            windowClosed.revoke();
            StopBackend();
            JoinCoreProbe();
        }
    };
    struct AppSource : implements<AppSource, IFrameworkViewSource>
    {
        IFrameworkView CreateView() { return make<App>(); }
    };
}
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        init_apartment(apartment_type::multi_threaded);
        CoreApplication::Run(make<AppSource>());
        return 0;
    }
    catch (const hresult_error& error)
    {
        OutputDebugStringW(L"ProtonDesktop startup failed: ");
        OutputDebugStringW(error.message().c_str());
        return static_cast<int>(error.code().value);
    }
    catch (...)
    {
        OutputDebugStringW(L"ProtonDesktop startup failed with an unexpected exception.\n");
        return static_cast<int>(E_FAIL);
    }
}
