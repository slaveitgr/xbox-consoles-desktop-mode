// Independent diagnostics companion to the DesktopMode shell; this is not a replacement shell.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include "runtime/RuntimeProbe.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::Graphics::Display;
using namespace Windows::Storage;
using namespace Windows::System;
using namespace Windows::System::Profile;
using namespace Windows::UI::Core;
using namespace Windows::UI::ViewManagement;

namespace {
using xcdm::runtime::ProbeResult;
using xcdm::runtime::ProbeStatus;

constexpr float CanvasWidth = 1920.0f;
constexpr float CanvasHeight = 1080.0f;
constexpr std::array<D2D1_RECT_F, 3> Buttons{{
    {124.0f, 540.0f, 904.0f, 606.0f},
    {988.0f, 540.0f, 1768.0f, 606.0f},
    {1496.0f, 770.0f, 1768.0f, 840.0f}
}};

D2D1_COLOR_F Color(unsigned rgb, float alpha = 1.0f) noexcept {
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 255) / 255.0f,
                        static_cast<float>((rgb >> 8) & 255) / 255.0f,
                        static_cast<float>(rgb & 255) / 255.0f, alpha);
}

bool Contains(D2D1_RECT_F const& rect, Point const& point) noexcept {
    return point.X >= rect.left && point.X <= rect.right &&
           point.Y >= rect.top && point.Y <= rect.bottom;
}

std::wstring Wide(std::string const& text) {
    auto value = to_hstring(text);
    return std::wstring(value.c_str(), value.size());
}

std::wstring UtcNow() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    if (gmtime_s(&utc, &now) != 0) return L"Time unavailable";
    wchar_t value[32]{};
    if (wcsftime(value, std::size(value), L"%Y-%m-%d %H:%M:%S UTC", &utc) == 0)
        return L"Time unavailable";
    return value;
}

std::wstring StatusName(ProbeStatus status) {
    switch (status) {
    case ProbeStatus::Passed: return L"Passed";
    case ProbeStatus::Failed: return L"Failed";
    case ProbeStatus::Unsupported: return L"Unsupported";
    default: return L"Not run";
    }
}

unsigned StatusColor(ProbeStatus status) noexcept {
    switch (status) {
    case ProbeStatus::Passed: return 0x8CEB67;
    case ProbeStatus::Failed: return 0xFF9292;
    case ProbeStatus::Unsupported: return 0xF1C875;
    default: return 0xAAB5AF;
    }
}

struct DiagnosticsApp : implements<DiagnosticsApp, IFrameworkView> {
    void Initialize(CoreApplicationView const& view) {
        m_activated = view.Activated([](CoreApplicationView const&, IActivatedEventArgs const&) {
            CoreWindow::GetForCurrentThread().Activate();
        });
        m_view = view;
        m_suspending = CoreApplication::Suspending([this](IInspectable const&, SuspendingEventArgs const&) {
            m_suspended = true;
            m_previousButtons = 0;
            if (m_d3d) {
                auto dxgi = m_d3d.try_as<IDXGIDevice3>();
                if (dxgi) dxgi->Trim();
            }
        });
        m_resuming = CoreApplication::Resuming([this](IInspectable const&, IInspectable const&) {
            m_suspended = false;
            m_previousButtons = 0;
            ValidateDevice();
        });
        try {
            const auto versionInfo = AnalyticsInfo::VersionInfo();
            m_deviceFamily = versionInfo.DeviceFamily().c_str();
            const auto packed = std::stoull(versionInfo.DeviceFamilyVersion().c_str());
            m_osVersion = std::to_wstring((packed >> 48) & 0xffff) + L"." +
                std::to_wstring((packed >> 32) & 0xffff) + L"." +
                std::to_wstring((packed >> 16) & 0xffff) + L"." +
                std::to_wstring(packed & 0xffff);
        } catch (...) {
            m_deviceFamily = L"UWP device";
        }
    }

    void SetWindow(CoreWindow const& window) {
        m_window = window;
        m_visible = window.Visible();
        m_window.Closed([this](CoreWindow const&, CoreWindowEventArgs const&) { m_closed = true; });
        m_window.VisibilityChanged([this](CoreWindow const&, VisibilityChangedEventArgs const& args) {
            m_visible = args.Visible();
            m_previousButtons = 0;
            if (m_visible) ValidateDevice();
        });
        m_window.SizeChanged([this](CoreWindow const&, WindowSizeChangedEventArgs const&) {
            ResizeSafely();
        });
        m_window.KeyDown([this](CoreWindow const&, KeyEventArgs const& args) {
            const auto key = args.VirtualKey();
            if (key == VirtualKey::Enter || key == VirtualKey::F5) {
                RunProbesAsync(-1);
            } else if (key == VirtualKey::S) {
                SaveReportAsync();
            } else if (key == VirtualKey::Space) {
                ActivateSelected();
            } else if (key == VirtualKey::Tab || key == VirtualKey::Right || key == VirtualKey::Down) {
                m_selected = (m_selected + 1) % 3;
            } else if (key == VirtualKey::Left || key == VirtualKey::Up) {
                m_selected = (m_selected + 2) % 3;
            } else if (key == VirtualKey::Escape) {
                SelectReport();
            } else {
                return;
            }
            args.Handled(true);
        });
        m_window.PointerMoved([this](CoreWindow const&, PointerEventArgs const& args) {
            m_pointer = LogicalPoint(args.CurrentPoint().Position());
        });
        m_window.PointerPressed([this](CoreWindow const&, PointerEventArgs const& args) {
            auto point = args.CurrentPoint();
            if (!point.Properties().IsLeftButtonPressed()) return;
            m_pointer = LogicalPoint(point.Position());
            for (int index = 0; index < 3; ++index) {
                if (Contains(Buttons[index], m_pointer)) {
                    m_selected = index;
                    ActivateSelected();
                    args.Handled(true);
                    break;
                }
            }
        });
        try {
            m_window.PointerCursor(CoreCursor(CoreCursorType::Arrow, 0));
        } catch (hresult_error const&) {
            // Controller-only hosts do not need a platform mouse cursor.
        }
        m_display = DisplayInformation::GetForCurrentView();
        m_dpiChanged = m_display.DpiChanged([this](DisplayInformation const&, IInspectable const&) {
            ResizeSafely();
        });
        try {
            ApplicationView::GetForCurrentView().TryEnterFullScreenMode();
        } catch (...) {
            // Windowed Windows hosts still use the same CoreWindow renderer.
        }
        RecreateGraphics();
    }

    void Load(hstring const&) {}

    void Run() {
        // A launch from Device Portal can collect results without remote input.
        RunProbesAsync(-1);
        while (!m_closed) {
            if (m_visible && !m_suspended) {
                m_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                if (m_closed || !m_visible || m_suspended) continue;
                PollGamepad();
                Render();
            } else {
                m_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
            }
        }
    }

    void Uninitialize() {
        if (m_view) m_view.Activated(m_activated);
        if (m_display) m_display.DpiChanged(m_dpiChanged);
        CoreApplication::Suspending(m_suspending);
        CoreApplication::Resuming(m_resuming);
        ReleaseGraphics();
    }

private:
    Point LogicalPoint(Point point) const noexcept {
        return Point{(point.X - m_offsetX) / m_scale, (point.Y - m_offsetY) / m_scale};
    }

    void ActivateSelected() {
        if (m_selected == 2) SaveReportAsync();
        else RunProbesAsync(m_selected);
    }

    void SelectReport() {
        m_selected = 2;
        if (!m_busy && !m_saving)
            m_actionStatus = L"Report selected. A runs both tests; X saves the current report.";
    }

    void PollGamepad() {
        try {
            const auto gamepads = Gamepad::Gamepads();
            if (gamepads.Size() == 0) {
                m_previousButtons = 0;
                m_controllerConnected = false;
                m_gamepad = nullptr;
                return;
            }
            const auto gamepad = gamepads.GetAt(0);
            if (!m_gamepad || gamepad != m_gamepad) {
                m_gamepad = gamepad;
                m_controllerConnected = false;
                m_previousButtons = 0;
            }
            const auto reading = gamepad.GetCurrentReading();
            const auto current = static_cast<std::uint64_t>(reading.Buttons);
            // A reconnect begins a fresh input state, avoiding held-button repeats.
            if (!m_controllerConnected) {
                m_previousButtons = current;
                m_controllerConnected = true;
                return;
            }
            const auto pressed = current & ~m_previousButtons;
            m_previousButtons = current;
            auto edge = [pressed](GamepadButtons button) {
                return (pressed & static_cast<std::uint64_t>(button)) != 0;
            };
            if (edge(GamepadButtons::A)) RunProbesAsync(-1);
            if (edge(GamepadButtons::X)) SaveReportAsync();
            if (edge(GamepadButtons::B)) SelectReport();
            if (edge(GamepadButtons::DPadRight) || edge(GamepadButtons::DPadDown))
                m_selected = (m_selected + 1) % 3;
            if (edge(GamepadButtons::DPadLeft) || edge(GamepadButtons::DPadUp))
                m_selected = (m_selected + 2) % 3;
        } catch (...) {
            m_previousButtons = 0;
            m_controllerConnected = false;
            m_gamepad = nullptr;
        }
    }

    fire_and_forget RunProbesAsync(int which) {
        if (m_busy || m_closed) co_return;
        if (m_saving) {
            m_actionStatus = L"Wait for the report to finish saving before running another check.";
            co_return;
        }
        auto lifetime = get_strong();
        m_busy = true;
        m_actionStatus = L"Running on this device. Waiting for the runtime checks...";
        const auto dispatcher = m_window.Dispatcher();
        ProbeResult memory{ProbeStatus::NotRun, {}, 0};
        ProbeResult pe{ProbeStatus::NotRun, {}, 0};
        bool failed = false;
        try {
            co_await resume_background();
            if (which == -1 || which == 0)
                memory = xcdm::runtime::RunExecutableMemoryProbe();
            if (which == -1 || which == 1)
                pe = xcdm::runtime::RunMinimalPeProbe();
        } catch (...) {
            failed = true;
        }
        try {
            co_await resume_foreground(dispatcher);
        } catch (...) {
            // A closed CoreWindow cannot accept a completion back on its dispatcher.
            OutputDebugStringW(L"Runtime diagnostics could not return to the UI dispatcher.\n");
            co_return;
        }
        m_busy = false;
        if (m_closed) co_return;
        if (failed) {
            m_actionStatus = L"The diagnostic could not complete. You can run the check again.";
            co_return;
        }
        if (which == -1 || which == 0) m_memoryResult = std::move(memory);
        if (which == -1 || which == 1) m_peResult = std::move(pe);
        m_testTime = UtcNow();
        m_actionStatus = L"Checks complete. Save the report to keep the exact results from this device.";
        if (which == -1) SaveReportAsync();
    }

    std::wstring ReportText() const {
        std::wostringstream report;
        report << L"Xbox Desktop Mode / Runtime diagnostics\n"
               << L"Independent companion to the DesktopMode shell\n"
               << L"Report created: " << UtcNow() << L"\n"
               << L"Device family: " << m_deviceFamily << L"\n"
               << L"OS version: " << m_osVersion << L"\n"
               << L"Application architecture: x64\n"
               << L"Last test completed: " << m_testTime << L"\n\n";
        auto append = [&report](wchar_t const* title, ProbeResult const& result) {
            report << title << L"\nStatus: " << StatusName(result.status)
                   << L"\nError code: " << result.errorCode
                   << L"\nMessage: " << (result.message.empty() ? L"No result yet." : Wide(result.message))
                   << L"\n\n";
        };
        append(L"Executable memory (write, then execute)", m_memoryResult);
        append(L"Minimal x64 Portable Executable", m_peResult);
        report << L"These are self-authored runtime diagnostics. They do not establish\n"
                  L"compatibility with Steam, arbitrary Win32 programs or commercial games.\n";
        return report.str();
    }

    fire_and_forget SaveReportAsync() {
        if (m_saving || m_closed) co_return;
        if (m_busy) {
            m_actionStatus = L"Wait for the running check to finish before saving its report.";
            co_return;
        }
        auto lifetime = get_strong();
        m_saving = true;
        m_actionStatus = L"Saving runtime-diagnostics.txt to this application's local storage...";
        try {
            const auto text = ReportText();
            const auto file = co_await ApplicationData::Current().LocalFolder().CreateFileAsync(
                L"runtime-diagnostics.txt", CreationCollisionOption::ReplaceExisting);
            co_await FileIO::WriteTextAsync(file, hstring(text));
            m_savedPath = file.Path().c_str();
            m_actionStatus = L"Saved runtime-diagnostics.txt. Retrieve it from this app's local storage in Device Portal.";
        } catch (hresult_error const& error) {
            m_actionStatus = L"Report could not be saved: ";
            m_actionStatus += error.message().c_str();
        } catch (...) {
            m_actionStatus = L"Report could not be saved. Try again while the application is active.";
        }
        m_saving = false;
    }

    void ReleaseGraphics() noexcept {
        if (m_context) m_context->SetTarget(nullptr);
        m_target = nullptr;
        m_brush = nullptr;
        m_small = nullptr;
        m_body = nullptr;
        m_heading = nullptr;
        m_title = nullptr;
        m_writeFactory = nullptr;
        m_context = nullptr;
        m_d2dDevice = nullptr;
        m_factory = nullptr;
        m_swapchain = nullptr;
        m_d3dContext = nullptr;
        m_d3d = nullptr;
    }

    void RecreateGraphics() {
        ReleaseGraphics();
        constexpr D3D_FEATURE_LEVEL levels[]{
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL selected{};
        auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION, m_d3d.put(), &selected, m_d3dContext.put());
        if (FAILED(result)) {
            check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION, m_d3d.put(), &selected, m_d3dContext.put()));
        }
        D2D1_FACTORY_OPTIONS options{};
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), &options, m_factory.put_void()));
        const auto dxgiDevice = m_d3d.as<IDXGIDevice>();
        check_hresult(m_factory->CreateDevice(dxgiDevice.get(), m_d2dDevice.put()));
        check_hresult(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_context.put()));
        check_hresult(m_context->CreateSolidColorBrush(Color(0xFFFFFF), m_brush.put()));
        check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<::IUnknown**>(m_writeFactory.put())));
        CreateFormat(22.0f, DWRITE_FONT_WEIGHT_NORMAL, m_small);
        CreateFormat(28.0f, DWRITE_FONT_WEIGHT_NORMAL, m_body);
        CreateFormat(34.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, m_heading);
        CreateFormat(62.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, m_title);
        Resize();
    }

    void CreateFormat(float size, DWRITE_FONT_WEIGHT weight, com_ptr<IDWriteTextFormat>& format) {
        check_hresult(m_writeFactory->CreateTextFormat(L"Segoe UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", format.put()));
        check_hresult(format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP));
    }

    void Resize() {
        if (!m_d3d || !m_context) return;
        const auto bounds = m_window.Bounds();
        m_dpi = m_display ? m_display.LogicalDpi() : 96.0f;
        if (m_dpi <= 0.0f) m_dpi = 96.0f;
        const float width = std::max(1.0f, bounds.Width);
        const float height = std::max(1.0f, bounds.Height);
        const auto pixelWidth = static_cast<UINT>(std::max(1.0f, std::round(width * m_dpi / 96.0f)));
        const auto pixelHeight = static_cast<UINT>(std::max(1.0f, std::round(height * m_dpi / 96.0f)));
        m_scale = std::max(0.01f, std::min(width / CanvasWidth, height / CanvasHeight));
        m_offsetX = (width - CanvasWidth * m_scale) * 0.5f;
        m_offsetY = (height - CanvasHeight * m_scale) * 0.5f;
        m_context->SetTarget(nullptr);
        m_target = nullptr;
        m_d3dContext->Flush();
        if (m_swapchain) {
            check_hresult(m_swapchain->ResizeBuffers(2, pixelWidth, pixelHeight, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
        } else {
            auto device = m_d3d.as<IDXGIDevice>();
            com_ptr<IDXGIAdapter> adapter;
            check_hresult(device->GetAdapter(adapter.put()));
            com_ptr<IDXGIFactory2> factory;
            check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = pixelWidth;
            desc.Height = pixelHeight;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            check_hresult(factory->CreateSwapChainForCoreWindow(m_d3d.get(),
                reinterpret_cast<::IUnknown*>(get_abi(m_window)), &desc, nullptr, m_swapchain.put()));
            check_hresult(device.as<IDXGIDevice1>()->SetMaximumFrameLatency(1));
        }
        com_ptr<IDXGISurface> surface;
        check_hresult(m_swapchain->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void()));
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), m_dpi, m_dpi);
        check_hresult(m_context->CreateBitmapFromDxgiSurface(surface.get(), &properties, m_target.put()));
        m_context->SetTarget(m_target.get());
        m_context->SetDpi(m_dpi, m_dpi);
        m_context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }

    void ResizeSafely() {
        try {
            Resize();
        } catch (hresult_error const&) {
            RecreateGraphics();
        }
    }

    void ValidateDevice() {
        if (m_d3d && FAILED(m_d3d->GetDeviceRemovedReason())) RecreateGraphics();
    }

    void Fill(D2D1_RECT_F rect, unsigned color, float radius = 0.0f) {
        m_brush->SetColor(Color(color));
        if (radius > 0.0f) m_context->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), m_brush.get());
        else m_context->FillRectangle(rect, m_brush.get());
    }

    void Border(D2D1_RECT_F rect, unsigned color, float thickness = 1.0f, float radius = 0.0f) {
        m_brush->SetColor(Color(color));
        if (radius > 0.0f) m_context->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), m_brush.get(), thickness);
        else m_context->DrawRectangle(rect, m_brush.get(), thickness);
    }

    void Text(std::wstring_view value, D2D1_RECT_F rect, IDWriteTextFormat* format, unsigned color) {
        m_brush->SetColor(Color(color));
        m_context->DrawText(value.data(), static_cast<UINT32>(value.size()), format, rect,
            m_brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void Button(int index, std::wstring const& label) {
        const bool disabled = m_busy || (index == 2 && m_saving);
        const bool hover = Contains(Buttons[index], m_pointer);
        Fill(Buttons[index], disabled ? 0x243128 : (hover ? 0x96F36F : 0x87DD61), 8.0f);
        if (m_selected == index) {
            auto focus = Buttons[index];
            focus.left -= 5.0f; focus.top -= 5.0f;
            focus.right += 5.0f; focus.bottom += 5.0f;
            Border(focus, 0xDEFBCF, 2.0f, 11.0f);
        }
        auto rect = Buttons[index];
        rect.left += 22.0f; rect.top += 15.0f;
        Text(label, rect, m_body.get(), disabled ? 0x8D9C92 : 0x13200E);
    }

    void ProbeCard(float left, wchar_t const* number, wchar_t const* title,
                   wchar_t const* description, ProbeResult const& result, int index) {
        const D2D1_RECT_F card{left, 256.0f, left + 836.0f, 634.0f};
        Fill(card, 0x151F19, 14.0f);
        Border(card, 0x2B3C30, 1.0f, 14.0f);
        Text(number, {left + 28, 283, left + 78, 324}, m_body.get(), 0x8CEB67);
        Text(title, {left + 84, 279, left + 808, 328}, m_heading.get(), 0xF0F5F1);
        Text(description, {left + 28, 337, left + 808, 378}, m_small.get(), 0xAAB9AF);
        Text(StatusName(result.status), {left + 28, 391, left + 808, 429}, m_body.get(), StatusColor(result.status));
        std::wstring message = result.message.empty() ? L"No result yet. Run this check on the current device." : Wide(result.message);
        if (result.errorCode != 0) message += L"  Error: " + std::to_wstring(result.errorCode);
        Text(message, {left + 28, 439, left + 808, 523}, m_small.get(), 0xD1DCD5);
        Button(index, m_busy ? L"Running checks..." : (index == 0 ? L"Run x64 memory test" : L"Run minimal PE test"));
    }

    void Render() {
        if (!m_context || !m_swapchain) return;
        m_context->BeginDraw();
        m_context->SetTransform(D2D1::Matrix3x2F::Identity());
        m_context->Clear(Color(0x090F0B));
        m_context->SetTransform(D2D1::Matrix3x2F::Scale(m_scale, m_scale) *
                                D2D1::Matrix3x2F::Translation(m_offsetX, m_offsetY));
        Fill({0, 0, CanvasWidth, CanvasHeight}, 0x0C140F);
        Fill({0, 0, 14, CanvasHeight}, 0x78CF52);
        Fill({96, 211, 1796, 213}, 0x2B3C30);
        Text(L"XBOX DESKTOP MODE", {96, 66, 1300, 107}, m_small.get(), 0x8CEB67);
        Text(L"Runtime diagnostics", {91, 111, 1500, 192}, m_title.get(), 0xF0F5F1);
        Text(m_deviceFamily + L"  /  x64", {1340, 78, 1796, 116}, m_small.get(), 0xAAB9AF);
        ProbeCard(96, L"01", L"Executable memory", L"Write a tiny x64 function, then switch its memory to executable.", m_memoryResult, 0);
        ProbeCard(960, L"02", L"Minimal PE loader", L"Map and run a self-authored x64 Portable Executable fixture.", m_peResult, 1);
        Fill({96, 666, 1796, 922}, 0x121C16, 14.0f);
        Border({96, 666, 1796, 922}, 0x2B3C30, 1.0f, 14.0f);
        Text(L"Device report", {124, 691, 1400, 744}, m_heading.get(), 0xF0F5F1);
        Text(m_actionStatus, {124, 751, 1445, 816}, m_small.get(), 0xD1DCD5);
        Text(L"Last check: " + m_testTime, {124, 824, 1445, 861}, m_small.get(), 0x9FB0A4);
        if (!m_savedPath.empty()) {
            Text(L"Saved: " + m_savedPath, {124, 861, 1768, 906}, m_small.get(), 0x9FB0A4);
        }
        Button(2, m_saving ? L"Saving..." : L"Save report");
        Text(L"A  Run both     X  Save report     B  Select report", {96, 959, 1796, 998}, m_body.get(), 0xD9E5DD);
        Text(L"Keyboard: Enter / F5 run  |  S save  |  Tab / arrows focus  |  Space activate  |  Esc select report",
             {96, 1006, 1796, 1043}, m_small.get(), 0x9FB0A4);
        const auto drawn = m_context->EndDraw();
        if (drawn == D2DERR_RECREATE_TARGET || drawn == DXGI_ERROR_DEVICE_REMOVED || drawn == DXGI_ERROR_DEVICE_RESET) {
            RecreateGraphics();
            return;
        }
        check_hresult(drawn);
        const auto presented = m_swapchain->Present(1, 0);
        if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) {
            RecreateGraphics();
            return;
        }
        check_hresult(presented);
    }

    CoreApplicationView m_view{nullptr};
    CoreWindow m_window{nullptr};
    DisplayInformation m_display{nullptr};
    Gamepad m_gamepad{nullptr};
    event_token m_activated{}, m_suspending{}, m_resuming{}, m_dpiChanged{};
    bool m_closed{false}, m_visible{true}, m_suspended{false}, m_busy{false}, m_saving{false};
    bool m_controllerConnected{false};
    int m_selected{0};
    std::uint64_t m_previousButtons{0};
    Point m_pointer{-100.0f, -100.0f};
    float m_scale{1.0f}, m_offsetX{0}, m_offsetY{0}, m_dpi{96};
    std::wstring m_deviceFamily{L"UWP device"};
    std::wstring m_osVersion{L"Unavailable"};
    std::wstring m_testTime{L"Not run"};
    std::wstring m_actionStatus{L"Ready. Run the two checks to measure this device's runtime capabilities."};
    std::wstring m_savedPath;
    ProbeResult m_memoryResult{ProbeStatus::NotRun, {}, 0};
    ProbeResult m_peResult{ProbeStatus::NotRun, {}, 0};
    com_ptr<ID3D11Device> m_d3d;
    com_ptr<ID3D11DeviceContext> m_d3dContext;
    com_ptr<IDXGISwapChain1> m_swapchain;
    com_ptr<ID2D1Factory1> m_factory;
    com_ptr<ID2D1Device> m_d2dDevice;
    com_ptr<ID2D1DeviceContext> m_context;
    com_ptr<ID2D1Bitmap1> m_target;
    com_ptr<ID2D1SolidColorBrush> m_brush;
    com_ptr<IDWriteFactory> m_writeFactory;
    com_ptr<IDWriteTextFormat> m_small, m_body, m_heading, m_title;
};

struct AppSource : implements<AppSource, IFrameworkViewSource> {
    IFrameworkView CreateView() { return make<DiagnosticsApp>(); }
};
} // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        init_apartment(apartment_type::multi_threaded);
        CoreApplication::Run(make<AppSource>());
        return 0;
    } catch (hresult_error const& error) {
        OutputDebugStringW(error.message().c_str());
        return static_cast<int>(error.code().value);
    } catch (...) {
        OutputDebugStringW(L"Runtime diagnostics failed to initialize.\n");
        return 1;
    }
}
