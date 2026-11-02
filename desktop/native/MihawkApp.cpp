// Native Xbox UWP host for the packaged Mihawk desktop. No desktop process APIs.
#include <windows.h>
#include <roapi.h>
#include <ppltasks.h>
#include <collection.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace concurrency;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Security::Cryptography;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::Pickers;
using namespace Windows::Storage::Streams;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;

namespace {
constexpr unsigned ReadLimit = 1024U * 1024U;
constexpr unsigned WriteLimit = 256U * 1024U;
constexpr unsigned PageSize = 128;
constexpr wchar_t UsbGrant[] = L"MihawkUsbRoot";

String^ Str(const std::wstring& value) { return ref new String(value.c_str()); }
std::wstring Wide(String^ value) { return value ? std::wstring(value->Data(), value->Length()) : std::wstring(); }
task_continuation_context Ui() { return task_continuation_context::use_current(); }
[[noreturn]] void Fail(String^ message) { throw ref new InvalidArgumentException(message); }
void Text(JsonObject^ object, String^ key, String^ value) { object->SetNamedValue(key, JsonValue::CreateStringValue(value ? value : L"")); }
void Bool(JsonObject^ object, String^ key, bool value) { object->SetNamedValue(key, JsonValue::CreateBooleanValue(value)); }
void Number(JsonObject^ object, String^ key, double value) { object->SetNamedValue(key, JsonValue::CreateNumberValue(value)); }
String^ Required(JsonObject^ object, String^ key, unsigned limit = 128) {
    auto value = object->GetNamedString(key);
    if (!value || value->Length() > limit) Fail(L"A request string exceeds its supported limit.");
    return value;
}
void Basename(String^ name) {
    const auto text = Wide(name);
    if (text.empty() || text.size() > 120 || text == L"." || text == L".." ||
        text.back() == L'.' || text.back() == L' ') Fail(L"Enter a single valid file or folder name.");
    for (wchar_t c : text)
        if (c < 32 || std::wstring(L"\\/:*?\"<>|").find(c) != std::wstring::npos)
            Fail(L"File and folder names cannot contain paths or reserved characters.");
}
bool Trusted(Uri^ uri) {
    return uri && Wide(uri->SchemeName) == L"ms-appx-web" &&
        Wide(uri->Host).empty() && Wide(uri->Path) == L"/Mihawk/index.html";
}
bool WebAddress(Uri^ uri) {
    const auto scheme = uri ? Wide(uri->SchemeName) : std::wstring();
    return (scheme == L"https" || scheme == L"http") && !Wide(uri->Host).empty();
}
struct Item {
    IStorageItem^ value = nullptr;
    StorageFolder^ parent = nullptr;
    bool writable = false;
};
struct State {
    Grid^ root = nullptr;
    WebView^ shell = nullptr;
    Grid^ overlay = nullptr;
    Grid^ surface = nullptr;
    WebView^ browser = nullptr;
    TextBlock^ overlayTitle = nullptr;
    TextBlock^ fallback = nullptr;
    Button^ back = nullptr;
    Win32Bridge::RuntimeSession^ session = nullptr;
    std::map<std::wstring, Item> items;
    std::wstring usbId;
    unsigned nextId = 1, pending = 0;
    bool javascriptReady = false;
    String^ userAgent = L"";
    String^ navigation = L"starting";
    String^ lastError = L"";
    task<void> initialized = task_from_result();
    task<void> statusTail = task_from_result();
};

task<void> Status(const std::shared_ptr<State>& state) {
    auto status = ref new JsonObject();
    Bool(status, L"nativeStarted", true);
    Bool(status, L"javascriptReady", state->javascriptReady);
    Text(status, L"platform", L"Xbox UWP");
    Text(status, L"host", L"MihawkDesktop");
    Text(status, L"navigation", state->navigation);
    Text(status, L"userAgent", state->userAgent);
    Text(status, L"lastError", state->lastError);
    const auto snapshot = status->Stringify();
    const auto local = ApplicationData::Current->LocalFolder;
    state->statusTail = state->statusTail.then([local, snapshot](task<void> previous) {
        try { previous.get(); } catch (...) { }
        return create_task(local->CreateFileAsync(L"mihawk-desktop-status.json", CreationCollisionOption::ReplaceExisting))
            .then([snapshot](StorageFile^ file) { return FileIO::WriteTextAsync(file, snapshot); });
    }, Ui());
    return state->statusTail;
}
void Observe(task<void> operation) {
    operation.then([](task<void> done) { try { done.get(); } catch (...) { } });
}
void HostError(const std::shared_ptr<State>& state, String^ error) {
    state->lastError = error ? error : L"Native host error.";
    if (state->fallback) {
        state->fallback->Text = state->lastError;
        state->fallback->Visibility = Visibility::Visible;
    }
    Observe(Status(state));
}
Item Find(const std::shared_ptr<State>& state, String^ id) {
    auto found = state->items.find(Wide(id));
    if (found == state->items.end()) Fail(L"Unknown or expired item token. Reopen its folder.");
    return found->second;
}
StorageFolder^ Folder(const Item& item) {
    auto folder = dynamic_cast<StorageFolder^>(item.value);
    if (!folder) Fail(L"This item is not a folder.");
    return folder;
}
StorageFile^ File(const Item& item) {
    auto file = dynamic_cast<StorageFile^>(item.value);
    if (!file) Fail(L"This item is not a file.");
    return file;
}
String^ Token(const std::shared_ptr<State>& state, IStorageItem^ item, StorageFolder^ parent, bool writable) {
    const auto path = Wide(item->Path);
    if (!path.empty())
        for (const auto& pair : state->items)
            if (pair.second.writable == writable && Wide(pair.second.value->Path) == path) return Str(pair.first);
    if (state->items.size() >= 8192) Fail(L"Too many open item tokens. Restart the desktop to refresh them.");
    const auto id = L"item-" + std::to_wstring(state->nextId++);
    state->items.emplace(id, Item{item, parent, writable});
    return Str(id);
}
JsonObject^ Entry(const std::shared_ptr<State>& state, String^ id) {
    const auto item = Find(state, id);
    auto result = ref new JsonObject();
    Text(result, L"id", id);
    Text(result, L"name", Wide(id) == L"workspace" ? L"Files" : item.value->Name);
    Text(result, L"kind", Str(dynamic_cast<StorageFolder^>(item.value) ? L"folder" : L"file"));
    Bool(result, L"writable", item.writable);
    return result;
}
void Mount(const std::shared_ptr<State>& state, StorageFolder^ folder) {
    // A grant only enables access to its selected folder. All external mounts
    // remain read-only even if the underlying storage provider permits writes.
    state->usbId = Wide(Token(state, folder, nullptr, false));
}
task<void> Restore(const std::shared_ptr<State>& state, unsigned index = 0) {
    static const wchar_t* keys[] = {UsbGrant, L"CoreShell.Files.Mount.Removable media", L"GamePreflightFolder"};
    if (index >= sizeof(keys) / sizeof(keys[0])) return task_from_result();
    auto access = StorageApplicationPermissions::FutureAccessList;
    auto key = ref new String(keys[index]);
    if (!access->ContainsItem(key)) return Restore(state, index + 1);
    return create_task(access->GetFolderAsync(key)).then([state, index](task<StorageFolder^> pending) -> task<void> {
        try {
            auto folder = pending.get();
            const auto path = Wide(folder->Path), local = Wide(ApplicationData::Current->LocalFolder->Path);
            if (!path.empty() && (local.empty() || path.compare(0, local.size(), local) != 0)) {
                Mount(state, folder);
                return task_from_result();
            }
        } catch (...) { }
        return Restore(state, index + 1);
    }, Ui());
}
task<void> WriteBuffer(StorageFile^ file, IBuffer^ buffer) {
    // A transaction preserves the existing file if writing fails. The reply
    // waits for Store, Flush and Commit, never just an enqueued write.
    return create_task(file->OpenTransactedWriteAsync()).then([buffer](StorageStreamTransaction^ transaction) {
        transaction->Stream->Size = 0;
        auto writer = ref new DataWriter(transaction->Stream->GetOutputStreamAt(0));
        writer->WriteBuffer(buffer);
        return create_task(writer->StoreAsync()).then([writer](unsigned) { return writer->FlushAsync(); })
            .then([transaction, writer](bool flushed) {
                if (!flushed) Fail(L"The file could not be flushed.");
                writer->DetachStream();
                return transaction->CommitAsync();
            }).then([transaction, writer](task<void> done) {
                try { done.get(); }
                catch (...) { try { delete transaction; } catch (...) { } throw; }
                delete transaction;
            });
    });
}
IBuffer^ TextBuffer(JsonObject^ parameters) {
    auto text = Required(parameters, L"text", WriteLimit);
    auto buffer = CryptographicBuffer::ConvertStringToBinary(text, BinaryStringEncoding::Utf8);
    if (buffer->Length > WriteLimit) Fail(L"Text writes are limited to 256 KiB of UTF-8 data.");
    return buffer;
}
void CloseOverlay(const std::shared_ptr<State>& state) {
    if (state->session) {
        state->session->SetInputEnabled(false);
        state->session->Close();
        state->session = nullptr;
    }
    if (state->browser) { state->browser->Stop(); state->browser = nullptr; }
    state->surface->Children->Clear();
    state->overlay->Visibility = Visibility::Collapsed;
}
void OpenOverlay(const std::shared_ptr<State>& state, String^ title, bool browser) {
    CloseOverlay(state);
    state->overlayTitle->Text = title;
    state->back->Visibility = browser ? Visibility::Visible : Visibility::Collapsed;
    state->overlay->Visibility = Visibility::Visible;
}
task<JsonObject^> Dispatch(const std::shared_ptr<State>& state, String^ method, JsonObject^ parameters) {
    const auto name = Wide(method);
    if (name == L"ready") {
        state->userAgent = parameters->HasKey(L"userAgent") ? Required(parameters, L"userAgent", 1024) : L"";
        state->javascriptReady = true;
        state->navigation = L"ready";
        state->lastError = L"";
        state->fallback->Visibility = Visibility::Collapsed;
        return Status(state).then([] { return ref new JsonObject(); }, Ui());
    }
    if (name == L"hello") {
        return state->initialized.then([state](task<void> initialized) {
            initialized.get();
            auto result = ref new JsonObject();
            Text(result, L"platform", L"Xbox UWP"); Text(result, L"version", L"Mihawk native host 1");
            Text(result, L"userAgent", state->userAgent);
            auto roots = ref new JsonArray(); roots->Append(Entry(state, L"workspace"));
            if (!state->usbId.empty()) roots->Append(Entry(state, Str(state->usbId)));
            result->SetNamedValue(L"roots", roots);
            return result;
        }, Ui());
    }
    if (name == L"list") {
        const auto item = Find(state, Required(parameters, L"folder"));
        auto folder = Folder(item);
        const double requested = parameters->HasKey(L"offset") ? parameters->GetNamedNumber(L"offset") : 0;
        if (!std::isfinite(requested) || requested < 0 || requested > 1000000 || std::floor(requested) != requested)
            Fail(L"Invalid folder page offset.");
        const auto offset = static_cast<unsigned>(requested);
        return create_task(folder->GetItemsAsync(offset, PageSize)).then([state, folder, item, offset](IVectorView<IStorageItem^>^ entries) {
            auto result = ref new JsonObject(); auto list = ref new JsonArray();
            for (unsigned index = 0; index < entries->Size; ++index)
                list->Append(Entry(state, Token(state, entries->GetAt(index), folder, item.writable)));
            result->SetNamedValue(L"entries", list);
            Number(result, L"nextOffset", entries->Size == PageSize ? static_cast<double>(offset + PageSize) : -1);
            return result;
        }, Ui());
    }
    if (name == L"read") {
        auto file = File(Find(state, Required(parameters, L"file")));
        return create_task(file->OpenReadAsync()).then([](IRandomAccessStreamWithContentType^ stream) {
            if (stream->Size > ReadLimit) { delete stream; Fail(L"Text reads are limited to 1 MiB."); }
            auto reader = ref new DataReader(stream->GetInputStreamAt(0));
            reader->UnicodeEncoding = UnicodeEncoding::Utf8;
            return create_task(reader->LoadAsync(static_cast<unsigned>(stream->Size))).then([reader, stream](unsigned loaded) {
                auto result = ref new JsonObject();
                try { Text(result, L"text", reader->ReadString(loaded)); }
                catch (...) { delete reader; delete stream; throw; }
                delete reader; delete stream;
                return result;
            });
        }, Ui());
    }
    if (name == L"write") {
        auto id = Required(parameters, L"file"); const auto item = Find(state, id);
        if (!item.writable) Fail(L"This folder is read-only. Save a copy in Files.");
        auto file = File(item); auto buffer = TextBuffer(parameters);
        return WriteBuffer(file, buffer).then([state, id] { return Entry(state, id); }, Ui());
    }
    if (name == L"create" || name == L"mkdir") {
        const auto parent = Find(state, Required(parameters, L"folder"));
        if (!parent.writable) Fail(L"This folder is read-only. Create items in Files.");
        auto folder = Folder(parent); auto itemName = Required(parameters, L"name", 120); Basename(itemName);
        if (name == L"mkdir")
            return create_task(folder->CreateFolderAsync(itemName, CreationCollisionOption::FailIfExists))
                .then([state, folder](StorageFolder^ created) { return Entry(state, Token(state, created, folder, true)); }, Ui());
        auto buffer = TextBuffer(parameters);
        return create_task(folder->CreateFileAsync(itemName, CreationCollisionOption::FailIfExists))
            .then([state, folder, buffer](StorageFile^ created) {
                return WriteBuffer(created, buffer).then([state, folder, created] {
                    return Entry(state, Token(state, created, folder, true));
                }, Ui());
            }, Ui());
    }
    if (name == L"pickFolder") {
        auto picker = ref new FolderPicker(); picker->FileTypeFilter->Append(L"*");
        picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
        return create_task(picker->PickSingleFolderAsync()).then([state](StorageFolder^ selected) {
            if (!selected) Fail(L"No folder was selected.");
            StorageApplicationPermissions::FutureAccessList->AddOrReplace(ref new String(UsbGrant), selected);
            Mount(state, selected);
            return Entry(state, Str(state->usbId));
        }, Ui());
    }
    if (name == L"browser") {
        auto uri = ref new Uri(Required(parameters, L"url", 4096));
        if (!WebAddress(uri)) Fail(L"The browser accepts only HTTP and HTTPS addresses.");
        OpenOverlay(state, L"Browser", true);
        auto browser = ref new WebView(); state->browser = browser;
        // This WebView has no ScriptNotify handler or injected native object.
        browser->NavigationStarting += ref new TypedEventHandler<WebView^, WebViewNavigationStartingEventArgs^>(
            [](WebView^, WebViewNavigationStartingEventArgs^ args) {
                if (!WebAddress(args->Uri)) args->Cancel = true;
            });
        browser->NewWindowRequested += ref new TypedEventHandler<WebView^, WebViewNewWindowRequestedEventArgs^>(
            [](WebView^ sender, WebViewNewWindowRequestedEventArgs^ args) {
                args->Handled = true;
                if (WebAddress(args->Uri)) sender->Navigate(args->Uri);
            });
        state->surface->Children->Append(browser); browser->Navigate(uri);
        auto result = ref new JsonObject(); Bool(result, L"opened", true); return task_from_result(result);
    }
    if (name == L"closeRuntime") {
        CloseOverlay(state); return task_from_result(ref new JsonObject());
    }
    if (name == L"run") {
        const auto item = Find(state, Required(parameters, L"file")); auto file = File(item);
        if (!item.parent) Fail(L"Open the executable from its containing folder first.");
        OpenOverlay(state, L"Preparing " + file->Name, false);
        auto session = ref new Win32Bridge::RuntimeSession(state->surface); state->session = session;
        return create_task(session->PrepareAsync(file, item.parent)).then([state, session, file](bool prepared) {
            if (state->session != session) Fail(L"Execution was canceled before startup.");
            if (!prepared) {
                state->overlayTitle->Text = L"Stopped: " + session->LastError;
                Fail(session->LastError);
            }
            session->SetInputEnabled(true);
            state->overlayTitle->Text = L"Running " + file->Name;
            auto running = create_task(session->RunAsync());
            running.then([state, session, file](task<int> finished) {
                try {
                    const auto exit = finished.get();
                    if (state->session == session)
                        state->overlayTitle->Text = session->LastError && session->LastError->Length()
                            ? L"Stopped: " + session->LastError : file->Name + L" exited " + Str(std::to_wstring(exit));
                    session->FlushDiagnostics();
                } catch (Exception^ error) {
                    if (state->session == session) state->overlayTitle->Text = L"Stopped: " + error->Message;
                } catch (...) {
                    if (state->session == session) state->overlayTitle->Text = L"Runtime stopped unexpectedly.";
                }
            }, Ui());
            auto result = ref new JsonObject(); Bool(result, L"started", true);
            Text(result, L"name", file->Name); Text(result, L"importSummary", session->ImportSummary);
            return result;
        }, Ui());
    }
    Fail(L"This native method is not implemented.");
}
void Reply(const std::shared_ptr<State>& state, String^ id, JsonObject^ result, String^ error) {
    auto response = ref new JsonObject(); Text(response, L"id", id); Bool(response, L"ok", !error);
    if (error) Text(response, L"error", error); else response->SetNamedValue(L"result", result ? result : ref new JsonObject());
    auto arguments = ref new Platform::Collections::Vector<String^>(); arguments->Append(response->Stringify());
    try {
        create_task(state->shell->InvokeScriptAsync(L"MihawkNativeReply", arguments)).then([state](task<String^> sent) {
            try { sent.get(); } catch (Exception^ exception) { HostError(state, L"Desktop reply failed: " + exception->Message); }
            catch (...) { HostError(state, L"Desktop reply failed."); }
        }, Ui());
    } catch (Exception^ exception) { HostError(state, exception->Message); }
}
void Request(const std::shared_ptr<State>& state, Object^ sender, NotifyEventArgs^ args) {
    if (sender != state->shell || !Trusted(state->shell->Source)) return;
    String^ id = L"";
    try {
        if (!args->Value || args->Value->Length() > 2U * 1024U * 1024U) Fail(L"Native request is too large.");
        auto request = JsonObject::Parse(args->Value);
        id = Required(request, L"id"); if (id->Length() == 0) Fail(L"A request ID is required.");
        auto method = Required(request, L"method", 64);
        auto parameters = request->HasKey(L"params") ? request->GetNamedObject(L"params") : ref new JsonObject();
        if (state->pending >= 8) Fail(L"Too many pending native requests. Try again shortly.");
        auto operation = Dispatch(state, method, parameters);
        ++state->pending;
        operation.then([state, id](task<JsonObject^> completed) {
            --state->pending;
            try { Reply(state, id, completed.get(), nullptr); }
            catch (Exception^ exception) { Reply(state, id, nullptr, exception->Message); }
            catch (...) { Reply(state, id, nullptr, L"The native operation failed."); }
        }, Ui());
    } catch (Exception^ exception) { Reply(state, id, nullptr, exception->Message); }
    catch (...) { Reply(state, id, nullptr, L"Invalid native request."); }
}
Button^ Action(String^ title) {
    auto button = ref new Button(); button->Content = title; button->Margin = ThicknessHelper::FromUniformLength(8);
    return button;
}
}

namespace MihawkXbox {
ref class App sealed : public Application {
internal:
    App() { }
protected:
    virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^) override {
        if (Window::Current->Content) { Window::Current->Activate(); return; }
        auto state = std::make_shared<State>(); m_state = state;
        state->root = ref new Grid();
        state->root->Background = ref new SolidColorBrush(Colors::Black);
        Window::Current->Content = state->root; Window::Current->Activate();
        state->fallback = ref new TextBlock(); state->fallback->Text = L"Opening Mihawk desktop...";
        state->fallback->TextWrapping = TextWrapping::Wrap;
        state->fallback->Foreground = ref new SolidColorBrush(Colors::White);
        state->fallback->Margin = ThicknessHelper::FromUniformLength(32);
        state->root->Children->Append(state->fallback);
        Observe(Status(state));
        try {
            state->initialized = create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(
                L"MihawkFiles", CreationCollisionOption::OpenIfExists)).then([state](StorageFolder^ workspace) {
                    state->items.emplace(L"workspace", Item{workspace, nullptr, true});
                    return Restore(state);
                }, Ui());
            state->overlay = ref new Grid(); state->overlay->Visibility = Visibility::Collapsed;
            state->overlay->Background = ref new SolidColorBrush(Colors::Black);
            auto headerRow = ref new RowDefinition(); headerRow->Height = GridLengthHelper::Auto;
            state->overlay->RowDefinitions->Append(headerRow);
            state->overlay->RowDefinitions->Append(ref new RowDefinition());
            auto header = ref new StackPanel(); header->Orientation = Orientation::Horizontal;
            auto close = Action(L"Back to desktop"); state->back = Action(L"Back");
            close->Click += ref new RoutedEventHandler([state](Object^, RoutedEventArgs^) { CloseOverlay(state); });
            state->back->Click += ref new RoutedEventHandler([state](Object^, RoutedEventArgs^) {
                if (state->browser && state->browser->CanGoBack) state->browser->GoBack();
            });
            state->overlayTitle = ref new TextBlock();
            state->overlayTitle->Foreground = ref new SolidColorBrush(Colors::White);
            state->overlayTitle->TextWrapping = TextWrapping::Wrap;
            state->overlayTitle->MaxWidth = 900;
            state->overlayTitle->VerticalAlignment = VerticalAlignment::Center;
            header->Children->Append(close); header->Children->Append(state->back); header->Children->Append(state->overlayTitle);
            state->surface = ref new Grid(); Grid::SetRow(state->surface, 1);
            state->overlay->Children->Append(header); state->overlay->Children->Append(state->surface);
            state->shell = ref new WebView();
            state->shell->NavigationStarting += ref new TypedEventHandler<WebView^, WebViewNavigationStartingEventArgs^>(
                [](WebView^, WebViewNavigationStartingEventArgs^ args) { if (!Trusted(args->Uri)) args->Cancel = true; });
            state->shell->FrameNavigationStarting += ref new TypedEventHandler<WebView^, WebViewNavigationStartingEventArgs^>(
                [](WebView^, WebViewNavigationStartingEventArgs^ args) { args->Cancel = true; });
            state->shell->NewWindowRequested += ref new TypedEventHandler<WebView^, WebViewNewWindowRequestedEventArgs^>(
                [](WebView^, WebViewNewWindowRequestedEventArgs^ args) { args->Handled = true; });
            state->shell->NavigationCompleted += ref new TypedEventHandler<WebView^, WebViewNavigationCompletedEventArgs^>(
                [state](WebView^, WebViewNavigationCompletedEventArgs^ args) {
                    state->navigation = Str(args->IsSuccess ? (state->javascriptReady ? L"ready" : L"loaded") : L"failed");
                    if (!args->IsSuccess) HostError(state, L"The packaged desktop page could not load. Web error " + Str(std::to_wstring(static_cast<int>(args->WebErrorStatus))));
                    else Observe(Status(state));
                });
            state->shell->ScriptNotify += ref new NotifyEventHandler([state](Object^ sender, NotifyEventArgs^ args) { Request(state, sender, args); });
            state->root->Children->InsertAt(0, state->shell);
            state->root->Children->Append(state->overlay);
            state->shell->Navigate(ref new Uri(L"ms-appx-web:///Mihawk/index.html"));
        } catch (Exception^ exception) { HostError(state, L"Native desktop initialization failed: " + exception->Message); }
        catch (...) { HostError(state, L"Native desktop initialization failed."); }
    }
private:
    std::shared_ptr<State> m_state;
};
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initialized)) return static_cast<int>(initialized);
    try {
        Application::Start(ref new ApplicationInitializationCallback([](ApplicationInitializationCallbackParams^) {
            ref new MihawkXbox::App();
        }));
    } catch (Exception^ exception) {
        RoUninitialize(); return exception->HResult;
    }
    RoUninitialize();
    return 0;
}
