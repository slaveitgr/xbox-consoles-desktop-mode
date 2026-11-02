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

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Graphics::Display;
using namespace Windows::UI::Core;

namespace
{
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

        ~App() { StopBackend(); }

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
            check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), write.put_void()));
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
            check_hresult(factory->CreateSwapChainForCoreWindow(device.get(), get_unknown(window),
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
            const wchar_t* status = L"Native UWP port starter\n\nBoxedwine64 UWP backend is not linked.\nNo Wine desktop has started.\n\nNext: interpreter + kernel port, real X11 pixels, validated Wine64 rootfs.";
            target->DrawText(status, static_cast<UINT32>(wcslen(status)), body.get(),
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
