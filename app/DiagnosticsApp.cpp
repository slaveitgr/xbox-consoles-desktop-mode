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
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include "runtime/RuntimeProbe.h"
#include "runtime/GamePreflight.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <exception>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
constexpr D2D1_RECT_F PageButton{1476.0f, 134.0f, 1796.0f, 194.0f};
constexpr std::array<D2D1_RECT_F, 3> GameButtons{{
    {96.0f, 755.0f, 610.0f, 817.0f},
    {638.0f, 755.0f, 1138.0f, 817.0f},
    {1412.0f, 755.0f, 1796.0f, 817.0f}
}};
constexpr wchar_t GameFolderToken[] = L"GamePreflightFolder";
constexpr std::uint64_t MaxGameImageBytes = 64ULL * 1024 * 1024;
constexpr std::uint32_t MaxGameFolderEntries = 256;
enum class Page { Game, Runtime, Controllers };
constexpr std::array<D2D1_RECT_F, 4> ControllerButtons{{
    {96.0f, 755.0f, 400.0f, 817.0f},
    {420.0f, 755.0f, 724.0f, 817.0f},
    {744.0f, 755.0f, 1096.0f, 817.0f},
    {1440.0f, 755.0f, 1796.0f, 817.0f}
}};
constexpr std::size_t MaxControllerEntries = 64;
constexpr std::uint32_t MaxRawButtons = 1024, MaxRawAxes = 256, MaxRawSwitches = 256;
constexpr std::size_t VisibleControllerRows = 6;

struct ControllerEntry {
    RawGameController raw{nullptr};
    Gamepad gamepad{nullptr};
    std::wstring name{L"Gamepad (raw details unavailable)"};
    std::wstring details;
    std::wstring metadataError;
    std::int32_t buttonCount{0}, axisCount{0}, switchCount{0};
    std::uint16_t vendor{0}, product{0};
    bool hasMetadata{false}, wireless{false};
};

struct ControllerEventState {
    // Static WGI events may arrive on a worker thread or outlive unsubscription.
    // Handlers capture only this shared state, never a DiagnosticsApp pointer.
    std::atomic<bool> active{true};
    std::atomic<bool> dirty{true};
    std::array<std::atomic<std::uint64_t>, 4> counts{};

    ControllerEventState() noexcept {
        for (auto& count : counts) count.store(0, std::memory_order_relaxed);
    }
};

D2D1_COLOR_F Color(unsigned rgb, float alpha = 1.0f) noexcept {
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 255) / 255.0f,
                        static_cast<float>((rgb >> 8) & 255) / 255.0f,
                        static_cast<float>(rgb & 255) / 255.0f, alpha);
}

bool Contains(D2D1_RECT_F const& rect, Point const& point) noexcept {
    return point.X >= rect.left && point.X <= rect.right &&
           point.Y >= rect.top && point.Y <= rect.bottom;
}

std::wstring Hex(std::uint64_t value, int width = 0) {
    std::wostringstream text;
    text << L"0x" << std::uppercase << std::hex << std::setfill(L'0') << std::setw(width) << value;
    return text.str();
}

std::wstring ApiError(wchar_t const* operation, hresult_error const& error) {
    return std::wstring(operation) + L": " + Hex(static_cast<std::uint32_t>(error.code().value), 8);
}

std::wstring ModelName(hstring const& name) {
    // Device-supplied labels must not introduce lines into the diagnostic report.
    std::wstring result;
    for (const auto character : name) {
        if (result.size() >= 100) break;
        result += character < 32 || character == 127 ? L' ' : character;
    }
    return result.empty() ? L"Unnamed controller" : result;
}

std::wstring GamepadButtonNames(GamepadButtons buttons) {
    constexpr std::array<std::pair<GamepadButtons, wchar_t const*>, 18> names{{
        {GamepadButtons::A, L"A"}, {GamepadButtons::B, L"B"},
        {GamepadButtons::X, L"X"}, {GamepadButtons::Y, L"Y"},
        {GamepadButtons::DPadUp, L"Up"}, {GamepadButtons::DPadDown, L"Down"},
        {GamepadButtons::DPadLeft, L"Left"}, {GamepadButtons::DPadRight, L"Right"},
        {GamepadButtons::LeftShoulder, L"LB"}, {GamepadButtons::RightShoulder, L"RB"},
        {GamepadButtons::LeftThumbstick, L"LS"}, {GamepadButtons::RightThumbstick, L"RS"},
        {GamepadButtons::Menu, L"Menu"}, {GamepadButtons::View, L"View"},
        {GamepadButtons::Paddle1, L"P1"}, {GamepadButtons::Paddle2, L"P2"},
        {GamepadButtons::Paddle3, L"P3"}, {GamepadButtons::Paddle4, L"P4"}
    }};
    std::wstring result;
    const auto value = static_cast<std::uint64_t>(buttons);
    for (auto const& item : names) {
        if ((value & static_cast<std::uint64_t>(item.first)) == 0) continue;
        if (!result.empty()) result += L" ";
        result += item.second;
    }
    return result.empty() ? L"none" : result;
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
        // Xbox input discovery can depend on an Added listener before enumeration.
        // Keep listeners alive for the complete framework-view lifetime.
        InitializeControllerEvents();
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
            if (key == VirtualKey::C) {
                TogglePage();
            } else if (m_page == Page::Game) {
                if (key == VirtualKey::O) InspectGameFolderAsync(true);
                else if (key == VirtualKey::R || key == VirtualKey::F5) InspectGameFolderAsync(false);
                else if (key == VirtualKey::S) SaveGameReportAsync();
                else if (key == VirtualKey::Enter || key == VirtualKey::Space) ActivateGameSelection();
                else if (key == VirtualKey::Tab || key == VirtualKey::Right || key == VirtualKey::Down)
                    m_gameSelection = (m_gameSelection + 1) % 3;
                else if (key == VirtualKey::Left || key == VirtualKey::Up)
                    m_gameSelection = (m_gameSelection + 2) % 3;
                else if (key == VirtualKey::Escape) TogglePage();
                else return;
            } else if (m_page == Page::Controllers) {
                if (key == VirtualKey::S) SaveReportAsync();
                else if (key == VirtualKey::R || key == VirtualKey::F5) RefreshControllers(true);
                else if (key == VirtualKey::Tab || key == VirtualKey::Right || key == VirtualKey::Down)
                    SelectController(1);
                else if (key == VirtualKey::Left || key == VirtualKey::Up) SelectController(-1);
                else if (key == VirtualKey::Escape) TogglePage();
                else return;
            } else if (key == VirtualKey::Enter || key == VirtualKey::F5) {
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
            if (Contains(PageButton, m_pointer)) {
                TogglePage();
                args.Handled(true);
                return;
            }
            if (m_page == Page::Game) {
                for (std::size_t index = 0; index < GameButtons.size(); ++index) {
                    if (!Contains(GameButtons[index], m_pointer)) continue;
                    m_gameSelection = static_cast<int>(index);
                    ActivateGameSelection();
                    args.Handled(true);
                    break;
                }
                return;
            }
            if (m_page == Page::Controllers) {
                for (std::size_t index = 0; index < ControllerButtons.size(); ++index) {
                    if (!Contains(ControllerButtons[index], m_pointer)) continue;
                    if (index == 0) SelectController(-1);
                    else if (index == 1) SelectController(1);
                    else if (index == 2) RefreshControllers(true);
                    else SaveReportAsync();
                    args.Handled(true);
                    return;
                }
                const auto first = (m_controllerIndex / VisibleControllerRows) * VisibleControllerRows;
                for (std::size_t row = 0; row < VisibleControllerRows; ++row) {
                    const auto top = 328.0f + static_cast<float>(row) * 60.0f;
                    if (first + row < m_controllers.size() &&
                        Contains({112.0f, top, 732.0f, top + 56.0f}, m_pointer)) {
                        m_controllerIndex = first + row;
                        ResetControllerReading();
                        ReadSelectedController();
                        args.Handled(true);
                        break;
                    }
                }
                return;
            }
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
        RefreshControllers(true);
        ReadSelectedController();
        RunProbesAsync(-1);
        while (!m_closed) {
            if (m_visible && !m_suspended) {
                m_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                if (m_closed || !m_visible || m_suspended) continue;
                RefreshControllers(false);
                PollGamepad();
                if (m_page == Page::Controllers) ReadSelectedController();
                Render();
            } else {
                m_window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
            }
        }
    }

    void Uninitialize() {
        UninitializeControllerEvents();
        if (m_view) m_view.Activated(m_activated);
        if (m_display) m_display.DpiChanged(m_dpiChanged);
        CoreApplication::Suspending(m_suspending);
        CoreApplication::Resuming(m_resuming);
        ReleaseGraphics();
    }

private:
    void InitializeControllerEvents() {
        const auto state = m_controllerEvents;
        auto notify = [state](std::size_t index) noexcept {
            if (!state->active.load(std::memory_order_relaxed)) return;
            state->counts[index].fetch_add(1, std::memory_order_relaxed);
            state->dirty.store(true, std::memory_order_release);
        };
        try {
            m_controllerEventTokens[0] = Gamepad::GamepadAdded(
                [notify](IInspectable const&, Gamepad const&) { notify(0); });
            m_controllerEventRegistered[0] = true;
            m_controllerEventStatus[0] = L"Registered";
        } catch (hresult_error const& error) { m_controllerEventStatus[0] = ApiError(L"Subscription failed", error); }
        catch (...) { m_controllerEventStatus[0] = L"Subscription failed (unknown error)"; }
        try {
            m_controllerEventTokens[1] = Gamepad::GamepadRemoved(
                [notify](IInspectable const&, Gamepad const&) { notify(1); });
            m_controllerEventRegistered[1] = true;
            m_controllerEventStatus[1] = L"Registered";
        } catch (hresult_error const& error) { m_controllerEventStatus[1] = ApiError(L"Subscription failed", error); }
        catch (...) { m_controllerEventStatus[1] = L"Subscription failed (unknown error)"; }
        try {
            m_controllerEventTokens[2] = RawGameController::RawGameControllerAdded(
                [notify](IInspectable const&, RawGameController const&) { notify(2); });
            m_controllerEventRegistered[2] = true;
            m_controllerEventStatus[2] = L"Registered";
        } catch (hresult_error const& error) { m_controllerEventStatus[2] = ApiError(L"Subscription failed", error); }
        catch (...) { m_controllerEventStatus[2] = L"Subscription failed (unknown error)"; }
        try {
            m_controllerEventTokens[3] = RawGameController::RawGameControllerRemoved(
                [notify](IInspectable const&, RawGameController const&) { notify(3); });
            m_controllerEventRegistered[3] = true;
            m_controllerEventStatus[3] = L"Registered";
        } catch (hresult_error const& error) { m_controllerEventStatus[3] = ApiError(L"Subscription failed", error); }
        catch (...) { m_controllerEventStatus[3] = L"Subscription failed (unknown error)"; }
    }

    void UninitializeControllerEvents() noexcept {
        m_controllerEvents->active.store(false, std::memory_order_relaxed);
        // Remove independently: one failing provider must not retain the others.
        if (m_controllerEventRegistered[0]) {
            try { Gamepad::GamepadAdded(m_controllerEventTokens[0]); } catch (...) {}
        }
        if (m_controllerEventRegistered[1]) {
            try { Gamepad::GamepadRemoved(m_controllerEventTokens[1]); } catch (...) {}
        }
        if (m_controllerEventRegistered[2]) {
            try { RawGameController::RawGameControllerAdded(m_controllerEventTokens[2]); } catch (...) {}
        }
        if (m_controllerEventRegistered[3]) {
            try { RawGameController::RawGameControllerRemoved(m_controllerEventTokens[3]); } catch (...) {}
        }
        m_controllerEventRegistered.fill(false);
    }

    void TogglePage() {
        m_page = m_page == Page::Game ? Page::Runtime :
            (m_page == Page::Runtime ? Page::Controllers : Page::Game);
        // Changing pages must not reinterpret a held button as a new action.
        m_controllerConnected = false;
        m_previousButtons = 0;
        if (m_page == Page::Controllers) {
            RefreshControllers(true);
            ReadSelectedController();
        }
    }

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

    void PopulateControllerMetadata(ControllerEntry& entry) {
        if (!entry.raw) return;
        entry.name = L"Raw controller";
        try { entry.name = ModelName(entry.raw.DisplayName()); }
        catch (hresult_error const& error) { entry.metadataError = ApiError(L"Device name", error); }
        try {
            entry.vendor = entry.raw.HardwareVendorId();
            entry.product = entry.raw.HardwareProductId();
            entry.wireless = entry.raw.IsWireless();
            entry.buttonCount = entry.raw.ButtonCount();
            entry.axisCount = entry.raw.AxisCount();
            entry.switchCount = entry.raw.SwitchCount();
            entry.hasMetadata = true;
            entry.details = L"VID " + Hex(entry.vendor, 4) + L" / PID " + Hex(entry.product, 4) +
                (entry.wireless ? L" / wireless" : L" / wired");
        } catch (hresult_error const& error) {
            entry.metadataError = ApiError(L"Device metadata", error);
        } catch (...) {
            entry.metadataError = L"Device metadata query failed.";
        }
    }

    void RefreshControllers(bool force) {
        const auto now = std::chrono::steady_clock::now();
        const bool changed = m_controllerEvents->dirty.exchange(false, std::memory_order_acquire);
        if (!force && !changed && now - m_lastControllerRefresh < std::chrono::seconds(1)) return;
        m_lastControllerRefresh = now;
        m_inventoryTime = UtcNow();
        RawGameController selectedRaw{nullptr};
        Gamepad selectedGamepad{nullptr};
        if (m_controllerIndex < m_controllers.size()) {
            selectedRaw = m_controllers[m_controllerIndex].raw;
            selectedGamepad = m_controllers[m_controllerIndex].gamepad;
        }
        std::vector<ControllerEntry> entries;
        m_navigationGamepads.clear();
        m_rawApiOkay = false;
        m_gamepadApiOkay = false;
        m_associationWarning = false;
        try {
            const auto raw = RawGameController::RawGameControllers();
            m_rawCount = raw.Size();
            m_rawApiStatus = std::to_wstring(m_rawCount) + L" exposed";
            m_rawApiOkay = true;
            for (std::uint32_t index = 0; index < raw.Size() && entries.size() < MaxControllerEntries; ++index) {
                ControllerEntry entry;
                entry.raw = raw.GetAt(index);
                PopulateControllerMetadata(entry);
                try { entry.gamepad = Gamepad::FromGameController(entry.raw); }
                catch (...) { m_associationWarning = true; }
                entries.push_back(std::move(entry));
            }
        } catch (hresult_error const& error) {
            m_rawApiOkay = false;
            m_rawApiStatus = ApiError(L"Enumeration failed", error);
        } catch (...) {
            m_rawApiOkay = false;
            m_rawApiStatus = L"Enumeration failed (unknown error).";
        }
        try {
            const auto gamepads = Gamepad::Gamepads();
            m_gamepadCount = gamepads.Size();
            m_gamepadApiStatus = std::to_wstring(m_gamepadCount) + L" exposed";
            m_gamepadApiOkay = true;
            for (std::uint32_t index = 0; index < gamepads.Size() && index < MaxControllerEntries; ++index) {
                const auto gamepad = gamepads.GetAt(index);
                m_navigationGamepads.push_back(gamepad);
                RawGameController raw{nullptr};
                try { raw = RawGameController::FromGameController(gamepad); }
                catch (...) { m_associationWarning = true; }
                const auto found = std::find_if(entries.begin(), entries.end(), [&](ControllerEntry const& entry) {
                    return (entry.gamepad && entry.gamepad == gamepad) || (raw && entry.raw == raw);
                });
                if (found != entries.end()) {
                    found->gamepad = gamepad;
                } else if (entries.size() < MaxControllerEntries) {
                    ControllerEntry entry;
                    entry.raw = raw;
                    entry.gamepad = gamepad;
                    PopulateControllerMetadata(entry);
                    entries.push_back(std::move(entry));
                }
            }
        } catch (hresult_error const& error) {
            m_gamepadApiOkay = false;
            m_gamepadApiStatus = ApiError(L"Enumeration failed", error);
        } catch (...) {
            m_gamepadApiOkay = false;
            m_gamepadApiStatus = L"Enumeration failed (unknown error).";
        }
        const auto found = std::find_if(entries.begin(), entries.end(), [&](ControllerEntry const& entry) {
            return (selectedRaw && entry.raw == selectedRaw) ||
                (selectedGamepad && entry.gamepad == selectedGamepad);
        });
        const bool sameSelection = found != entries.end();
        m_controllerIndex = sameSelection ? static_cast<std::size_t>(found - entries.begin()) : 0;
        m_controllers = std::move(entries);
        if (!sameSelection) ResetControllerReading();
    }

    void ResetControllerReading() {
        m_rawReadingOkay = false;
        m_gamepadReadingOkay = false;
        m_rawReadingStatus = L"Not sampled";
        m_gamepadReadingStatus = L"Not sampled";
        m_rawButtons = {};
        m_rawAxes = {};
        m_rawSwitches = {};
        m_controllerConnected = false;
        m_previousButtons = 0;
    }

    void SelectController(int direction) {
        if (m_controllers.empty()) return;
        if (direction > 0) m_controllerIndex = (m_controllerIndex + 1) % m_controllers.size();
        else m_controllerIndex = (m_controllerIndex + m_controllers.size() - 1) % m_controllers.size();
        ResetControllerReading();
        ReadSelectedController();
    }

    void ReadSelectedController() {
        m_rawReadingOkay = false;
        m_gamepadReadingOkay = false;
        if (m_controllerIndex >= m_controllers.size()) return;
        auto const& entry = m_controllers[m_controllerIndex];
        m_readingTime = UtcNow();
        if (entry.gamepad) {
            try {
                m_gamepadReading = entry.gamepad.GetCurrentReading();
                m_gamepadReadingOkay = true;
                m_gamepadReadingStatus = L"Reading received";
            } catch (hresult_error const& error) { m_gamepadReadingStatus = ApiError(L"Reading failed", error); }
            catch (...) { m_gamepadReadingStatus = L"Reading failed (unknown error)."; }
        } else m_gamepadReadingStatus = L"No Gamepad mapping for this entry";
        if (!entry.raw) {
            m_rawReadingStatus = L"No RawGameController association";
            return;
        }
        if (!entry.hasMetadata) {
            m_rawReadingStatus = L"Raw reading unavailable: metadata query failed";
            return;
        }
        if (entry.buttonCount < 0 || entry.axisCount < 0 || entry.switchCount < 0 ||
            static_cast<std::uint32_t>(entry.buttonCount) > MaxRawButtons ||
            static_cast<std::uint32_t>(entry.axisCount) > MaxRawAxes ||
            static_cast<std::uint32_t>(entry.switchCount) > MaxRawSwitches) {
            m_rawReadingStatus = L"Raw reading skipped: control counts exceed diagnostic bounds";
            return;
        }
        try {
            const auto buttons = static_cast<std::uint32_t>(entry.buttonCount);
            const auto axes = static_cast<std::uint32_t>(entry.axisCount);
            const auto switches = static_cast<std::uint32_t>(entry.switchCount);
            if (m_rawButtons.size() != buttons) m_rawButtons = com_array<bool>(buttons);
            if (m_rawAxes.size() != axes) m_rawAxes = com_array<double>(axes);
            if (m_rawSwitches.size() != switches) m_rawSwitches = com_array<GameControllerSwitchPosition>(switches);
            entry.raw.GetCurrentReading(m_rawButtons, m_rawSwitches, m_rawAxes);
            m_rawReadingOkay = true;
            m_rawReadingStatus = L"Reading received";
        } catch (hresult_error const& error) { m_rawReadingStatus = ApiError(L"Reading failed", error); }
        catch (...) { m_rawReadingStatus = L"Reading failed (unknown error)."; }
    }

    void PollGamepad() {
        try {
            Gamepad gamepad{nullptr};
            if (m_controllerIndex < m_controllers.size()) gamepad = m_controllers[m_controllerIndex].gamepad;
            if (!gamepad && !m_navigationGamepads.empty()) gamepad = m_navigationGamepads.front();
            if (!gamepad) {
                m_previousButtons = 0;
                m_controllerConnected = false;
                m_gamepad = nullptr;
                return;
            }
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
            if (edge(GamepadButtons::Y)) {
                TogglePage();
                return;
            }
            if (m_page == Page::Game) {
                if (edge(GamepadButtons::DPadRight) || edge(GamepadButtons::DPadDown))
                    m_gameSelection = (m_gameSelection + 1) % 3;
                if (edge(GamepadButtons::DPadLeft) || edge(GamepadButtons::DPadUp))
                    m_gameSelection = (m_gameSelection + 2) % 3;
                if (edge(GamepadButtons::A)) ActivateGameSelection();
                if (edge(GamepadButtons::X)) SaveGameReportAsync();
                return;
            }
            if (m_page == Page::Controllers) {
                if (edge(GamepadButtons::LeftShoulder)) SelectController(-1);
                else if (edge(GamepadButtons::RightShoulder)) SelectController(1);
                if (edge(GamepadButtons::X)) SaveReportAsync();
                return;
            }
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

    void ActivateGameSelection() {
        if (m_gameSelection == 0) InspectGameFolderAsync(true);
        else if (m_gameSelection == 1) InspectGameFolderAsync(false);
        else SaveGameReportAsync();
    }

    fire_and_forget InspectGameFolderAsync(bool chooseFolder) {
        if (m_gameBusy || m_gameSaving || m_closed) co_return;
        auto lifetime = get_strong();
        m_gameBusy = true;
        m_gameStatus = chooseFolder ? L"Choose the USB folder containing gta_sa.exe." :
            L"Opening the selected game folder...";
        StorageFolder folder = m_gameFolder;
        std::wstring failure;
        try {
            // Pickers must be created and displayed on the CoreWindow UI thread.
            // Their grant authorizes this folder directly; game files stay on USB.
            if (chooseFolder) {
                Windows::Storage::Pickers::FolderPicker picker;
                picker.ViewMode(Windows::Storage::Pickers::PickerViewMode::List);
                picker.SuggestedStartLocation(Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
                picker.FileTypeFilter().Append(L"*");
                folder = co_await picker.PickSingleFolderAsync();
                if (!folder) {
                    m_gameStatus = L"Folder selection cancelled. Choose a folder to inspect its game files.";
                    m_gameBusy = false;
                    co_return;
                }
            } else if (!folder) {
                const auto grants = Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList();
                if (grants.ContainsItem(GameFolderToken))
                    folder = co_await grants.GetFolderAsync(GameFolderToken);
                else {
                    m_gameStatus = L"Choose a game folder first. No saved folder is available.";
                    m_gameBusy = false;
                    co_return;
                }
            }
            if (m_closed) { m_gameBusy = false; co_return; }
            m_gameFolder = folder;
            m_gameFolderLabel = folder.DisplayName().c_str();
            m_gameFolderPath = folder.Path().c_str();
            try {
                Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList().AddOrReplace(
                    GameFolderToken, folder);
            } catch (...) {
                // A session grant is still usable if persistence is unavailable.
            }
        } catch (hresult_error const& error) {
            failure = ApiError(L"Folder access failed", error) + L". " + error.message().c_str();
        } catch (...) {
            failure = L"Folder access failed. The Xbox file picker or USB location may be unavailable.";
        }

        const auto dispatcher = m_window.Dispatcher();
        std::wstring summary;
        std::wstring details;
        std::wstring inventory;
        bool metadataRecognized = false;
        if (failure.empty()) {
            m_gameStatus = L"Reading gta_sa.exe and folder metadata directly from the selected location...";
            m_gameSummary = L"Inspecting executable metadata...";
            try {
                co_await resume_background();
                // Storage I/O and PE parsing never block the UI render loop.
                const auto file = folder.GetFileAsync(L"gta_sa.exe").get();
                const auto stream = file.OpenReadAsync().get();
                const auto size = stream.Size();
                if (size == 0 || size > MaxGameImageBytes) {
                    summary = L"Executable skipped: file size is outside the 1 byte to 64 MiB inspection limit.";
                    details = summary + L"\nFile size: " + std::to_wstring(size) + L" bytes\n";
                } else {
                    Windows::Storage::Streams::DataReader reader(stream.GetInputStreamAt(0));
                    const auto wanted = static_cast<std::uint32_t>(size);
                    const auto loaded = reader.LoadAsync(wanted).get();
                    if (loaded != wanted) {
                        summary = L"Executable read was incomplete. Check that the USB remains connected.";
                        details = summary + L"\nExpected/read bytes: " + std::to_wstring(wanted) + L"/" +
                            std::to_wstring(loaded) + L"\n";
                    } else {
                        std::vector<std::uint8_t> bytes(wanted);
                        reader.ReadBytes(bytes);
                        const auto result = xcdm::runtime::InspectGamePe(bytes.data(), bytes.size());
                        metadataRecognized = result.ok;
                        summary = Wide(result.summary);
                        details = L"File name: gta_sa.exe\nFile size: " + std::to_wstring(size) +
                            L" bytes\n" + Wide(result.report);
                        try {
                            const auto algorithm = Windows::Security::Cryptography::Core::HashAlgorithmProvider::OpenAlgorithm(
                                Windows::Security::Cryptography::Core::HashAlgorithmNames::Sha256());
                            const auto hash = algorithm.HashData(
                                Windows::Security::Cryptography::CryptographicBuffer::CreateFromByteArray(bytes));
                            details += L"\nSHA-256: ";
                            details += Windows::Security::Cryptography::CryptographicBuffer::EncodeToHexString(hash).c_str();
                            details += L"\n";
                        } catch (hresult_error const& error) {
                            details += L"\n" + ApiError(L"SHA-256 unavailable", error) + L"\n";
                        }
                    }
                }
                std::wostringstream listing;
                listing << L"Root folder files (first " << MaxGameFolderEntries << L" at most):\n";
                try {
                    const auto files = folder.GetFilesAsync(
                        Windows::Storage::Search::CommonFileQuery::DefaultQuery, 0, MaxGameFolderEntries).get();
                    std::uint32_t modules = 0;
                    for (auto const& item : files) {
                        const std::wstring name(item.Name().c_str());
                        std::wstring extension(item.FileType().c_str());
                        std::transform(extension.begin(), extension.end(), extension.begin(),
                            [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                        if (extension == L".dll" || extension == L".asi") ++modules;
                        listing << L"  " << name.substr(0, 260) << L"\n";
                    }
                    listing << L"DLL/ASI files in this bounded listing: " << modules << L"\n";
                    if (files.Size() == MaxGameFolderEntries)
                        listing << L"Listing reached its limit; more files may be present.\n";
                } catch (hresult_error const& error) {
                    listing << ApiError(L"Folder listing unavailable", error) << L"\n";
                }
                listing << L"Known game folders (presence only):\n";
                for (auto const* name : {L"data", L"models", L"audio", L"text", L"scripts", L"plugins"}) {
                    try {
                        const auto item = folder.TryGetItemAsync(name).get();
                        listing << L"  " << name << L": " <<
                            (item && item.IsOfType(StorageItemTypes::Folder) ? L"folder present" : L"not a folder / absent") << L"\n";
                    } catch (...) { listing << L"  " << name << L": query failed\n"; }
                }
                inventory = listing.str();
            } catch (hresult_error const& error) {
                failure = ApiError(L"Game read failed", error) + L". " + error.message().c_str();
            } catch (std::exception const&) {
                failure = L"Game inspection failed. Reconnect the USB and choose the folder containing gta_sa.exe.";
            } catch (...) {
                failure = L"Game inspection failed with an unknown storage or metadata error.";
            }
        }
        // This also covers failures thrown after switching to a background thread.
        try { co_await resume_foreground(dispatcher); }
        catch (...) {
            OutputDebugStringW(L"Game preflight could not return to the UI dispatcher.\n");
            co_return;
        }
        m_gameBusy = false;
        if (m_closed) co_return;
        m_gameMetadataRecognized = metadataRecognized && failure.empty();
        m_gameSummary = failure.empty() ? summary : failure;
        m_gameScanTime = UtcNow();
        std::wostringstream report;
        report << L"Xbox Desktop Mode / Game preflight\n"
               << L"Report created: " << m_gameScanTime << L"\n"
               << L"Device family: " << m_deviceFamily << L"\nOS version: " << m_osVersion << L"\n"
               << L"Application architecture: x64\n"
               << L"Selected folder: " << m_gameFolderPath << L"\n"
               << L"Metadata recognized: " << (m_gameMetadataRecognized ? L"yes" : L"no") << L"\n\n"
               << m_gameSummary << L"\n\n" << details << L"\n" << inventory << L"\n"
               << L"Operation: metadata inspection in the selected folder. No game was copied or executed.\n"
               << L"Windows x86 execution and Direct3D 9 game compatibility are not implemented.\n"
               << L"A readable PE file is not proof that its code can execute on Xbox.\n";
        try {
            const auto version = Package::Current().Id().Version();
            report << L"Package version: " << version.Major << L'.' << version.Minor << L'.'
                   << version.Build << L'.' << version.Revision << L"\n";
        } catch (...) { report << L"Package version unavailable\n"; }
        m_gameReport = report.str();
        m_gameStatus = failure.empty() ? L"Inspection complete. Saving the metadata report..." :
            L"Inspection could not complete. Saving the error report; choose the game folder again to retry.";
        SaveGameReportAsync();
    }

    fire_and_forget SaveGameReportAsync() {
        if (m_gameBusy || m_gameSaving || m_closed) co_return;
        if (m_gameReport.empty()) {
            m_gameStatus = L"Inspect a game folder first. No game report is available yet.";
            co_return;
        }
        auto lifetime = get_strong();
        m_gameSaving = true;
        const auto report = m_gameReport;
        try {
            const auto file = co_await ApplicationData::Current().LocalFolder().CreateFileAsync(
                L"game-preflight.txt", CreationCollisionOption::ReplaceExisting);
            co_await FileIO::WriteTextAsync(file, hstring(report), Windows::Storage::Streams::UnicodeEncoding::Utf8);
            if (!m_closed) {
                m_gameReportPath = file.Path().c_str();
                m_gameStatus = L"Saved game-preflight.txt. The report contains metadata only; game execution is unavailable.";
            }
        } catch (hresult_error const& error) {
            if (!m_closed) m_gameStatus = ApiError(L"Report save failed", error);
        } catch (...) {
            if (!m_closed) m_gameStatus = L"Game report could not be saved. Press Save report to retry.";
        }
        m_gameSaving = false;
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
        try {
            const auto version = Package::Current().Id().Version();
            report << L"Package version: " << version.Major << L'.' << version.Minor << L'.'
                   << version.Build << L'.' << version.Revision << L"\n\n";
        } catch (...) { report << L"Package version unavailable\n\n"; }
        auto append = [&report](wchar_t const* title, ProbeResult const& result) {
            report << title << L"\nStatus: " << StatusName(result.status)
                   << L"\nError code: " << result.errorCode
                   << L"\nMessage: " << (result.message.empty() ? L"No result yet." : Wide(result.message))
                   << L"\n\n";
        };
        append(L"Executable memory (write, then execute)", m_memoryResult);
        append(L"Minimal x64 Portable Executable", m_peResult);
        report << L"Controller APIs (counts overlap; do not add them)\n"
               << L"Inventory sampled: " << m_inventoryTime << L"\n"
               << L"RawGameController: " << m_rawApiStatus << L"\n"
               << L"Gamepad: " << m_gamepadApiStatus << L"\n"
               << L"Grouped entries shown: " << m_controllers.size() << L" (limit " << MaxControllerEntries << L")\n"
               << L"Gamepad mappings are grouped with their raw entry where an association is available.\n"
               << L"Unassociated entries are not proof of additional physical devices.\n";
        constexpr std::array<wchar_t const*, 4> eventNames{{
            L"GamepadAdded", L"GamepadRemoved", L"RawGameControllerAdded", L"RawGameControllerRemoved"
        }};
        for (std::size_t index = 0; index < eventNames.size(); ++index) {
            report << eventNames[index] << L": " << m_controllerEventStatus[index]
                   << L"; received=" << m_controllerEvents->counts[index].load(std::memory_order_relaxed) << L"\n";
        }
        if (m_associationWarning) report << L"One or more association queries failed; grouping may be incomplete.\n";
        for (std::size_t index = 0; index < m_controllers.size(); ++index) {
            auto const& entry = m_controllers[index];
            report << L"\nEntry " << index + 1 << L": " << entry.name << L"\n"
                   << L"Raw association: " << (entry.raw ? L"yes" : L"no") << L"\n"
                   << L"Gamepad mapping: " << (entry.gamepad ? L"yes" : L"no") << L"\n";
            if (entry.hasMetadata) {
                report << entry.details << L"\n"
                       << L"Declared buttons/axes/switches: " << entry.buttonCount << L'/'
                       << entry.axisCount << L'/' << entry.switchCount << L"\n";
            }
            if (!entry.metadataError.empty()) report << entry.metadataError << L"\n";
        }
        if (m_controllerIndex < m_controllers.size()) {
            report << L"\nSelected entry: " << m_controllerIndex + 1 << L"\n"
                   << L"Input sampled: " << m_readingTime << L"\n"
                   << L"Gamepad input: " << m_gamepadReadingStatus << L"\n";
            if (m_gamepadReadingOkay) {
                report << GamepadSummary() << L"\n";
            }
            report << L"Raw input: " << m_rawReadingStatus << L"\n";
            if (m_rawReadingOkay) {
                report << L"Raw pressed buttons (zero-based): " << RawButtonSummary(MaxRawButtons) << L"\n"
                       << L"Raw axes: " << RawAxisSummary(MaxRawAxes) << L"\n"
                       << L"Raw switches (enum values): " << RawSwitchSummary(MaxRawSwitches) << L"\n";
            }
        } else if (m_rawApiOkay && m_gamepadApiOkay) {
            report << L"\nNo controller entry is currently exposed to this application.\n";
        } else {
            report << L"\nController inventory is incomplete because an API query failed.\n";
        }
        report << L"\nThis checks input exposed by Xbox/Windows to this UWP application.\n"
                  L"It does not install USB/Bluetooth drivers or establish Xbox 360 controller compatibility.\n\n";
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
            RefreshControllers(true);
            ReadSelectedController();
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

    std::wstring GamepadSummary() const {
        std::wostringstream text;
        text << std::fixed << std::setprecision(3)
             << L"Buttons: " << GamepadButtonNames(m_gamepadReading.Buttons) << L"\n"
             << L"Triggers: L " << m_gamepadReading.LeftTrigger << L"  R " << m_gamepadReading.RightTrigger << L"\n"
             << L"Sticks: L (" << m_gamepadReading.LeftThumbstickX << L", " << m_gamepadReading.LeftThumbstickY
             << L")  R (" << m_gamepadReading.RightThumbstickX << L", " << m_gamepadReading.RightThumbstickY << L')';
        return text.str();
    }

    std::wstring RawButtonSummary(std::size_t limit) const {
        std::wstring text;
        std::size_t pressed = 0;
        for (std::size_t index = 0; index < m_rawButtons.size(); ++index) {
            if (!m_rawButtons[index]) continue;
            if (pressed < limit) {
                if (!text.empty()) text += L", ";
                text += std::to_wstring(index);
            }
            ++pressed;
        }
        if (pressed > limit) text += L" ... (" + std::to_wstring(pressed) + L" pressed)";
        return text.empty() ? L"none" : text;
    }

    std::wstring RawAxisSummary(std::size_t limit) const {
        std::wostringstream text;
        text << std::fixed << std::setprecision(3);
        const auto count = std::min<std::size_t>(m_rawAxes.size(), limit);
        for (std::size_t index = 0; index < count; ++index) {
            if (index != 0) text << (index % 4 == 0 ? L"\n" : L"   ");
            text << index << L'=' << m_rawAxes[index];
        }
        if (m_rawAxes.size() > count) text << L" ... (" << m_rawAxes.size() << L" axes)";
        return count == 0 ? L"none" : text.str();
    }

    std::wstring RawSwitchSummary(std::size_t limit) const {
        std::wostringstream text;
        const auto count = std::min<std::size_t>(m_rawSwitches.size(), limit);
        for (std::size_t index = 0; index < count; ++index) {
            if (index != 0) text << L"  ";
            text << index << L'=' << static_cast<std::int32_t>(m_rawSwitches[index]);
        }
        if (m_rawSwitches.size() > count) text << L" ... (" << m_rawSwitches.size() << L" switches)";
        return count == 0 ? L"none" : text.str();
    }

    void ActionButton(D2D1_RECT_F rect, std::wstring_view label) {
        Fill(rect, Contains(rect, m_pointer) ? 0x96F36F : 0x87DD61, 8.0f);
        rect.left += 20;
        rect.top += 16;
        Text(label, rect, m_body.get(), 0x13200E);
    }

    void RenderGame() {
        Text(L"Choose the folder containing gta_sa.exe on your USB. Files are inspected in place.",
             {96, 218, 1796, 248}, m_small.get(), 0xAAB9AF);
        Fill({96, 256, 748, 724}, 0x151F19, 14.0f);
        Border({96, 256, 748, 724}, 0x2B3C30, 1.0f, 14.0f);
        Fill({776, 256, 1796, 724}, 0x151F19, 14.0f);
        Border({776, 256, 1796, 724}, 0x2B3C30, 1.0f, 14.0f);
        Text(L"Selected game folder", {124, 277, 720, 324}, m_heading.get(), 0xF0F5F1);
        Text(m_gameFolderLabel.empty() ? L"No folder selected" : m_gameFolderLabel,
             {124, 345, 720, 420}, m_body.get(), 0xD1DCD5);
        Text(m_gameFolderPath.empty() ? L"Select the folder through the Xbox file picker. A previous selection can be rescanned." :
             m_gameFolderPath, {124, 432, 720, 562}, m_small.get(), 0xAAB9AF);
        Text(L"Last inspection: " + m_gameScanTime, {124, 608, 720, 680}, m_small.get(), 0x9FB0A4);
        Text(L"Executable inspection", {806, 277, 1768, 324}, m_heading.get(), 0xF0F5F1);
        Text(m_gameSummary, {806, 338, 1768, 528}, m_body.get(),
             m_gameMetadataRecognized ? 0xD1DCD5 : 0xF1C875);
        Text(L"Windows x86 execution and Direct3D 9 compatibility are not implemented. This page reads metadata; it does not launch San Andreas.",
             {806, 555, 1768, 700}, m_small.get(), 0xF1C875);
        const std::array<std::wstring, 3> labels{{
            m_gameBusy ? L"Inspecting..." : L"Choose game folder",
            L"Rescan selected folder",
            m_gameSaving ? L"Saving..." : L"Save report"
        }};
        for (std::size_t index = 0; index < GameButtons.size(); ++index) {
            const auto rect = GameButtons[index];
            const bool disabled = m_gameBusy || m_gameSaving;
            Fill(rect, disabled ? 0x243128 : (Contains(rect, m_pointer) ? 0x96F36F : 0x87DD61), 8.0f);
            if (m_gameSelection == static_cast<int>(index))
                Border({rect.left - 5, rect.top - 5, rect.right + 5, rect.bottom + 5}, 0xDEFBCF, 2.0f, 11.0f);
            Text(labels[index], {rect.left + 20, rect.top + 16, rect.right - 10, rect.bottom},
                 m_body.get(), disabled ? 0x8D9C92 : 0x13200E);
        }
        Fill({96, 840, 1796, 938}, 0x121C16, 14.0f);
        Text(m_gameStatus, {124, 851, 1768, 908}, m_small.get(), 0xD1DCD5);
        if (!m_gameReportPath.empty())
            Text(L"Report: game-preflight.txt in this app's local storage", {124, 909, 1768, 937}, m_small.get(), 0x9FB0A4);
        Text(L"D-pad  Select action     A  Activate     X  Save report     Y  Runtime checks",
             {96, 959, 1796, 998}, m_body.get(), 0xD9E5DD);
        Text(L"Keyboard: O choose folder  |  R / F5 rescan  |  S save  |  arrows / Tab select  |  Enter activate  |  C switch page",
             {96, 1006, 1796, 1043}, m_small.get(), 0x9FB0A4);
    }

    void RenderControllers() {
        Text(L"RawGameController: " + m_rawApiStatus + L"   |   Gamepad: " + m_gamepadApiStatus +
             L"   (counts overlap)", {96, 218, 1796, 248}, m_small.get(), 0xAAB9AF);
        Fill({96, 256, 748, 724}, 0x151F19, 14.0f);
        Border({96, 256, 748, 724}, 0x2B3C30, 1.0f, 14.0f);
        Fill({776, 256, 1796, 724}, 0x151F19, 14.0f);
        Border({776, 256, 1796, 724}, 0x2B3C30, 1.0f, 14.0f);
        Text(L"Exposed entries", {124, 277, 720, 324}, m_heading.get(), 0xF0F5F1);
        if (m_controllers.empty()) {
            const bool empty = m_rawApiOkay && m_gamepadApiOkay;
            Text(empty ? L"No devices exposed to this app." : L"Controller API query failed.",
                 {124, 345, 716, 420}, m_body.get(), empty ? 0xD1DCD5 : 0xF1C875);
            Text(L"Connect the controller, then refresh. A USB connection alone does not establish input support.",
                 {124, 447, 716, 560}, m_small.get(), 0xAAB9AF);
            Text(L"Only devices returned by Xbox/Windows appear here. No USB or Bluetooth driver is installed.",
                 {806, 340, 1768, 470}, m_body.get(), 0xD1DCD5);
        } else {
            const auto first = (m_controllerIndex / VisibleControllerRows) * VisibleControllerRows;
            for (std::size_t row = 0; row < VisibleControllerRows && first + row < m_controllers.size(); ++row) {
                const auto index = first + row;
                const auto top = 328.0f + static_cast<float>(row) * 60.0f;
                auto const& entry = m_controllers[index];
                if (index == m_controllerIndex) {
                    Fill({112, top, 732, top + 56}, 0x263C2C, 6.0f);
                    Border({112, top, 732, top + 56}, 0x8CEB67, 1.0f, 6.0f);
                }
                Text(std::to_wstring(index + 1) + L". " + entry.name.substr(0, 42),
                     {124, top + 3, 720, top + 30}, m_small.get(), 0xF0F5F1);
                const auto kind = entry.raw ? (entry.gamepad ? L"Raw + Gamepad mapping" : L"Raw; no Gamepad mapping") :
                    L"Gamepad; raw association unavailable";
                Text(kind, {124, top + 29, 720, top + 56}, m_small.get(), 0xAAB9AF);
            }
            Text(L"Entry " + std::to_wstring(m_controllerIndex + 1) + L" / " + std::to_wstring(m_controllers.size()),
                 {124, 695, 720, 724}, m_small.get(), 0xAAB9AF);
            auto const& entry = m_controllers[m_controllerIndex];
            Text(entry.name.substr(0, 50), {806, 277, 1768, 322}, m_heading.get(), 0xF0F5F1);
            Text(entry.hasMetadata ? entry.details : L"Hardware details unavailable",
                 {806, 325, 1768, 355}, m_small.get(), 0xAAB9AF);
            Text(entry.hasMetadata ? L"Declared buttons / axes / switches: " + std::to_wstring(entry.buttonCount) +
                 L" / " + std::to_wstring(entry.axisCount) + L" / " + std::to_wstring(entry.switchCount) : entry.metadataError,
                 {806, 358, 1768, 389}, m_small.get(), 0xAAB9AF);
            Text(L"Gamepad: " + m_gamepadReadingStatus, {806, 395, 1768, 426}, m_small.get(),
                 m_gamepadReadingOkay ? 0x8CEB67 : 0xF1C875);
            if (m_gamepadReadingOkay) {
                Text(GamepadSummary(), {806, 430, 1768, 536}, m_small.get(), 0xD1DCD5);
            }
            Text(L"Raw: " + m_rawReadingStatus, {806, 539, 1768, 571}, m_small.get(),
                 m_rawReadingOkay ? 0x8CEB67 : 0xF1C875);
            if (m_rawReadingOkay) {
                Text(L"Pressed buttons: " + RawButtonSummary(20), {806, 575, 1768, 607}, m_small.get(), 0xD1DCD5);
                Text(L"Axes: " + RawAxisSummary(8), {806, 609, 1768, 667}, m_small.get(), 0xD1DCD5);
                Text(L"Switches (enum): " + RawSwitchSummary(8), {806, 673, 1768, 706}, m_small.get(), 0xD1DCD5);
            }
        }
        ActionButton(ControllerButtons[0], L"Previous device");
        ActionButton(ControllerButtons[1], L"Next device");
        ActionButton(ControllerButtons[2], L"Refresh inventory");
        ActionButton(ControllerButtons[3], m_saving ? L"Saving..." : L"Save report");
        Fill({96, 840, 1796, 938}, 0x121C16, 14.0f);
        Text(m_actionStatus, {124, 858, 1768, 925}, m_small.get(), 0xD1DCD5);
        Text(L"LB / RB  Select device     X  Save report     Y  Game preflight", {96, 959, 1796, 998}, m_body.get(), 0xD9E5DD);
        Text(L"Keyboard: arrows / Tab select  |  R / F5 refresh  |  S save  |  C switch page. No controller driver is installed.",
             {96, 1006, 1796, 1043}, m_small.get(), 0x9FB0A4);
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
        Text(m_page == Page::Game ? L"Game preflight" : (m_page == Page::Controllers ? L"Controller diagnostics" : L"Runtime diagnostics"),
             {91, 111, 1450, 192}, m_title.get(), 0xF0F5F1);
        Text(m_deviceFamily + L"  /  x64", {1340, 78, 1796, 116}, m_small.get(), 0xAAB9AF);
        ActionButton(PageButton, m_page == Page::Game ? L"Runtime checks" :
            (m_page == Page::Runtime ? L"Controllers" : L"Game preflight"));
        if (m_page == Page::Game) {
            RenderGame();
        } else if (m_page == Page::Controllers) {
            RenderControllers();
        } else {
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
        Text(L"A  Run both     X  Save report     B  Select report     Y  Controllers", {96, 959, 1796, 998}, m_body.get(), 0xD9E5DD);
        Text(L"Keyboard: Enter / F5 run  |  S save  |  Tab / arrows focus  |  Space activate  |  Esc select report",
             {96, 1006, 1796, 1043}, m_small.get(), 0x9FB0A4);
        }
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
    Page m_page{Page::Game};
    StorageFolder m_gameFolder{nullptr};
    bool m_gameBusy{false}, m_gameSaving{false}, m_gameMetadataRecognized{false};
    int m_gameSelection{0};
    std::wstring m_gameFolderLabel, m_gameFolderPath, m_gameReport, m_gameReportPath;
    std::wstring m_gameSummary{L"Choose the game folder to inspect gta_sa.exe and its imported Windows libraries."};
    std::wstring m_gameStatus{L"Ready. Choose the USB folder containing gta_sa.exe. No game files need to be copied."};
    std::wstring m_gameScanTime{L"Not run"};
    bool m_rawApiOkay{false}, m_gamepadApiOkay{false}, m_associationWarning{false};
    bool m_rawReadingOkay{false}, m_gamepadReadingOkay{false};
    std::uint32_t m_rawCount{0}, m_gamepadCount{0};
    std::size_t m_controllerIndex{0};
    std::vector<ControllerEntry> m_controllers;
    std::vector<Gamepad> m_navigationGamepads;
    std::shared_ptr<ControllerEventState> m_controllerEvents{std::make_shared<ControllerEventState>()};
    std::array<event_token, 4> m_controllerEventTokens{};
    std::array<bool, 4> m_controllerEventRegistered{};
    std::array<std::wstring, 4> m_controllerEventStatus{{
        L"Not registered", L"Not registered", L"Not registered", L"Not registered"
    }};
    com_array<bool> m_rawButtons;
    com_array<double> m_rawAxes;
    com_array<GameControllerSwitchPosition> m_rawSwitches;
    GamepadReading m_gamepadReading{};
    std::chrono::steady_clock::time_point m_lastControllerRefresh{};
    std::wstring m_rawApiStatus{L"Not queried"}, m_gamepadApiStatus{L"Not queried"};
    std::wstring m_rawReadingStatus{L"Not sampled"}, m_gamepadReadingStatus{L"Not sampled"};
    std::wstring m_inventoryTime{L"Not sampled"}, m_readingTime{L"Not sampled"};
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
