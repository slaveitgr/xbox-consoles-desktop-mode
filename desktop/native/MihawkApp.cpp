// Native Xbox UWP host for the packaged Mihawk desktop. No desktop process APIs.
#include <windows.h>
#include <roapi.h>
#include <ppltasks.h>
#include <collection.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "LanTransfer.h"
#include "ModernBrowser.h"

using namespace concurrency;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Security::Cryptography;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::Pickers;
using namespace Windows::Storage::Streams;
using namespace Windows::UI;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;
using namespace Microsoft::Web::WebView2::Core;
using ShellView = Microsoft::UI::Xaml::Controls::WebView2;

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
constexpr wchar_t ShellUrl[] = L"https://mihawk-shell.local/index.html";
bool Trusted(Uri^ uri) { return uri && Wide(uri->AbsoluteUri) == ShellUrl; }
bool WebAddress(Uri^ uri) {
    const auto scheme = uri ? Wide(uri->SchemeName) : std::wstring();
    return (scheme == L"https" || scheme == L"http") && !Wide(uri->Host).empty();
}
struct Item {
    IStorageItem^ value = nullptr;
    StorageFolder^ parent = nullptr;
    bool writable = false;
};
struct Game {
    std::wstring id, name, executable, notes, coverFile;
    String^ cover = L"";
    bool inputEnabled = true, available = false;
    StorageFile^ file = nullptr;
    StorageFolder^ parent = nullptr;
};
struct State {
    Grid^ root = nullptr;
    ShellView^ shell = nullptr;
    Grid^ overlay = nullptr;
    Grid^ surface = nullptr;
    std::shared_ptr<MihawkModern::Browser> modernBrowser;
    std::shared_ptr<MihawkLan::Server> lan = std::make_shared<MihawkLan::Server>();
    TextBlock^ overlayTitle = nullptr;
    TextBlock^ fallback = nullptr;
    Button^ back = nullptr;
    Win32Bridge::RuntimeSession^ session = nullptr;
    DispatcherTimer^ inputTimer = nullptr;
    bool runtimeInput = true, libraryBusy = false;
    std::vector<Game> games;
    StorageFolder^ coversFolder = nullptr;
    std::map<std::wstring, Item> items;
    std::wstring usbId;
    unsigned nextId = 1, pending = 0, notifyCount = 0, rejectedNotifyCount = 0;
    String^ lastNotifyUri = L"";
    String^ shellProbe = L"";
    String^ shellProbeError = L"";
    bool javascriptReady = false, smokeStarted = false;
    bool layoutScalingDisabled = false, useCoreWindow = false;
    String^ layoutError = L"";
    String^ shellEngineVersion = L"";
    String^ userAgent = L"";
    String^ navigation = L"starting";
    String^ lastError = L"";
    task<void> initialized = task_from_result();
    task<void> libraryInitialized = task_from_result();
    task<void> libraryTail = task_from_result();
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
    Text(status, L"shellSource", state->shell && state->shell->Source ? state->shell->Source->AbsoluteUri : L"");
    Text(status, L"lastNotifyUri", state->lastNotifyUri);
    Number(status, L"notifyCount", state->notifyCount);
    Number(status, L"rejectedNotifyCount", state->rejectedNotifyCount);
    Text(status, L"shellProbe", state->shellProbe);
    Text(status, L"shellProbeError", state->shellProbeError);
    Text(status, L"shellEngine", L"WebView2 Edge/Chromium");
    Text(status, L"shellEngineVersion", state->shellEngineVersion);
    Bool(status, L"layoutScalingDisabled", state->layoutScalingDisabled);
    Bool(status, L"useCoreWindow", state->useCoreWindow);
    Text(status, L"layoutError", state->layoutError);
    if (Window::Current) {
        const auto bounds = Window::Current->Bounds;
        Number(status, L"windowWidth", bounds.Width); Number(status, L"windowHeight", bounds.Height);
    }
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
void ProbeShell(const std::shared_ptr<State>& state) {
    if (!state->shell || !Trusted(state->shell->Source) || !state->shell->CoreWebView2) return;
    // Fixed read-only diagnostic in our exact local virtual-host page. No
    // request strings, filenames or remote browser pages are evaluated.
    try {
        String^ script = L"({href:location.href,userAgent:navigator.userAgent,readyState:document.readyState,nativeReply:typeof window.MihawkNativeReply,transport:typeof window.chrome.webview.postMessage,icons:document.querySelectorAll('.desktop-icon').length,body:(document.body?document.body.innerText:'').slice(0,300),viewportWidth:innerWidth,viewportHeight:innerHeight,devicePixelRatio:devicePixelRatio})";
        create_task(state->shell->CoreWebView2->ExecuteScriptAsync(script)).then([state](task<String^> result) {
            try { state->shellProbe = result.get(); state->shellProbeError = L""; }
            catch (Exception^ error) { state->shellProbeError = error->Message; }
            catch (...) { state->shellProbeError = L"The read-only shell probe failed."; }
            Observe(Status(state));
        }, Ui());
    } catch (Exception^ error) { state->shellProbeError = error->Message; Observe(Status(state)); }
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
task<String^> ReadText(StorageFile^ file, unsigned limit) {
    return create_task(file->OpenReadAsync()).then([limit](IRandomAccessStreamWithContentType^ stream) {
        if (stream->Size > limit) { delete stream; Fail(L"This text file exceeds the supported read limit."); }
        auto reader = ref new DataReader(stream->GetInputStreamAt(0)); reader->UnicodeEncoding = UnicodeEncoding::Utf8;
        return create_task(reader->LoadAsync(static_cast<unsigned>(stream->Size))).then([reader, stream](unsigned loaded) {
            String^ result;
            try { result = reader->ReadString(loaded); }
            catch (...) { delete reader; delete stream; throw; }
            delete reader; delete stream; return result;
        });
    });
}
bool GameId(const std::wstring& id) {
    if (id.size() != 37 || id.compare(0, 5, L"game-") != 0) return false;
    for (std::size_t n = 5; n < id.size(); ++n)
        if (!((id[n] >= L'0' && id[n] <= L'9') || (id[n] >= L'a' && id[n] <= L'f'))) return false;
    return true;
}
bool CoverName(const std::wstring& id, const std::wstring& filename) {
    if (filename == id + L".png") return true;
    return filename.size() == 74 && filename.compare(0, 38, id + L"-") == 0 &&
        filename.compare(70, 4, L".png") == 0 && GameId(L"game-" + filename.substr(38, 32));
}
String^ GameGrant(const std::wstring& id, bool parent) { return Str(L"Mihawk." + id + (parent ? L".Folder" : L".File")); }
Game& FindGame(const std::shared_ptr<State>& state, String^ id) {
    for (auto& game : state->games) if (game.id == Wide(id)) return game;
    Fail(L"This library shortcut no longer exists.");
}
JsonObject^ GameJson(const Game& game, bool stored = false) {
    auto result = ref new JsonObject(); Text(result, L"id", Str(game.id));
    Text(result, L"name", Str(game.name)); Text(result, L"executable", Str(game.executable));
    Text(result, L"notes", Str(game.notes)); Bool(result, L"inputEnabled", game.inputEnabled);
    if (stored) Text(result, L"coverFile", Str(game.coverFile));
    else {
        Bool(result, L"available", game.available);
        if (game.cover && game.cover->Length()) Text(result, L"cover", game.cover);
    }
    return result;
}
task<void> SaveLibrary(const std::shared_ptr<State>& state) {
    auto object = ref new JsonObject(); auto games = ref new JsonArray();
    for (const auto& game : state->games) games->Append(GameJson(game, true));
    object->SetNamedValue(L"games", games); Number(object, L"version", 1);
    auto buffer = CryptographicBuffer::ConvertStringToBinary(object->Stringify(), BinaryStringEncoding::Utf8);
    const auto local = ApplicationData::Current->LocalFolder;
    state->libraryTail = state->libraryTail.then([local, buffer](task<void> previous) {
        try { previous.get(); } catch (...) { }
        return create_task(local->CreateFileAsync(L"mihawk-library.json", CreationCollisionOption::OpenIfExists))
            .then([buffer](StorageFile^ file) { return WriteBuffer(file, buffer); });
    }, Ui());
    return state->libraryTail;
}
task<void> RestoreCover(const std::shared_ptr<State>& state, String^ id) {
    const auto filename = FindGame(state, id).coverFile;
    if (filename.empty()) return task_from_result();
    return create_task(state->coversFolder->GetFileAsync(Str(filename))).then([](StorageFile^ file) {
        return create_task(file->OpenReadAsync());
    }).then([](IRandomAccessStreamWithContentType^ stream) {
        if (stream->Size > 256U * 1024U) { delete stream; Fail(L"Stored cover exceeds the preview limit."); }
        auto reader = ref new DataReader(stream->GetInputStreamAt(0));
        return create_task(reader->LoadAsync(static_cast<unsigned>(stream->Size))).then([reader, stream](unsigned count) {
            auto buffer = reader->ReadBuffer(count); delete reader; delete stream;
            return L"data:image/png;base64," + CryptographicBuffer::EncodeToBase64String(buffer);
        });
    }).then([state, id](task<String^> cover) {
        try { FindGame(state, id).cover = cover.get(); }
        catch (...) { FindGame(state, id).cover = L""; }
    }, Ui());
}
task<void> RestoreGame(const std::shared_ptr<State>& state, String^ id) {
    auto access = StorageApplicationPermissions::FutureAccessList;
    const auto key = Wide(id);
    try {
        return create_task(access->GetFileAsync(GameGrant(key, false))).then([state, id, access, key](StorageFile^ file) {
            return create_task(access->GetFolderAsync(GameGrant(key, true))).then([state, id, file](StorageFolder^ parent) {
                auto& game = FindGame(state, id); game.file = file; game.parent = parent;
                game.executable = Wide(file->Name); game.available = true;
            }, Ui());
        }, Ui()).then([state, id](task<void> restored) {
            try { restored.get(); } catch (...) { FindGame(state, id).available = false; }
            return RestoreCover(state, id);
        }, Ui());
    } catch (...) { return RestoreCover(state, id); }
}
task<void> LoadLibrary(const std::shared_ptr<State>& state) {
    auto local = ApplicationData::Current->LocalFolder;
    return create_task(local->CreateFolderAsync(L"MihawkCovers", CreationCollisionOption::OpenIfExists))
        .then([state, local](StorageFolder^ covers) {
            state->coversFolder = covers; return local->TryGetItemAsync(L"mihawk-library.json");
        }, Ui()).then([](IStorageItem^ item) -> task<String^> {
            auto file = dynamic_cast<StorageFile^>(item);
            return file ? ReadText(file, 2U * 1024U * 1024U) : task_from_result<String^>(nullptr);
        }, Ui()).then([state](String^ text) -> task<void> {
            if (!text) return task_from_result();
            auto object = JsonObject::Parse(text); auto games = object->GetNamedArray(L"games");
            if (games->Size > 64) Fail(L"The library exceeds its 64-shortcut limit.");
            for (unsigned n = 0; n < games->Size; ++n) {
                auto json = games->GetObjectAt(n); Game game;
                game.id = Wide(Required(json, L"id"));
                if (!GameId(game.id)) Fail(L"Library contains an invalid shortcut ID.");
                for (const auto& previous : state->games)
                    if (previous.id == game.id) Fail(L"Library contains duplicate shortcut IDs.");
                game.name = Wide(Required(json, L"name", 128));
                game.executable = Wide(Required(json, L"executable", 260));
                game.notes = json->HasKey(L"notes") ? Wide(Required(json, L"notes", 4096)) : L"";
                game.inputEnabled = json->HasKey(L"inputEnabled") ? json->GetNamedBoolean(L"inputEnabled") : true;
                game.coverFile = json->HasKey(L"coverFile") ? Wide(Required(json, L"coverFile", 80)) : L"";
                if (!game.coverFile.empty() && !CoverName(game.id, game.coverFile)) Fail(L"Library contains an invalid cover filename.");
                state->games.push_back(std::move(game));
            }
            task<void> restored = task_from_result();
            for (const auto& game : state->games) {
                auto id = Str(game.id);
                restored = restored.then([state, id] { return RestoreGame(state, id); }, Ui());
            }
            return restored;
        }, Ui());
}
task<IBuffer^> CoverPreview(StorageFile^ file) {
    return create_task(file->OpenReadAsync()).then([](IRandomAccessStreamWithContentType^ input) {
        if (!input->Size || input->Size > 4U * 1024U * 1024U) { delete input; Fail(L"Choose an image no larger than 4 MiB."); }
        return create_task(BitmapDecoder::CreateAsync(input)).then([input](BitmapDecoder^ decoder) {
            const auto width = decoder->PixelWidth, height = decoder->PixelHeight;
            if (!width || !height || width > 4096 || height > 4096) Fail(L"Cover images are limited to 4096 pixels per side.");
            auto transform = ref new BitmapTransform();
            const double scale = (std::max)(1.0, (std::max)(static_cast<double>(width), static_cast<double>(height)) / 192.0);
            transform->ScaledWidth = (std::max)(1U, static_cast<unsigned>(width / scale));
            transform->ScaledHeight = (std::max)(1U, static_cast<unsigned>(height / scale));
            return create_task(decoder->GetSoftwareBitmapAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied,
                transform, ExifOrientationMode::RespectExifOrientation, ColorManagementMode::ColorManageToSRgb));
        }, Ui()).then([input](SoftwareBitmap^ bitmap) {
            delete input;
            auto output = ref new InMemoryRandomAccessStream();
            return create_task(BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId, output))
                .then([bitmap, output](BitmapEncoder^ encoder) {
                    encoder->SetSoftwareBitmap(bitmap);
                    return create_task(encoder->FlushAsync()).then([bitmap, output] {
                        delete bitmap;
                        if (output->Size > 256U * 1024U) Fail(L"The image preview exceeds its storage limit.");
                        auto reader = ref new DataReader(output->GetInputStreamAt(0));
                        return create_task(reader->LoadAsync(static_cast<unsigned>(output->Size))).then([reader, output](unsigned count) {
                            auto result = reader->ReadBuffer(count); delete reader; delete output; return result;
                        });
                    });
                });
        }, Ui());
    }, Ui());
}
void CloseOverlay(const std::shared_ptr<State>& state) {
    if (state->session) {
        state->session->SetInputEnabled(false);
        state->session->Close();
        state->session = nullptr;
    }
    if (state->modernBrowser) { state->modernBrowser->Close(); state->modernBrowser.reset(); }
    state->surface->Children->Clear();
    state->overlay->Visibility = Visibility::Collapsed;
}
void OpenOverlay(const std::shared_ptr<State>& state, String^ title, bool browser) {
    CloseOverlay(state);
    state->overlayTitle->Text = title;
    state->back->Visibility = browser ? Visibility::Visible : Visibility::Collapsed;
    state->overlay->Visibility = Visibility::Visible;
}
task<JsonObject^> RunFile(const std::shared_ptr<State>& state, StorageFile^ file, StorageFolder^ parent, bool inputEnabled) {
    if (!parent) Fail(L"Open the executable from its containing folder first.");
    OpenOverlay(state, L"Preparing " + file->Name, false);
    state->runtimeInput = inputEnabled;
    auto session = ref new Win32Bridge::RuntimeSession(state->surface); state->session = session;
    return create_task(session->PrepareAsync(file, parent)).then([state, session, file, inputEnabled](bool prepared) {
        if (state->session != session) Fail(L"Execution was canceled before startup.");
        if (!prepared) { state->overlayTitle->Text = L"Stopped: " + session->LastError; Fail(session->LastError); }
        session->SetInputEnabled(inputEnabled);
        state->overlayTitle->Text = L"Running " + file->Name;
        auto running = create_task(session->RunAsync());
        session->SetInputEnabled(inputEnabled);
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
        auto result = ref new JsonObject(); Bool(result, L"started", true); Bool(result, L"inputEnabled", inputEnabled);
        Text(result, L"name", file->Name); Text(result, L"importSummary", session->ImportSummary);
        return result;
    }, Ui());
}
void NativeSmoke(const std::shared_ptr<State>& state);
task<JsonObject^> Dispatch(const std::shared_ptr<State>& state, String^ method, JsonObject^ parameters) {
    const auto name = Wide(method);
    if (name == L"ready") {
        state->userAgent = parameters->HasKey(L"userAgent") ? Required(parameters, L"userAgent", 1024) : L"";
        state->javascriptReady = true;
        state->navigation = L"ready";
        state->lastError = L"";
        state->fallback->Visibility = Visibility::Collapsed;
        NativeSmoke(state);
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
    if (name == L"libraryList") {
        auto result = ref new JsonObject(); auto games = ref new JsonArray();
        for (const auto& game : state->games) games->Append(GameJson(game));
        result->SetNamedValue(L"games", games); return task_from_result(result);
    }
    if (name == L"libraryAdd") {
        const auto item = Find(state, Required(parameters, L"file")); auto file = File(item);
        if (!item.parent) Fail(L"Open the containing folder before creating a library shortcut.");
        const auto path = Wide(file->Path);
        for (const auto& existing : state->games)
            if (!path.empty() && existing.file && Wide(existing.file->Path) == path) return task_from_result(GameJson(existing));
        if (state->games.size() >= 64) Fail(L"The library is limited to 64 shortcuts.");
        Game game;
        game.id = L"game-" + Wide(CryptographicBuffer::EncodeToHexString(CryptographicBuffer::GenerateRandom(16)));
        game.name = parameters->HasKey(L"name") ? Wide(Required(parameters, L"name", 128)) : Wide(file->DisplayName);
        if (game.name.empty()) Fail(L"A game name is required.");
        game.file = file; game.parent = item.parent; game.available = true; game.executable = Wide(file->Name);
        auto access = StorageApplicationPermissions::FutureAccessList;
        access->AddOrReplace(GameGrant(game.id, false), file);
        try { access->AddOrReplace(GameGrant(game.id, true), item.parent); }
        catch (...) { access->Remove(GameGrant(game.id, false)); throw; }
        const auto id = Str(game.id); state->games.push_back(game);
        return SaveLibrary(state).then([state, id](task<void> saved) {
            try { saved.get(); }
            catch (...) {
                auto access = StorageApplicationPermissions::FutureAccessList;
                try { access->Remove(GameGrant(Wide(id), false)); access->Remove(GameGrant(Wide(id), true)); } catch (...) { }
                state->games.erase(std::remove_if(state->games.begin(), state->games.end(), [id](const Game& item) { return item.id == Wide(id); }), state->games.end());
                throw;
            }
            return GameJson(FindGame(state, id));
        }, Ui());
    }
    if (name == L"libraryUpdate") {
        auto id = Required(parameters, L"game"); auto& game = FindGame(state, id); const auto previous = game;
        const auto title = parameters->HasKey(L"name") ? Wide(Required(parameters, L"name", 128)) : game.name;
        if (title.empty()) Fail(L"A game name is required.");
        const auto notes = parameters->HasKey(L"notes") ? Wide(Required(parameters, L"notes", 4096)) : game.notes;
        const auto input = parameters->HasKey(L"inputEnabled") ? parameters->GetNamedBoolean(L"inputEnabled") : game.inputEnabled;
        if (parameters->HasKey(L"cover")) {
            if (Required(parameters, L"cover", 0)->Length()) Fail(L"Use the local image picker to change a cover.");
            game.coverFile.clear(); game.cover = L"";
        }
        game.name = title; game.notes = notes; game.inputEnabled = input;
        return SaveLibrary(state).then([state, id, previous](task<void> saved) {
            try { saved.get(); } catch (...) { FindGame(state, id) = previous; throw; }
            return GameJson(FindGame(state, id));
        }, Ui());
    }
    if (name == L"libraryRemove") {
        auto id = Required(parameters, L"game"); const auto removed = FindGame(state, id);
        state->games.erase(std::remove_if(state->games.begin(), state->games.end(), [id](const Game& item) { return item.id == Wide(id); }), state->games.end());
        return SaveLibrary(state).then([state, removed](task<void> saved) {
            try { saved.get(); } catch (...) { state->games.push_back(removed); throw; }
            auto access = StorageApplicationPermissions::FutureAccessList;
            for (bool parent : {false, true}) {
                try { auto key = GameGrant(removed.id, parent); if (access->ContainsItem(key)) access->Remove(key); } catch (...) { }
            }
            if (!removed.coverFile.empty()) {
                Observe(create_task(state->coversFolder->GetFileAsync(Str(removed.coverFile)))
                    .then([](StorageFile^ file) { return file->DeleteAsync(); }));
            }
            return ref new JsonObject();
        }, Ui());
    }
    if (name == L"libraryCover") {
        auto id = Required(parameters, L"game"); const auto previous = FindGame(state, id);
        auto file = File(Find(state, Required(parameters, L"file")));
        return CoverPreview(file).then([state, id, previous](IBuffer^ preview) {
            // A new app-owned preview is committed before metadata points to it.
            // Failure preserves both the previous shortcut and previous cover.
            const auto filename = Wide(id) + L"-" + Wide(CryptographicBuffer::EncodeToHexString(
                CryptographicBuffer::GenerateRandom(16))) + L".png";
            auto cover = L"data:image/png;base64," + CryptographicBuffer::EncodeToBase64String(preview);
            return create_task(state->coversFolder->CreateFileAsync(Str(filename), CreationCollisionOption::FailIfExists))
                .then([state, id, previous, preview, filename, cover](StorageFile^ target) {
                    return WriteBuffer(target, preview).then([state, id, filename, cover] {
                        auto& game = FindGame(state, id); game.coverFile = filename; game.cover = cover;
                        return SaveLibrary(state);
                    }, Ui()).then([state, id, previous, target](task<void> saved) {
                        try { saved.get(); }
                        catch (...) { FindGame(state, id) = previous; Observe(create_task(target->DeleteAsync())); throw; }
                        if (!previous.coverFile.empty())
                            Observe(create_task(state->coversFolder->GetFileAsync(Str(previous.coverFile)))
                                .then([](StorageFile^ old) { return old->DeleteAsync(); }));
                        return GameJson(FindGame(state, id));
                    }, Ui());
                }, Ui());
        }, Ui());
    }
    if (name == L"libraryRun") {
        const auto game = FindGame(state, Required(parameters, L"game"));
        if (!game.available || !game.file || !game.parent) Fail(L"The game file is unavailable. Reconnect its drive or add it again.");
        return RunFile(state, game.file, game.parent, game.inputEnabled);
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
    if (name == L"lanStart") {
        return state->initialized.then([state](task<void> initialized) {
            initialized.get(); return state->lan->StartAsync(Folder(Find(state, L"workspace")));
        }, Ui());
    }
    if (name == L"lanStop") return task_from_result(state->lan->Stop());
    if (name == L"lanStatus") return task_from_result(state->lan->Status());
    if (name == L"browser") {
        auto uri = ref new Uri(Required(parameters, L"url", 4096));
        if (!WebAddress(uri)) Fail(L"The browser accepts only HTTP and HTTPS addresses.");
        OpenOverlay(state, L"Starting Edge / Chromium", true);
        auto browser = std::make_shared<MihawkModern::Browser>(); state->modernBrowser = browser;
        return browser->OpenAsync(state->surface, uri, state->overlayTitle);
    }
    if (name == L"closeRuntime") {
        CloseOverlay(state); return task_from_result(ref new JsonObject());
    }
    if (name == L"run") {
        const auto item = Find(state, Required(parameters, L"file")); auto file = File(item);
        return RunFile(state, file, item.parent, true);
    }
    Fail(L"This native method is not implemented.");
}
task<void> SmokeRecord(String^ filename, JsonObject^ result) {
    // Pairing data stays in the app's private LocalState and is emitted only
    // for an explicitly requested native smoke, never ordinary diagnostics.
    auto buffer = CryptographicBuffer::ConvertStringToBinary(result->Stringify(), BinaryStringEncoding::Utf8);
    return create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(filename, CreationCollisionOption::OpenIfExists))
        .then([buffer](StorageFile^ file) { return WriteBuffer(file, buffer); }, Ui());
}
void NativeSmoke(const std::shared_ptr<State>& state) {
    if (state->smokeStarted) return;
    state->smokeStarted = true;
    // Only an explicit app-local test configuration enables network/browser
    // smoke actions. An ordinary launch neither listens nor opens a web page.
    Observe(create_task(ApplicationData::Current->LocalFolder->TryGetItemAsync(L"mihawk-native-smoke.json"))
        .then([](IStorageItem^ item) -> task<String^> {
            auto file = dynamic_cast<StorageFile^>(item);
            return file ? ReadText(file, 4096) : task_from_result<String^>(nullptr);
        }, Ui()).then([state](String^ text) -> task<void> {
            if (!text) return task_from_result();
            auto config = JsonObject::Parse(text);
            task<void> smoke = task_from_result();
            if (config->HasKey(L"lan") && config->GetNamedBoolean(L"lan"))
                smoke = smoke.then([state] {
                    return Dispatch(state, L"lanStart", ref new JsonObject()).then([](JsonObject^ result) {
                        return SmokeRecord(L"mihawk-lan-status.json", result);
                    }, Ui());
                }, Ui());
            if (config->HasKey(L"browser") && config->GetNamedBoolean(L"browser"))
                smoke = smoke.then([state] {
                    auto parameters = ref new JsonObject(); Text(parameters, L"url", L"https://example.com");
                    return Dispatch(state, L"browser", parameters).then([](JsonObject^ result) {
                        return SmokeRecord(L"mihawk-native-smoke-result.json", result);
                    }, Ui());
                }, Ui());
            return smoke;
        }, Ui()).then([state](task<void> completed) {
            try { completed.get(); }
            catch (Exception^ error) { HostError(state, L"Native smoke: " + error->Message); }
            catch (...) { HostError(state, L"Native smoke failed."); }
        }, Ui()));
}
void Reply(const std::shared_ptr<State>& state, String^ id, JsonObject^ result, String^ error) {
    auto response = ref new JsonObject(); Text(response, L"id", id); Bool(response, L"ok", !error);
    if (error) Text(response, L"error", error); else response->SetNamedValue(L"result", result ? result : ref new JsonObject());
    try {
        if (!state->shell || !state->shell->CoreWebView2 || !Trusted(state->shell->Source))
            Fail(L"The trusted desktop message channel is unavailable.");
        state->shell->CoreWebView2->PostWebMessageAsJson(response->Stringify());
    } catch (Exception^ exception) { HostError(state, L"Desktop reply failed: " + exception->Message); }
}
void Request(const std::shared_ptr<State>& state, String^ payload) {
    String^ id = L"";
    try {
        if (!payload || payload->Length() > 2U * 1024U * 1024U) Fail(L"Native request is too large.");
        auto request = JsonObject::Parse(payload);
        id = Required(request, L"id"); if (id->Length() == 0) Fail(L"A request ID is required.");
        auto method = Required(request, L"method", 64);
        auto parameters = request->HasKey(L"params") ? request->GetNamedObject(L"params") : ref new JsonObject();
        if (state->pending >= 8) Fail(L"Too many pending native requests. Try again shortly.");
        const auto name = Wide(method);
        const bool library = name.compare(0, 7, L"library") == 0;
        const bool mutation = name == L"libraryAdd" || name == L"libraryUpdate" || name == L"libraryRemove" || name == L"libraryCover";
        if (library && state->libraryBusy) Fail(L"A library change is still being saved. Try again shortly.");
        auto operation = library ? state->libraryInitialized.then([state, method, parameters](task<void> ready) {
            ready.get(); return Dispatch(state, method, parameters);
        }, Ui()) : Dispatch(state, method, parameters);
        if (mutation) state->libraryBusy = true;
        ++state->pending;
        operation.then([state, id, mutation](task<JsonObject^> completed) {
            --state->pending;
            if (mutation) state->libraryBusy = false;
            try { Reply(state, id, completed.get(), nullptr); }
            catch (Exception^ exception) { Reply(state, id, nullptr, exception->Message); }
            catch (...) { Reply(state, id, nullptr, L"The native operation failed."); }
        }, Ui());
    } catch (Exception^ exception) { Reply(state, id, nullptr, exception->Message); }
    catch (...) { Reply(state, id, nullptr, L"Invalid native request."); }
}
void ShellMessage(const std::shared_ptr<State>& state, CoreWebView2^ sender, CoreWebView2WebMessageReceivedEventArgs^ args) {
    ++state->notifyCount; state->lastNotifyUri = args->Source;
    if (!state->shell || sender != state->shell->CoreWebView2 || Wide(args->Source) != ShellUrl) {
        ++state->rejectedNotifyCount;
        if (state->rejectedNotifyCount <= 4) Observe(Status(state));
        return;
    }
    try { Request(state, args->TryGetWebMessageAsString()); }
    catch (Exception^ error) { state->lastError = L"Desktop message: " + error->Message; Observe(Status(state)); }
    catch (...) { state->lastError = L"Desktop message was not a JSON string."; Observe(Status(state)); }
}
Button^ Action(String^ title) {
    auto button = ref new Button(); button->Content = title; button->Margin = ThicknessHelper::FromUniformLength(8);
    return button;
}
}

namespace MihawkXbox {
ref class App sealed : public Application, public Windows::UI::Xaml::Markup::IXamlMetadataProvider {
public:
    virtual Windows::UI::Xaml::Markup::IXamlType^ GetXamlType(Windows::UI::Xaml::Interop::TypeName type) {
        return Metadata()->GetXamlType(type);
    }
    virtual Windows::UI::Xaml::Markup::IXamlType^ GetXamlType(String^ fullName) {
        return Metadata()->GetXamlType(fullName);
    }
    virtual Platform::Array<Windows::UI::Xaml::Markup::XmlnsDefinition>^ GetXmlnsDefinitions() {
        return Metadata()->GetXmlnsDefinitions();
    }
internal:
    App() { }
protected:
    virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^) override {
        if (Window::Current->Content) { Window::Current->Activate(); return; }
        auto state = std::make_shared<State>(); m_state = state;
        state->root = ref new Grid();
        state->root->Background = ref new SolidColorBrush(Colors::Black);
        Window::Current->Content = state->root;
        try {
            if (Windows::Foundation::Metadata::ApiInformation::IsTypePresent(L"Windows.UI.ViewManagement.ApplicationViewScaling"))
                state->layoutScalingDisabled = Windows::UI::ViewManagement::ApplicationViewScaling::TrySetDisableLayoutScaling(true);
            state->useCoreWindow = Windows::UI::ViewManagement::ApplicationView::GetForCurrentView()->SetDesiredBoundsMode(
                Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
        } catch (Exception^ error) { state->layoutError = error->Message; }
        Window::Current->Activate();
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
            state->libraryInitialized = state->initialized.then([state] { return LoadLibrary(state); }, Ui());
            // Observing here prevents an unhandled-task exception; library RPCs
            // still await the original task and receive the concrete load error.
            Observe(state->libraryInitialized.then([state](task<void> initialized) {
                try { initialized.get(); }
                catch (Exception^ error) { state->lastError = L"Library restore: " + error->Message; Observe(Status(state)); }
                catch (...) { state->lastError = L"Library could not be restored."; Observe(Status(state)); }
            }, Ui()));
            state->inputTimer = ref new DispatcherTimer();
            TimeSpan inputInterval; inputInterval.Duration = 1000000;
            state->inputTimer->Interval = inputInterval;
            state->inputTimer->Tick += ref new EventHandler<Object^>([state](Object^, Object^) {
                // The bridge worker enables input as it starts. Reapply an
                // explicitly disabled per-game setting while that worker runs.
                try { if (state->session && !state->runtimeInput) state->session->SetInputEnabled(false); }
                catch (...) { }
            });
            state->inputTimer->Start();
            Suspending += ref new Windows::UI::Xaml::SuspendingEventHandler(
                [state](Object^, Windows::ApplicationModel::SuspendingEventArgs^) {
                    try { state->lan->Stop(); } catch (...) { }
                    try { if (state->session) state->session->SetInputEnabled(false); } catch (...) { }
                });
            state->overlay = ref new Grid(); state->overlay->Visibility = Visibility::Collapsed;
            state->overlay->Background = ref new SolidColorBrush(Colors::Black);
            auto headerRow = ref new RowDefinition(); headerRow->Height = GridLengthHelper::Auto;
            state->overlay->RowDefinitions->Append(headerRow);
            state->overlay->RowDefinitions->Append(ref new RowDefinition());
            auto header = ref new StackPanel(); header->Orientation = Orientation::Horizontal;
            auto close = Action(L"Back to desktop"); state->back = Action(L"Back");
            close->Click += ref new RoutedEventHandler([state](Object^, RoutedEventArgs^) { CloseOverlay(state); });
            state->back->Click += ref new RoutedEventHandler([state](Object^, RoutedEventArgs^) {
                if (state->modernBrowser && state->modernBrowser->CanGoBack()) state->modernBrowser->GoBack();
            });
            state->overlayTitle = ref new TextBlock();
            state->overlayTitle->Foreground = ref new SolidColorBrush(Colors::White);
            state->overlayTitle->TextWrapping = TextWrapping::Wrap;
            state->overlayTitle->MaxWidth = 900;
            state->overlayTitle->VerticalAlignment = VerticalAlignment::Center;
            header->Children->Append(close); header->Children->Append(state->back); header->Children->Append(state->overlayTitle);
            state->surface = ref new Grid(); Grid::SetRow(state->surface, 1);
            state->overlay->Children->Append(header); state->overlay->Children->Append(state->surface);
            MihawkModern::InitializeResources();
            state->shell = ref new ShellView();
            state->root->Children->InsertAt(0, state->shell);
            state->root->Children->Append(state->overlay);
            state->navigation = L"initializing-webview2"; Observe(Status(state));
            create_task(state->shell->EnsureCoreWebView2Async()).then([state](task<void> initialized) {
                try {
                    initialized.get();
                    auto core = state->shell->CoreWebView2;
                    if (!core) Fail(L"The desktop WebView2 engine did not initialize.");
                    state->shellEngineVersion = core->Environment->BrowserVersionString;
                    core->Settings->IsWebMessageEnabled = true;
                    core->Settings->IsGeneralAutofillEnabled = false;
                    core->Settings->IsPasswordAutosaveEnabled = false;
                    core->Settings->AreDevToolsEnabled = false;
                    core->SetVirtualHostNameToFolderMapping(L"mihawk-shell.local", L"Mihawk", CoreWebView2HostResourceAccessKind::DenyCors);
                    state->shell->NavigationStarting += ref new TypedEventHandler<ShellView^, CoreWebView2NavigationStartingEventArgs^>(
                        [](ShellView^, CoreWebView2NavigationStartingEventArgs^ args) {
                            try { if (!Trusted(ref new Uri(args->Uri))) args->Cancel = true; }
                            catch (...) { args->Cancel = true; }
                        });
                    state->shell->NavigationCompleted += ref new TypedEventHandler<ShellView^, CoreWebView2NavigationCompletedEventArgs^>(
                        [state](ShellView^, CoreWebView2NavigationCompletedEventArgs^ args) {
                            state->navigation = Str(args->IsSuccess ? (state->javascriptReady ? L"ready" : L"loaded") : L"failed");
                            if (!args->IsSuccess) HostError(state, L"The packaged desktop page could not load. WebView2 error " +
                                Str(std::to_wstring(static_cast<int>(args->WebErrorStatus))));
                            else { Observe(Status(state)); ProbeShell(state); }
                        });
                    core->WebMessageReceived += ref new TypedEventHandler<CoreWebView2^, CoreWebView2WebMessageReceivedEventArgs^>(
                        [state](CoreWebView2^ sender, CoreWebView2WebMessageReceivedEventArgs^ args) { ShellMessage(state, sender, args); });
                    core->NewWindowRequested += ref new TypedEventHandler<CoreWebView2^, CoreWebView2NewWindowRequestedEventArgs^>(
                        [](CoreWebView2^, CoreWebView2NewWindowRequestedEventArgs^ args) { args->Handled = true; });
                    core->LaunchingExternalUriScheme += ref new TypedEventHandler<CoreWebView2^, CoreWebView2LaunchingExternalUriSchemeEventArgs^>(
                        [](CoreWebView2^, CoreWebView2LaunchingExternalUriSchemeEventArgs^ args) { args->Cancel = true; });
                    state->shell->Source = ref new Uri(ref new String(ShellUrl));
                } catch (Exception^ error) { state->navigation = L"failed"; HostError(state, L"Desktop WebView2 initialization failed: " + error->Message); }
                catch (...) { state->navigation = L"failed"; HostError(state, L"Desktop WebView2 initialization failed."); }
            }, Ui());
        } catch (Exception^ exception) { HostError(state, L"Native desktop initialization failed: " + exception->Message); }
        catch (...) { HostError(state, L"Native desktop initialization failed."); }
    }
private:
    Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider^ Metadata() {
        if (!m_metadata) m_metadata = ref new Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider();
        return m_metadata;
    }
    Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider^ m_metadata;
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
