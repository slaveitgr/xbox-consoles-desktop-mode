// SPDX-License-Identifier: MIT
// In-app window chrome for native guests. No desktop HWND or process APIs.
#pragma once
#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

namespace XWindows {
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::UI;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;

class HostedWindow : public std::enable_shared_from_this<HostedWindow> {
    enum class Mode { Windowed, Maximized, Fullscreen };
    Grid^ m_parent = nullptr;
    Canvas^ m_layer = nullptr;
    Grid^ m_frame = nullptr;
    Border^ m_border = nullptr;
    Grid^ m_caption = nullptr;
    RowDefinition^ m_captionRow = nullptr;
    Button^ m_restore = nullptr;
    Button^ m_maximize = nullptr;
    Button^ m_leaveFullscreen = nullptr;
    Border^ m_grip = nullptr;
    std::function<void()> m_close;
    std::function<void(bool)> m_input;
    Mode m_mode = Mode::Windowed, m_beforeFullscreen = Mode::Windowed;
    Rect m_normal{}, m_bounds{};
    double m_width = 1920, m_height = 1080;
    bool m_open = false, m_minimized = false, m_active = false;
    bool m_dragging = false, m_resizing = false;
    Point m_pointerStart;
    Rect m_dragStart;

    static Rect Bounds(double x, double y, double w, double h) {
        Rect r; r.X=static_cast<float>(x); r.Y=static_cast<float>(y);
        r.Width=static_cast<float>(w); r.Height=static_cast<float>(h); return r;
    }
    static CornerRadius Corners(double value) {
        CornerRadius c; c.TopLeft=c.TopRight=c.BottomLeft=c.BottomRight=value; return c;
    }
    static SolidColorBrush^ Brush(unsigned char r, unsigned char g, unsigned char b) {
        Color c; c.A = 255; c.R = r; c.G = g; c.B = b;
        return ref new SolidColorBrush(c);
    }
    static Button^ Control(String^ text, String^ tip) {
        auto b = ref new Button(); b->Content = text;
        b->Width = 42; b->Height = 36; b->Padding = ThicknessHelper::FromUniformLength(0);
        b->Background = ref new SolidColorBrush(Colors::Transparent);
        b->Foreground = Brush(235, 242, 252);
        b->BorderThickness = ThicknessHelper::FromUniformLength(0);
        ToolTipService::SetToolTip(b, tip);
        Windows::UI::Xaml::Automation::AutomationProperties::SetName(b, tip);
        return b;
    }
    void Active(bool value) {
        m_active = value && m_open && !m_minimized;
        if (m_input) m_input(m_active);
    }
    void Layout() {
        const double workspace = (std::max)(1.0, m_height - 48);
        const double minWidth = (std::min)(480.0, m_width);
        const double minHeight = (std::min)(280.0, workspace);
        m_normal.Width = static_cast<float>((std::clamp)(static_cast<double>(m_normal.Width), minWidth, m_width));
        m_normal.Height = static_cast<float>((std::clamp)(static_cast<double>(m_normal.Height), minHeight, workspace));
        m_normal.X = static_cast<float>((std::clamp)(static_cast<double>(m_normal.X), 0.0, m_width - m_normal.Width));
        m_normal.Y = static_cast<float>((std::clamp)(static_cast<double>(m_normal.Y), 0.0, workspace - m_normal.Height));
        m_bounds = m_mode == Mode::Windowed ? m_normal : Bounds(0, 0, static_cast<float>(m_width),
            static_cast<float>(m_mode == Mode::Fullscreen ? m_height : workspace));
        Canvas::SetLeft(m_frame, m_bounds.X); Canvas::SetTop(m_frame, m_bounds.Y);
        m_frame->Width = m_bounds.Width; m_frame->Height = m_bounds.Height;
        const bool full = m_mode == Mode::Fullscreen;
        m_caption->Visibility = full ? Visibility::Collapsed : Visibility::Visible;
        m_captionRow->Height = full ? GridLengthHelper::FromValueAndType(0, GridUnitType::Pixel) : GridLengthHelper::Auto;
        m_leaveFullscreen->Visibility = full ? Visibility::Visible : Visibility::Collapsed;
        m_grip->Visibility = m_mode == Mode::Windowed ? Visibility::Visible : Visibility::Collapsed;
        m_border->CornerRadius = full || m_mode == Mode::Maximized ? Corners(0) : Corners(9);
        m_border->BorderThickness = ThicknessHelper::FromUniformLength(full ? 0 : 1);
        m_maximize->Content = ref new String(m_mode == Mode::Maximized ? L"\u2750" : L"\u25a1");
        Canvas::SetLeft(m_restore, 10); Canvas::SetTop(m_restore, (std::max)(0.0, m_height - 44));
    }
    void Init(Grid^ parent) {
        m_parent = parent; m_layer = ref new Canvas();
        // No background: clicks outside the actual frame reach the desktop.
        parent->Children->Append(m_layer);
        m_frame = ref new Grid(); m_frame->Visibility = Visibility::Collapsed;
        m_border = ref new Border(); m_border->Background = Brush(25, 32, 44);
        m_border->BorderBrush = Brush(94, 119, 150);
        auto content = ref new Grid(); m_border->Child = content;
        m_captionRow = ref new RowDefinition(); m_captionRow->Height = GridLengthHelper::Auto;
        content->RowDefinitions->Append(m_captionRow); content->RowDefinitions->Append(ref new RowDefinition());
        m_caption = ref new Grid(); m_caption->Background = Brush(32, 42, 57);
        auto titleColumn = ref new ColumnDefinition(); m_caption->ColumnDefinitions->Append(titleColumn);
        auto controlsColumn = ref new ColumnDefinition(); controlsColumn->Width = GridLengthHelper::Auto;
        m_caption->ColumnDefinitions->Append(controlsColumn);
        auto titleZone = ref new Grid(); titleZone->Background = ref new SolidColorBrush(Colors::Transparent);
        Title = ref new TextBlock(); Title->Foreground = Brush(235, 242, 252);
        Title->FontSize = 13; Thickness titleMargin; titleMargin.Left=12; titleMargin.Right=8; titleMargin.Top=titleMargin.Bottom=0; Title->Margin=titleMargin;
        Title->VerticalAlignment = VerticalAlignment::Center; Title->TextTrimming = TextTrimming::CharacterEllipsis;
        titleZone->Children->Append(Title); m_caption->Children->Append(titleZone);
        auto controls = ref new StackPanel(); controls->Orientation = Orientation::Horizontal; Grid::SetColumn(controls, 1);
        Back = Control(L"\u2190", L"Back"); controls->Children->Append(Back);
        auto minimize = Control(L"\u2014", L"Minimize"); controls->Children->Append(minimize);
        m_maximize = Control(L"\u25a1", L"Maximize or restore"); controls->Children->Append(m_maximize);
        auto fullscreen = Control(L"\u26f6", L"Fullscreen (F11)"); controls->Children->Append(fullscreen);
        auto close = Control(L"\u00d7", L"Close program"); controls->Children->Append(close);
        m_caption->Children->Append(controls); content->Children->Append(m_caption);
        Surface = ref new Grid(); Surface->Background = Brush(15, 20, 29); Grid::SetRow(Surface, 1);
        content->Children->Append(Surface); m_frame->Children->Append(m_border);
        m_grip = ref new Border(); m_grip->Width = 18; m_grip->Height = 18;
        m_grip->HorizontalAlignment = HorizontalAlignment::Right; m_grip->VerticalAlignment = VerticalAlignment::Bottom;
        m_grip->Background = Brush(75, 98, 130);
        auto gripMark = ref new TextBlock(); gripMark->Text = L"\u25e2"; gripMark->Foreground = Brush(220, 235, 255);
        gripMark->FontSize = 12; gripMark->IsHitTestVisible = false; m_grip->Child = gripMark;
        m_frame->Children->Append(m_grip);
        m_leaveFullscreen = ref new Button(); m_leaveFullscreen->Content = L"Exit fullscreen (F11)";
        m_leaveFullscreen->HorizontalAlignment = HorizontalAlignment::Right;
        m_leaveFullscreen->VerticalAlignment = VerticalAlignment::Top;
        m_leaveFullscreen->Margin = ThicknessHelper::FromUniformLength(8);
        m_leaveFullscreen->Visibility = Visibility::Collapsed;
        m_frame->Children->Append(m_leaveFullscreen); m_layer->Children->Append(m_frame);
        m_restore = ref new Button(); m_restore->Content = L"Restore program";
        m_restore->Height = 38; m_restore->MaxWidth = 240;
        m_restore->Background = Brush(35, 63, 101); m_restore->Foreground = Brush(240, 246, 255);
        m_restore->Visibility = Visibility::Collapsed; m_layer->Children->Append(m_restore);
        const std::weak_ptr<HostedWindow> weak = shared_from_this();
        close->Click += ref new RoutedEventHandler([weak](Object^, RoutedEventArgs^) {
            if (auto self = weak.lock()) if (self->m_close) self->m_close();
        });
        minimize->Click += ref new RoutedEventHandler([weak](Object^, RoutedEventArgs^) {
            if (auto self = weak.lock()) self->Minimize();
        });
        m_restore->Click += ref new RoutedEventHandler([weak](Object^, RoutedEventArgs^) {
            if (auto self = weak.lock()) self->Restore();
        });
        m_maximize->Click += ref new RoutedEventHandler([weak](Object^, RoutedEventArgs^) {
            if (auto self = weak.lock()) self->ToggleMaximize();
        });
        auto toggleFull = ref new RoutedEventHandler([weak](Object^, RoutedEventArgs^) {
            if (auto self = weak.lock()) self->ToggleFullscreen();
        });
        fullscreen->Click += toggleFull; m_leaveFullscreen->Click += toggleFull;
        titleZone->DoubleTapped += ref new DoubleTappedEventHandler([weak](Object^, DoubleTappedRoutedEventArgs^ e) {
            if (auto self = weak.lock()) { self->ToggleMaximize(); e->Handled = true; }
        });
        BindDrag(titleZone, false); BindDrag(m_grip, true);
        parent->SizeChanged += ref new SizeChangedEventHandler([weak](Object^, SizeChangedEventArgs^ e) {
            if (auto self = weak.lock()) { self->m_width = (std::max)(1.0, static_cast<double>(e->NewSize.Width));
                self->m_height = (std::max)(1.0, static_cast<double>(e->NewSize.Height)); self->Layout(); }
        });
        // Registered before a guest session subscribes: desktop/chrome input
        // cannot leak into the guest's global CoreWindow handlers.
        Window::Current->CoreWindow->PointerPressed += ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(
            [weak](CoreWindow^, PointerEventArgs^ e) {
                if (auto self = weak.lock()) {
                    const auto p = e->CurrentPoint->Position; const auto b = self->m_bounds;
                    const double top = b.Y + (self->m_mode == Mode::Fullscreen ? 0 : 36);
                    const bool exitControl = self->m_mode == Mode::Fullscreen && p.X > b.X+b.Width-220 && p.Y < b.Y+52;
                    const bool grip = self->m_mode == Mode::Windowed && p.X > b.X+b.Width-20 && p.Y > b.Y+b.Height-20;
                    self->Active(!exitControl && !grip && p.X >= b.X && p.X < b.X + b.Width && p.Y >= top && p.Y < b.Y + b.Height);
                }
            });
        Window::Current->CoreWindow->KeyDown += ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
            [weak](CoreWindow^, KeyEventArgs^ e) {
                if (auto self = weak.lock()) {
                    if (!self->m_open || self->m_minimized || !self->m_active) return;
                    if (e->VirtualKey == Windows::System::VirtualKey::F11 ||
                        (e->VirtualKey == Windows::System::VirtualKey::Escape && self->m_mode == Mode::Fullscreen)) {
                        self->ToggleFullscreen(); e->Handled = true;
                    }
                }
            });
        m_width = (std::max)(1.0, parent->ActualWidth); m_height = (std::max)(1.0, parent->ActualHeight);
        if (m_width <= 1 || m_height <= 1) { const auto b = Window::Current->Bounds; m_width = b.Width; m_height = b.Height; }
        Layout();
    }
    void BindDrag(UIElement^ zone, bool resize) {
        const std::weak_ptr<HostedWindow> weak = shared_from_this();
        zone->PointerPressed += ref new PointerEventHandler([weak, zone, resize](Object^, PointerRoutedEventArgs^ e) {
            if (auto self = weak.lock()) {
                if (self->m_mode != Mode::Windowed || !e->GetCurrentPoint(zone)->Properties->IsLeftButtonPressed) return;
                self->Active(false); self->m_pointerStart = e->GetCurrentPoint(self->m_layer)->Position;
                self->m_dragStart = self->m_normal;
                self->m_dragging = !resize; self->m_resizing = resize;
                if (!zone->CapturePointer(e->Pointer)) { self->m_dragging = self->m_resizing = false; return; }
                e->Handled = true;
            }
        });
        zone->PointerMoved += ref new PointerEventHandler([weak](Object^, PointerRoutedEventArgs^ e) {
            if (auto self = weak.lock()) {
                if (!self->m_dragging && !self->m_resizing) return;
                const auto p = e->GetCurrentPoint(self->m_layer)->Position;
                const auto dx = p.X - self->m_pointerStart.X, dy = p.Y - self->m_pointerStart.Y;
                self->m_normal = self->m_dragStart;
                if (self->m_resizing) { self->m_normal.Width += dx; self->m_normal.Height += dy; }
                else { self->m_normal.X += dx; self->m_normal.Y += dy; }
                self->Layout(); e->Handled = true;
            }
        });
        zone->PointerReleased += ref new PointerEventHandler([weak, zone](Object^, PointerRoutedEventArgs^ e) {
            if (auto self = weak.lock()) {
                self->m_dragging = self->m_resizing = false; zone->ReleasePointerCapture(e->Pointer);
            }
        });
        zone->PointerCaptureLost += ref new PointerEventHandler([weak](Object^, PointerRoutedEventArgs^) {
            if (auto self = weak.lock()) self->m_dragging = self->m_resizing = false;
        });
    }
public:
    Grid^ Surface = nullptr;
    TextBlock^ Title = nullptr;
    Button^ Back = nullptr;
    static std::shared_ptr<HostedWindow> Create(Grid^ parent, std::function<void()> close, std::function<void(bool)> input) {
        auto result = std::make_shared<HostedWindow>(); result->m_close = std::move(close); result->m_input = std::move(input);
        result->Init(parent); return result;
    }
    Grid^ Frame() const { return m_frame; }
    bool InputActive() const { return m_active && m_open && !m_minimized; }
    Windows::Data::Json::JsonObject^ Snapshot() const {
        using namespace Windows::Data::Json;
        auto j=ref new JsonObject();
        j->SetNamedValue(L"mode", JsonValue::CreateStringValue(ref new String(m_mode==Mode::Fullscreen ? L"fullscreen" : m_mode==Mode::Maximized ? L"maximized" : L"windowed")));
        j->SetNamedValue(L"minimized", JsonValue::CreateBooleanValue(m_minimized));
        j->SetNamedValue(L"inputActive", JsonValue::CreateBooleanValue(InputActive()));
        j->SetNamedValue(L"visible", JsonValue::CreateBooleanValue(m_frame->Visibility==Visibility::Visible));
        j->SetNamedValue(L"width", JsonValue::CreateNumberValue(m_frame->ActualWidth));
        j->SetNamedValue(L"height", JsonValue::CreateNumberValue(m_frame->ActualHeight));
        j->SetNamedValue(L"surfaceWidth", JsonValue::CreateNumberValue(Surface->ActualWidth));
        j->SetNamedValue(L"surfaceHeight", JsonValue::CreateNumberValue(Surface->ActualHeight));
        return j;
    }
    void Open(String^ title, bool browser) {
        m_open = true; m_minimized = false; m_mode = Mode::Windowed;
        m_normal = Bounds(static_cast<float>((std::max)(0.0, (m_width - 1100) / 2)), 48,
            static_cast<float>((std::min)(1100.0, m_width - 48)), static_cast<float>((std::min)(740.0, m_height - 120)));
        Title->Text = title; Back->Visibility = browser ? Visibility::Visible : Visibility::Collapsed;
        m_restore->Visibility = Visibility::Collapsed; m_frame->Visibility = Visibility::Visible; Layout(); Active(true);
    }
    void Hide() {
        m_open = false; m_minimized = false; m_frame->Visibility = Visibility::Collapsed;
        m_restore->Visibility = Visibility::Collapsed; Active(false);
    }
    void Minimize() {
        if (!m_open) return;
        m_minimized = true; m_frame->Visibility = Visibility::Collapsed;
        m_restore->Content = Title->Text; ToolTipService::SetToolTip(m_restore, L"Restore program");
        m_restore->Visibility = Visibility::Visible; Active(false);
    }
    void Restore() {
        if (!m_open) return;
        m_minimized = false; m_frame->Visibility = Visibility::Visible;
        m_restore->Visibility = Visibility::Collapsed; Layout(); Active(true);
    }
    void ToggleMaximize() {
        if (m_mode == Mode::Fullscreen) m_mode = m_beforeFullscreen;
        m_mode = m_mode == Mode::Maximized ? Mode::Windowed : Mode::Maximized; Layout(); Active(true);
    }
    void ToggleFullscreen() {
        if (m_mode == Mode::Fullscreen) m_mode = m_beforeFullscreen;
        else { m_beforeFullscreen = m_mode; m_mode = Mode::Fullscreen; }
        Layout(); Active(true);
    }
};
}
