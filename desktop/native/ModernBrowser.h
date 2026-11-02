// SPDX-License-Identifier: MIT
// Edge/Chromium browser using Microsoft's supported UWP WebView2 control.
#pragma once
#include <ppltasks.h>
#include <memory>
#include <vector>
#include <string>

namespace MihawkModern {
using namespace concurrency;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using ModernView = Microsoft::UI::Xaml::Controls::WebView2;
using namespace Microsoft::Web::WebView2::Core;

inline void InitializeResources() {
    static bool initialized=false;
    if (!initialized) {
        Application::Current->Resources->MergedDictionaries->Append(ref new Microsoft::UI::Xaml::Controls::XamlControlsResources());
        initialized=true;
    }
}

class Browser : public std::enable_shared_from_this<Browser> {
    struct Tab {
        ModernView^ view = nullptr;
        Button^ button = nullptr;
        Uri^ requested = nullptr;
        bool ready = false;
        EventRegistrationToken clickToken{}, startingToken{}, completedToken{}, newWindowToken{}, externalToken{};
    };
    Panel^ m_parent = nullptr;
    Grid^ m_root = nullptr;
    Grid^ m_pages = nullptr;
    StackPanel^ m_tabs = nullptr;
    TextBox^ m_address = nullptr;
    TextBlock^ m_message = nullptr;
    TextBlock^ m_title = nullptr;
    std::vector<std::shared_ptr<Tab>> m_items;
    std::shared_ptr<Tab> m_active;
    bool m_closed = false;
    task<void> m_statusTail = task_from_result();
    static task_continuation_context UI() { return task_continuation_context::use_current(); }
    static void Observe(task<void> operation) { operation.then([](task<void> done) { try { done.get(); } catch (...) {} }); }
    static bool Http(Uri^ uri) {
        return uri && (uri->SchemeName == L"http" || uri->SchemeName == L"https");
    }
    static Button^ ButtonFor(String^ text, RoutedEventHandler^ handler) {
        auto button = ref new Button(); button->Content = text;
        button->Margin = ThicknessHelper::FromUniformLength(3); button->Click += handler;
        return button;
    }
    void Status(String^ state, String^ error = nullptr, String^ version = nullptr) {
        auto record = ref new JsonObject();
        record->SetNamedValue(L"engine", JsonValue::CreateStringValue(L"WebView2 Edge/Chromium"));
        record->SetNamedValue(L"state", JsonValue::CreateStringValue(state));
        record->SetNamedValue(L"error", JsonValue::CreateStringValue(error ? error : L""));
        record->SetNamedValue(L"version", JsonValue::CreateStringValue(version ? version : L""));
        record->SetNamedValue(L"url", JsonValue::CreateStringValue(m_address ? m_address->Text : L""));
        auto text = record->Stringify(); auto folder = ApplicationData::Current->LocalFolder;
        m_statusTail = m_statusTail.then([folder, text](task<void> prior) {
            try { prior.get(); } catch (...) {}
            return create_task(folder->CreateFileAsync(L"mihawk-browser-status.json", CreationCollisionOption::ReplaceExisting))
                .then([text](StorageFile^ file) { return FileIO::WriteTextAsync(file, text); });
        }, UI());
        Observe(m_statusTail);
    }
    void Select(const std::shared_ptr<Tab>& tab) {
        if (m_closed) return;
        m_active = tab;
        for (auto& item : m_items) item->view->Visibility = item == tab ? Visibility::Visible : Visibility::Collapsed;
        if (tab->view->Source) m_address->Text = tab->view->Source->AbsoluteUri;
        else if (tab->requested) m_address->Text = tab->requested->AbsoluteUri;
        if (tab->ready) tab->view->Focus(FocusState::Programmatic);
    }
    void Navigate() {
        if (!m_active || !m_active->ready) return;
        try {
            auto value = m_address->Text;
            std::wstring text(value->Data(), value->Length());
            if (text.find(L"://") == std::wstring::npos) value = L"https://" + value;
            auto uri = ref new Uri(value);
            if (!Http(uri)) throw ref new InvalidArgumentException(L"Use an HTTP or HTTPS address.");
            m_active->view->Source = uri;
        } catch (Exception^ error) { m_message->Text = error->Message; }
    }
    void ReleaseTab(const std::shared_ptr<Tab>& tab) {
        try { tab->button->Click -= tab->clickToken; } catch (...) {}
        try { tab->view->NavigationStarting -= tab->startingToken; } catch (...) {}
        try { tab->view->NavigationCompleted -= tab->completedToken; } catch (...) {}
        if (tab->ready) {
            try { tab->view->CoreWebView2->NewWindowRequested -= tab->newWindowToken; } catch (...) {}
            try { tab->view->CoreWebView2->LaunchingExternalUriScheme -= tab->externalToken; } catch (...) {}
        }
        try { tab->view->Close(); } catch (...) {}
    }
    void CloseTab() {
        if (!m_active || m_items.size() <= 1) return;
        auto selected = m_active;
        unsigned index=0;
        if (m_pages->Children->IndexOf(selected->view, &index)) m_pages->Children->RemoveAt(index);
        if (m_tabs->Children->IndexOf(selected->button, &index)) m_tabs->Children->RemoveAt(index);
        ReleaseTab(selected);
        for (auto it=m_items.begin(); it!=m_items.end(); ++it) if (*it == selected) { m_items.erase(it); break; }
        Select(m_items.back());
    }
    task<JsonObject^> AddTab(Uri^ uri) {
        if (m_items.size() >= 8) throw ref new InvalidArgumentException(L"Close a tab before opening another (maximum 8).");
        auto self=shared_from_this(); auto tab=std::make_shared<Tab>();
        tab->view=ref new ModernView(); tab->requested=uri;
        tab->button=ref new Button(); tab->button->Content=L"New tab";
        tab->button->Margin=ThicknessHelper::FromUniformLength(3);
        tab->clickToken = tab->button->Click += ref new RoutedEventHandler([self,tab](Object^,RoutedEventArgs^) { self->Select(tab); });
        m_tabs->Children->Append(tab->button); m_items.push_back(tab);
        m_pages->Children->Append(tab->view); Select(tab);
        tab->startingToken = tab->view->NavigationStarting += ref new TypedEventHandler<ModernView^,CoreWebView2NavigationStartingEventArgs^>(
            [self,tab](ModernView^,CoreWebView2NavigationStartingEventArgs^ args) {
                try { if (!Http(ref new Uri(args->Uri))) { args->Cancel=true; return; } }
                catch (...) { args->Cancel=true; return; }
                if (self->m_active==tab) { self->m_address->Text=args->Uri; self->m_message->Text=L"Loading..."; }
                self->Status(L"loading");
            });
        tab->completedToken = tab->view->NavigationCompleted += ref new TypedEventHandler<ModernView^,CoreWebView2NavigationCompletedEventArgs^>(
            [self,tab](ModernView^ sender,CoreWebView2NavigationCompletedEventArgs^ args) {
                if (self->m_closed || !tab->ready) return;
                auto core=sender->CoreWebView2;
                tab->button->Content=core->DocumentTitle->Length() ? core->DocumentTitle : L"Browser";
                if (self->m_active==tab) self->m_message->Text=ref new String(args->IsSuccess ? L"Edge / Chromium" : L"Navigation failed.");
                self->Status(ref new String(args->IsSuccess ? L"loaded" : L"navigation-failed"), args->IsSuccess ? nullptr : ref new String(L"The page could not be loaded."), core->Environment->BrowserVersionString);
            });
        return create_task(tab->view->EnsureCoreWebView2Async()).then([self,tab,uri](task<void> initialized) {
            try {
                initialized.get();
                if (self->m_closed) throw ref new OperationCanceledException(L"Browser was closed.");
                auto core=tab->view->CoreWebView2;
                if (!core) throw ref new FailureException(L"WebView2 did not provide an engine instance.");
                tab->ready=true;
                core->Settings->IsWebMessageEnabled=false;
                core->Settings->IsGeneralAutofillEnabled=false;
                core->Settings->IsPasswordAutosaveEnabled=false;
                core->Settings->AreDevToolsEnabled=false;
                // Open web pages receive no application objects, RPC or filesystem access.
                tab->newWindowToken = core->NewWindowRequested += ref new TypedEventHandler<CoreWebView2^,CoreWebView2NewWindowRequestedEventArgs^>(
                    [self](CoreWebView2^,CoreWebView2NewWindowRequestedEventArgs^ args) {
                        args->Handled=true;
                        try {
                            auto target=ref new Uri(args->Uri);
                            if (Http(target)) Observe(self->AddTab(target).then([](JsonObject^){}));
                        } catch (Exception^ error) { self->m_message->Text=error->Message; }
                    });
                tab->externalToken = core->LaunchingExternalUriScheme += ref new TypedEventHandler<CoreWebView2^,CoreWebView2LaunchingExternalUriSchemeEventArgs^>(
                    [](CoreWebView2^,CoreWebView2LaunchingExternalUriSchemeEventArgs^ args) { args->Cancel=true; });
                self->Status(L"initialized",nullptr,core->Environment->BrowserVersionString);
                tab->view->Source=uri; self->Select(tab);
                auto result=ref new JsonObject();
                result->SetNamedValue(L"opened",JsonValue::CreateBooleanValue(true));
                result->SetNamedValue(L"engine",JsonValue::CreateStringValue(L"WebView2 Edge/Chromium"));
                result->SetNamedValue(L"version",JsonValue::CreateStringValue(core->Environment->BrowserVersionString));
                return result;
            } catch (Exception^ error) {
                if (!self->m_closed) { self->m_message->Text=L"WebView2: "+error->Message; self->Status(L"failed",error->Message); }
                throw;
            }
        },UI());
    }
public:
    task<JsonObject^> OpenAsync(Panel^ surface,Uri^ uri,TextBlock^ title) {
        if (!Http(uri)) throw ref new InvalidArgumentException(L"The browser accepts HTTP and HTTPS addresses.");
        m_closed=false; m_parent=surface; m_title=title;
        auto self=shared_from_this();
        InitializeResources();
        m_title->Text=L"Edge / Chromium Browser";
        m_root=ref new Grid();
        for (unsigned i=0;i<4;++i) { auto row=ref new RowDefinition(); if (i!=2) row->Height=GridLengthHelper::Auto; m_root->RowDefinitions->Append(row); }
        m_tabs=ref new StackPanel();m_tabs->Orientation=Orientation::Horizontal;
        m_root->Children->Append(m_tabs);
        auto toolbar=ref new Grid();Grid::SetRow(toolbar,1);
        for (unsigned i=0;i<6;++i) { auto col=ref new ColumnDefinition(); if(i!=3)col->Width=GridLengthHelper::Auto;toolbar->ColumnDefinitions->Append(col); }
        auto addControl=[toolbar](FrameworkElement^ control,int column){Grid::SetColumn(control,column);toolbar->Children->Append(control);};
        addControl(ButtonFor(L"Back",ref new RoutedEventHandler([self](Object^,RoutedEventArgs^){self->GoBack();})),0);
        addControl(ButtonFor(L"Forward",ref new RoutedEventHandler([self](Object^,RoutedEventArgs^){if(self->m_active&&self->m_active->ready&&self->m_active->view->CanGoForward)self->m_active->view->GoForward();})),1);
        addControl(ButtonFor(L"Reload",ref new RoutedEventHandler([self](Object^,RoutedEventArgs^){if(self->m_active&&self->m_active->ready)self->m_active->view->Reload();})),2);
        m_address=ref new TextBox();m_address->Text=uri->AbsoluteUri;m_address->Margin=ThicknessHelper::FromUniformLength(3);
        m_address->KeyDown+=ref new Windows::UI::Xaml::Input::KeyEventHandler([self](Object^,Windows::UI::Xaml::Input::KeyRoutedEventArgs^ args){if(args->Key==Windows::System::VirtualKey::Enter){args->Handled=true;self->Navigate();}});
        addControl(m_address,3);
        addControl(ButtonFor(L"Go",ref new RoutedEventHandler([self](Object^,RoutedEventArgs^){self->Navigate();})),4);
        addControl(ButtonFor(L"+ Tab",ref new RoutedEventHandler([self](Object^,RoutedEventArgs^){try{Observe(self->AddTab(ref new Uri(L"https://www.bing.com")).then([](JsonObject^){}));}catch(Exception^ e){self->m_message->Text=e->Message;}})),5);
        m_root->Children->Append(toolbar);
        m_pages=ref new Grid();Grid::SetRow(m_pages,2);m_root->Children->Append(m_pages);
        auto footer=ref new StackPanel();footer->Orientation=Orientation::Horizontal;Grid::SetRow(footer,3);
        footer->Children->Append(ButtonFor(L"Close tab",ref new RoutedEventHandler([self](Object^,RoutedEventArgs^){self->CloseTab();})));
        m_message=ref new TextBlock();m_message->Text=L"Starting Edge / Chromium...";m_message->Foreground=ref new SolidColorBrush(Windows::UI::Colors::White);m_message->VerticalAlignment=VerticalAlignment::Center;footer->Children->Append(m_message);m_root->Children->Append(footer);
        surface->Children->Append(m_root);Status(L"initializing");return AddTab(uri);
    }
    bool CanGoBack(){return m_active&&m_active->ready&&m_active->view->CanGoBack;}
    void GoBack(){if(CanGoBack())m_active->view->GoBack();}
    void Close(){
        m_closed=true;
        for(auto& tab:m_items) ReleaseTab(tab);
        m_items.clear();m_active.reset();
        if(m_pages)m_pages->Children->Clear();
        if(m_tabs)m_tabs->Children->Clear();
        if(m_root)m_root->Children->Clear();
        Status(L"closed");
        m_root=nullptr;m_pages=nullptr;m_tabs=nullptr;m_address=nullptr;m_message=nullptr;m_title=nullptr;m_parent=nullptr;
    }
};
}
