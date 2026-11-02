// SPDX-License-Identifier: MIT
// Opt-in LAN file exchange for the XWindows OS app-owned inbox. No external paths or
// executable activation are accepted by this server.
#pragma once

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace MihawkLan {
namespace Detail {
constexpr std::size_t HeaderLimit = 16U * 1024U;
constexpr std::size_t UploadLimit = 16U * 1024U * 1024U;
constexpr unsigned FilePage = 128;

struct HttpError {
    int code;
    std::string message;
};
[[noreturn]] inline void Reject(int code, const char* message) { throw HttpError{code, message}; }
inline std::string Lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}
inline bool EqualToken(const std::string& supplied, const std::string& expected) noexcept {
    // Never return early at the first differing byte. Pairing tokens are exactly
    // 32 hexadecimal characters, representing 128 cryptographically random bits.
    unsigned difference = static_cast<unsigned>(supplied.size() ^ expected.size());
    for (std::size_t i = 0; i < 32; ++i) {
        const unsigned a = i < supplied.size() ? static_cast<unsigned char>(supplied[i]) : 0;
        const unsigned b = i < expected.size() ? static_cast<unsigned char>(expected[i]) : 0;
        difference |= a ^ b;
    }
    return difference == 0 && expected.size() == 32;
}
inline std::uint64_t Decimal(const std::string& value, std::uint64_t maximum) {
    if (value.empty()) Reject(400, "A decimal value is missing.");
    std::uint64_t result = 0;
    for (const auto c : value) {
        if (c < '0' || c > '9' || result > maximum / 10 ||
            (result == maximum / 10 && static_cast<unsigned>(c - '0') > maximum % 10))
            Reject(400, "An invalid or oversized decimal value was supplied.");
        result = result * 10 + static_cast<unsigned>(c - '0');
    }
    return result;
}
inline int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
inline std::string Unescape(const std::string& input) {
    if (input.empty() || input.size() > 1440) Reject(400, "The filename is missing or too long.");
    std::string output;
    for (std::size_t i = 0; i < input.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == '%') {
            if (i + 2 >= input.size() || Hex(input[i + 1]) < 0 || Hex(input[i + 2]) < 0)
                Reject(400, "The filename encoding is invalid.");
            c = static_cast<unsigned char>(Hex(input[i + 1]) * 16 + Hex(input[i + 2]));
            i += 2;
        }
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            Reject(400, "Use a filename without a path or reserved characters.");
        output.push_back(static_cast<char>(c));
    }
    if (output.empty() || output.size() > 480 || output == "." || output == ".." ||
        output.back() == '.' || output.back() == ' ' || Lower(output).find(".mihawk-") == 0)
        Reject(400, "The filename is reserved or too long.");
    auto stem = Lower(output.substr(0, output.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
        (stem.size() == 4 && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt") &&
            stem[3] >= '1' && stem[3] <= '9')) Reject(400, "The filename is reserved.");
    return output;
}
inline bool Utf8Filename(const std::string& text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned count = 0, point = 0, minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) { count = 1; point = first & 31; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { count = 2; point = first & 15; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { count = 3; point = first & 7; minimum = 0x10000; }
        else return false;
        if (count > text.size() - i) return false;
        while (count--) {
            const auto c = static_cast<unsigned char>(text[i++]);
            if ((c & 0xC0) != 0x80) return false;
            point = point * 64 + (c & 63);
        }
        if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) return false;
    }
    return true;
}
inline std::string Escape(const std::string& text) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') output.push_back(static_cast<char>(c));
        else { output.push_back('%'); output.push_back(digits[c >> 4]); output.push_back(digits[c & 15]); }
    }
    return output;
}
struct Request {
    std::string method, target;
    std::map<std::string, std::string> headers;
    std::uint64_t length = 0;
};
inline Request Parse(const std::string& header, const std::string& host) {
    if (header.size() > HeaderLimit) Reject(431, "Request headers are limited to 16 KiB.");
    if (header.size() < 4 || header.substr(header.size() - 4) != "\r\n\r\n") Reject(400, "Incomplete headers.");
    const auto first = header.find("\r\n");
    if (first == std::string::npos || first > 2048) Reject(400, "Invalid request line.");
    const auto line = header.substr(0, first);
    const auto a = line.find(' '), b = a == std::string::npos ? a : line.find(' ', a + 1);
    if (a == std::string::npos || b == std::string::npos || line.find(' ', b + 1) != std::string::npos)
        Reject(400, "Invalid request line.");
    const auto version = line.substr(b + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") Reject(400, "Unsupported HTTP version.");
    Request result;
    result.method = line.substr(0, a); result.target = line.substr(a + 1, b - a - 1);
    if (result.target.empty() || result.target[0] != '/' || result.target.find('#') != std::string::npos)
        Reject(400, "Invalid request target.");
    for (unsigned char c : result.target) if (c <= 32 || c >= 127) Reject(400, "Invalid request target.");
    unsigned count = 0;
    auto position = first + 2;
    while (position < header.size()) {
        const auto end = header.find("\r\n", position);
        if (end == std::string::npos) Reject(400, "Incomplete headers.");
        if (end == position) { position = end + 2; break; }
        const auto colon = header.find(':', position);
        if (++count > 64 || colon == std::string::npos || colon >= end || colon == position)
            Reject(400, "Invalid request headers.");
        auto name = Lower(header.substr(position, colon - position));
        for (unsigned char c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                Reject(400, "Invalid header name.");
        auto begin = colon + 1, finish = end;
        while (begin < finish && (header[begin] == ' ' || header[begin] == '\t')) ++begin;
        while (finish > begin && (header[finish - 1] == ' ' || header[finish - 1] == '\t')) --finish;
        const auto value = header.substr(begin, finish - begin);
        for (unsigned char c : value) if (c < 32 || c == 127) Reject(400, "Invalid header value.");
        if (!result.headers.emplace(name, value).second) Reject(400, "Duplicate headers are unsupported.");
        position = end + 2;
    }
    if (position != header.size()) Reject(400, "Unexpected header data.");
    auto get = [&result](const char* key) {
        const auto found = result.headers.find(key);
        return found == result.headers.end() ? std::string() : found->second;
    };
    // Exact Host validation prevents a page on an attacker-controlled hostname
    // from reaching this listener through DNS rebinding.
    if (get("host") != host) Reject(403, "Use the pairing address shown on the Xbox.");
    const auto origin = get("origin");
    if (result.headers.count("origin") && origin != "http://" + host) Reject(403, "Cross-origin requests are forbidden.");
    if (result.headers.count("transfer-encoding")) Reject(400, "Chunked requests are unsupported.");
    if (result.headers.count("content-length")) result.length = Decimal(get("content-length"), UploadLimit);
    if (result.method != "GET" && result.method != "POST") Reject(405, "Use GET or POST.");
    if (result.method == "GET" && result.length) Reject(400, "GET request bodies are unsupported.");
    return result;
}
inline bool PrivateIpv4(const std::string& text) {
    unsigned octets[4]{};
    std::size_t start = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto end = text.find('.', start);
        if ((i < 3 && end == std::string::npos) || (i == 3 && end != std::string::npos)) return false;
        try { octets[i] = static_cast<unsigned>(Decimal(text.substr(start,
            end == std::string::npos ? text.size() - start : end - start), 255)); }
        catch (const HttpError&) { return false; }
        start = end == std::string::npos ? text.size() : end + 1;
    }
    return octets[0] == 10 || (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
        (octets[0] == 192 && octets[1] == 168);
}
} // namespace Detail
} // namespace MihawkLan

// This guard lets the hostile-input parser run under portable sanitizers. The
// production UWP build always includes the actual listener below.
#ifndef MIHAWK_LAN_PORTABLE_ONLY
#include <windows.h>
#include <ppltasks.h>
#include <collection.h>

namespace MihawkLan {
using namespace concurrency;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::Sockets;
using namespace Windows::Security::Cryptography;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::System::Threading;

namespace Detail {
inline Platform::Array<unsigned char>^ Bytes(const unsigned char* data, unsigned length) {
    auto result = ref new Platform::Array<unsigned char>(length);
    if (length) std::copy(data, data + length, result->Data);
    return result;
}
inline std::string Utf8(Platform::String^ value) {
    auto buffer = CryptographicBuffer::ConvertStringToBinary(value ? value : L"", BinaryStringEncoding::Utf8);
    Platform::Array<unsigned char>^ bytes = nullptr;
    CryptographicBuffer::CopyToByteArray(buffer, &bytes);
    return bytes->Length ? std::string(reinterpret_cast<const char*>(bytes->Data), bytes->Length) : std::string();
}
inline Platform::String^ Unicode(const std::string& value) {
    auto bytes = Bytes(reinterpret_cast<const unsigned char*>(value.data()), static_cast<unsigned>(value.size()));
    return CryptographicBuffer::ConvertBinaryToString(BinaryStringEncoding::Utf8, CryptographicBuffer::CreateFromByteArray(bytes));
}
inline void Text(JsonObject^ object, Platform::String^ key, Platform::String^ value) {
    object->SetNamedValue(key, JsonValue::CreateStringValue(value ? value : L""));
}
inline void Number(JsonObject^ object, Platform::String^ key, double value) { object->SetNamedValue(key, JsonValue::CreateNumberValue(value)); }
inline std::string RandomToken() { return Utf8(CryptographicBuffer::EncodeToHexString(CryptographicBuffer::GenerateRandom(16))); }
inline task<void> Drain(DataWriter^ writer) {
    if (!writer->UnstoredBufferLength) return task_from_result();
    return create_task(writer->StoreAsync()).then([writer](unsigned written) -> task<void> {
        if (!written) Reject(500, "A stream stopped accepting data.");
        return Drain(writer);
    }, task_continuation_context::use_arbitrary());
}
inline const char* Reason(int code) {
    switch (code) { case 200: return "OK"; case 201: return "Created"; case 400: return "Bad Request";
    case 401: return "Unauthorized"; case 403: return "Forbidden"; case 404: return "Not Found";
    case 405: return "Method Not Allowed"; case 411: return "Length Required"; case 413: return "Content Too Large";
    case 431: return "Request Header Fields Too Large"; case 503: return "Service Unavailable"; default: return "Internal Server Error"; }
}
inline const char* Page() {
    // Public static pairing UI contains no token, filenames or external assets.
    // Every API request carries the token in an Authorization header; fragments
    // never reach the server, browser history referrers or HTTP request targets.
    return R"MIHAWK(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>XWindows OS LAN Files</title><style>*{box-sizing:border-box}body{margin:0;background:#eff4f8;color:#142d44;font:16px system-ui,sans-serif}main{max-width:760px;margin:32px auto;padding:24px;background:white;border-radius:18px;box-shadow:0 12px 45px #142d4420}h1{margin-top:0}p{line-height:1.5}input,button{font:inherit;padding:12px;border:1px solid #8dabc0;border-radius:8px;max-width:100%}input[type=password]{width:100%}button{background:#163c5a;color:white;cursor:pointer;margin:8px 4px 8px 0}button:disabled{opacity:.5;cursor:wait}ul{padding:0;list-style:none}li{display:flex;align-items:center;justify-content:space-between;gap:16px;border-top:1px solid #dfebf3;padding:8px 0;overflow-wrap:anywhere}#status{min-height:24px;white-space:pre-wrap}small{color:#52697b}.notice{background:#fff5d8;padding:12px;border-radius:8px}</style><main><h1>XWindows OS LAN Files</h1><p>Exchange files with this Xbox's <strong>Files / LAN Inbox</strong>. Keep XWindows OS open on the console.</p><p class="notice">This local connection uses HTTP. Pair only on a trusted home network. Files are never opened automatically.</p><label for="token">Pairing code shown on the Xbox</label><p><input id="token" type="password" autocomplete="off" spellcheck="false" maxlength="32"></p><button id="connect">Connect / Refresh</button><p><input id="file" type="file"><br><button id="upload">Upload file</button><small>Maximum 16 MiB per file. Existing files are preserved.</small></p><p id="status" role="status">Enter the pairing code to see files.</p><ul id="files"></ul><button id="more" hidden>Load more</button><small>Stop LAN transfer on the Xbox when finished. A new start creates a new pairing code.</small></main><script>(function(){'use strict';var code=document.getElementById('token'),status=document.getElementById('status'),files=document.getElementById('files'),more=document.getElementById('more'),busy=false,next=-1;var fragment=location.hash.slice(1);if(/^[a-f0-9]{32}$/i.test(fragment))code.value=fragment;try{history.replaceState(null,'',location.pathname)}catch(e){}function message(s){status.textContent=s}function api(path,options){options=options||{};options.headers=options.headers||{};options.headers.Authorization='Bearer '+code.value.trim();return fetch(path,options).then(function(r){if(!r.ok)return r.json().then(function(e){throw new Error(e.error||('HTTP '+r.status))});return r})}function refresh(append){if(busy)return;busy=true;if(!append)files.textContent='';api('/files?offset='+(append?next:0)).then(function(r){return r.json()}).then(function(data){data.entries.forEach(function(item){var li=document.createElement('li'),name=document.createElement('span'),button=document.createElement('button');name.textContent=item.name;button.textContent='Download';button.onclick=function(){if(busy)return;busy=true;message('Downloading '+item.name+'…');api('/download?id='+encodeURIComponent(item.id)).then(function(r){return r.blob()}).then(function(blob){var url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=item.name;document.body.appendChild(a);a.click();a.remove();setTimeout(function(){URL.revokeObjectURL(url)},60000);message('File sent to this browser.');}).catch(function(e){message(e.message)}).then(function(){busy=false})};li.appendChild(name);li.appendChild(button);files.appendChild(li)});next=data.nextOffset;more.hidden=next<0;message('Connected. '+files.children.length+' files shown.');}).catch(function(e){message(e.message)}).then(function(){busy=false})}document.getElementById('connect').onclick=function(){refresh(false)};more.onclick=function(){refresh(true)};document.getElementById('upload').onclick=function(){var file=document.getElementById('file').files[0];if(busy)return;if(!file){message('Choose a file first.');return}if(file.size>16777216){message('This file exceeds the 16 MiB limit.');return}busy=true;message('Uploading '+file.name+'…');api('/upload',{method:'POST',headers:{'Content-Type':'application/octet-stream','X-File-Name':encodeURIComponent(file.name)},body:file}).then(function(r){return r.json()}).then(function(data){message('Saved '+data.name+' to the Xbox.');document.getElementById('file').value='';}).catch(function(e){message(e.message)}).then(function(){busy=false;refresh(false)})};if(code.value)refresh(false)})();</script></html>)MIHAWK";
}
} // namespace Detail

class Server : public std::enable_shared_from_this<Server> {
    struct Connection {
        StreamSocket^ socket;
        DataReader^ reader;
        ThreadPoolTimer^ timer = nullptr;
        std::mutex mutex;
        bool closed = false;
        ULONGLONG began = GetTickCount64(), touched = began;
        std::uint64_t generation;
        std::string host, token;
        std::vector<unsigned char> bytes;
        Connection(StreamSocket^ value, std::uint64_t version, const std::string& address, const std::string& pairing)
            : socket(value), reader(ref new DataReader(value->InputStream)), generation(version), host(address), token(pairing) {
            reader->InputStreamOptions = InputStreamOptions::Partial;
        }
        void Touch() { std::lock_guard<std::mutex> guard(mutex); touched = GetTickCount64(); }
        void Close() {
            std::lock_guard<std::mutex> guard(mutex);
            if (closed) return;
            closed = true;
            if (timer) { timer->Cancel(); timer = nullptr; }
            try { delete socket; } catch (...) { }
        }
        void Arm(const std::shared_ptr<Connection>& self) {
            std::lock_guard<std::mutex> guard(mutex);
            if (closed) return;
            TimeSpan interval; interval.Duration = 50000000LL;
            timer = ThreadPoolTimer::CreatePeriodicTimer(ref new TimerElapsedHandler([self](ThreadPoolTimer^) {
                bool expire;
                { std::lock_guard<std::mutex> guard(self->mutex);
                    const auto now = GetTickCount64();
                    expire = now - self->touched > 15000 || now - self->began > 180000; }
                if (expire) self->Close();
            }), interval);
        }
    };
    std::mutex mutex_;
    StreamSocketListener^ listener_ = nullptr;
    StorageFolder^ inbox_ = nullptr;
    std::uint64_t generation_ = 0, nextFile_ = 1;
    bool running_ = false, starting_ = false;
    std::string host_, token_;
    Platform::String^ lastError_ = L"";
    std::uint64_t uploadedFiles_ = 0, downloadedFiles_ = 0, uploadedBytes_ = 0, downloadedBytes_ = 0;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::map<std::string, StorageFile^> files_;

    JsonObject^ Snapshot() {
        auto object = ref new JsonObject();
        object->SetNamedValue(L"running", JsonValue::CreateBooleanValue(running_));
        object->SetNamedValue(L"starting", JsonValue::CreateBooleanValue(starting_));
        const auto url = running_ ? "http://" + host_ + "/" : std::string();
        Detail::Text(object, L"url", Detail::Unicode(url));
        Detail::Text(object, L"token", Detail::Unicode(running_ ? token_ : std::string()));
        Detail::Text(object, L"pairingUrl", Detail::Unicode(running_ ? url + "#" + token_ : std::string()));
        Detail::Text(object, L"lastError", lastError_);
        Detail::Text(object, L"error", lastError_);
        Detail::Number(object, L"uploadedFiles", static_cast<double>(uploadedFiles_));
        Detail::Number(object, L"downloadedFiles", static_cast<double>(downloadedFiles_));
        Detail::Number(object, L"uploadedBytes", static_cast<double>(uploadedBytes_));
        Detail::Number(object, L"bytesReceived", static_cast<double>(uploadedBytes_));
        Detail::Number(object, L"filesReceived", static_cast<double>(uploadedFiles_));
        Detail::Number(object, L"downloadedBytes", static_cast<double>(downloadedBytes_));
        Detail::Number(object, L"maxUploadBytes", static_cast<double>(Detail::UploadLimit));
        Detail::Text(object, L"destination", L"Files / LAN Inbox");
        return object;
    }
    bool Active(std::uint64_t generation) {
        std::lock_guard<std::mutex> guard(mutex_);
        return running_ && generation_ == generation;
    }
    task<Detail::Request> Headers(const std::shared_ptr<Connection>& c) {
        const std::string current(c->bytes.begin(), c->bytes.end());
        const auto end = current.find("\r\n\r\n");
        if (end != std::string::npos) {
            auto request = Detail::Parse(current.substr(0, end + 4), c->host);
            c->bytes.erase(c->bytes.begin(), c->bytes.begin() + static_cast<std::ptrdiff_t>(end + 4));
            if (c->bytes.size() > request.length) Detail::Reject(400, "Unexpected request body.");
            return task_from_result(request);
        }
        if (c->bytes.size() >= Detail::HeaderLimit) Detail::Reject(431, "Request headers are limited to 16 KiB.");
        auto self = shared_from_this();
        return create_task(c->reader->LoadAsync(4096)).then([self, c](unsigned loaded) -> task<Detail::Request> {
            if (!loaded) Detail::Reject(400, "The request ended before its headers.");
            auto bytes = ref new Platform::Array<unsigned char>(loaded);
            c->reader->ReadBytes(bytes);
            c->bytes.insert(c->bytes.end(), bytes->Data, bytes->Data + bytes->Length);
            c->Touch();
            return self->Headers(c);
        }, task_continuation_context::use_arbitrary());
    }
    task<void> Body(const std::shared_ptr<Connection>& c, std::uint64_t length) {
        if (c->bytes.size() == length) return task_from_result();
        if (c->bytes.size() > length || length > Detail::UploadLimit) Detail::Reject(413, "Uploads are limited to 16 MiB.");
        auto self = shared_from_this();
        const auto amount = static_cast<unsigned>(std::min<std::uint64_t>(65536, length - c->bytes.size()));
        return create_task(c->reader->LoadAsync(amount)).then([self, c, length](unsigned loaded) -> task<void> {
            if (!loaded) Detail::Reject(400, "The upload was interrupted.");
            auto bytes = ref new Platform::Array<unsigned char>(loaded);
            c->reader->ReadBytes(bytes);
            c->bytes.insert(c->bytes.end(), bytes->Data, bytes->Data + bytes->Length);
            c->Touch();
            return self->Body(c, length);
        }, task_continuation_context::use_arbitrary());
    }
    task<void> SendBody(const std::shared_ptr<Connection>& c, DataWriter^ writer, IBuffer^ body, unsigned offset = 0) {
        if (!body || offset == body->Length) return task_from_result();
        const auto amount = std::min<unsigned>(65536, body->Length - offset);
        writer->WriteBuffer(body, offset, amount);
        auto self = shared_from_this();
        return Detail::Drain(writer).then([self, c, writer, body, offset, amount] {
            c->Touch();
            return self->SendBody(c, writer, body, offset + amount);
        }, task_continuation_context::use_arbitrary());
    }
    task<void> Send(const std::shared_ptr<Connection>& c, int code, const std::string& type,
        IBuffer^ body, const std::string& attachment = std::string()) {
        const auto length = body ? body->Length : 0U;
        auto writer = ref new DataWriter(c->socket->OutputStream);
        std::string header = "HTTP/1.1 " + std::to_string(code) + " " + Detail::Reason(code) +
            "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nContent-Type: " + type +
            "\r\nContent-Length: " + std::to_string(length) +
            "\r\nContent-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n";
        if (!attachment.empty()) header += "Content-Disposition: attachment; filename=\"download.bin\"; filename*=UTF-8''" + Detail::Escape(attachment) + "\r\n";
        header += "\r\n";
        writer->WriteBytes(Detail::Bytes(reinterpret_cast<const unsigned char*>(header.data()), static_cast<unsigned>(header.size())));
        auto self = shared_from_this();
        return Detail::Drain(writer).then([self, c, writer, body] { c->Touch(); return self->SendBody(c, writer, body); }, task_continuation_context::use_arbitrary())
            .then([writer] { return writer->FlushAsync(); }, task_continuation_context::use_arbitrary())
            .then([writer, c](bool flushed) {
                if (!flushed) Detail::Reject(500, "The response could not be flushed.");
                writer->DetachStream(); c->Touch();
            }, task_continuation_context::use_arbitrary());
    }
    task<void> Json(const std::shared_ptr<Connection>& c, int code, JsonObject^ object) {
        return Send(c, code, "application/json; charset=utf-8", CryptographicBuffer::ConvertStringToBinary(object->Stringify(), BinaryStringEncoding::Utf8));
    }
    task<void> List(const std::shared_ptr<Connection>& c, unsigned offset) {
        auto self = shared_from_this();
        StorageFolder^ inbox;
        { std::lock_guard<std::mutex> guard(mutex_); inbox = inbox_; }
        return create_task(inbox->GetFilesAsync(Windows::Storage::Search::CommonFileQuery::DefaultQuery, offset, Detail::FilePage)).then([self, c, offset](Windows::Foundation::Collections::IVectorView<StorageFile^>^ files) {
            auto entries = ref new JsonArray();
            { std::lock_guard<std::mutex> guard(self->mutex_);
                for (auto file : files) {
                    if (Detail::Utf8(file->Name).find(".mihawk-") == 0) continue;
                    std::string id;
                    for (const auto& existing : self->files_) if (existing.second->Path == file->Path) { id = existing.first; break; }
                    if (id.empty()) {
                        if (self->files_.size() >= 4096) Detail::Reject(503, "Restart LAN transfer to refresh its file list.");
                        id = std::to_string(self->nextFile_++); self->files_.emplace(id, file);
                    }
                    auto entry = ref new JsonObject();
                    Detail::Text(entry, L"id", Detail::Unicode(id)); Detail::Text(entry, L"name", file->Name);
                    entries->Append(entry);
                }
            }
            auto object = ref new JsonObject(); object->SetNamedValue(L"entries", entries);
            Detail::Number(object, L"nextOffset", files->Size == Detail::FilePage ? offset + Detail::FilePage : -1.0);
            return self->Json(c, 200, object);
        }, task_continuation_context::use_arbitrary());
    }
    task<void> Download(const std::shared_ptr<Connection>& c, const std::string& id) {
        Detail::Decimal(id, std::numeric_limits<std::uint64_t>::max());
        StorageFile^ file = nullptr;
        { std::lock_guard<std::mutex> guard(mutex_); const auto found = files_.find(id); if (found != files_.end()) file = found->second; }
        if (!file) Detail::Reject(404, "Refresh the inbox before downloading this file.");
        auto self = shared_from_this();
        return create_task(file->OpenReadAsync()).then([](IRandomAccessStreamWithContentType^ stream) -> task<IBuffer^> {
            if (stream->Size > Detail::UploadLimit) { delete stream; Detail::Reject(413, "Downloads are limited to 16 MiB in this version."); }
            const auto length = static_cast<unsigned>(stream->Size);
            auto reader = ref new DataReader(stream->GetInputStreamAt(0));
            // The exact requested length remains bounded even if the file is
            // concurrently replaced or grows after its size was checked.
            return create_task(reader->LoadAsync(length)).then([stream, reader, length](task<unsigned> read) -> IBuffer^ {
                try {
                    if (read.get() != length) Detail::Reject(500, "The file changed while it was being read.");
                    auto buffer = reader->ReadBuffer(length);
                    reader->DetachStream(); delete stream; return buffer;
                } catch (...) { delete stream; throw; }
            }, task_continuation_context::use_arbitrary());
        }, task_continuation_context::use_arbitrary()).then([self, c, file](IBuffer^ buffer) {
            const auto length = buffer->Length;
            return self->Send(c, 200, "application/octet-stream", buffer, Detail::Utf8(file->Name)).then([self, length] {
                std::lock_guard<std::mutex> guard(self->mutex_); ++self->downloadedFiles_; self->downloadedBytes_ += length;
            }, task_continuation_context::use_arbitrary());
        }, task_continuation_context::use_arbitrary());
    }
    task<void> Upload(const std::shared_ptr<Connection>& c, const Detail::Request& request) {
        if (!request.headers.count("content-length")) Detail::Reject(411, "Content-Length is required.");
        const auto found = request.headers.find("x-file-name");
        if (found == request.headers.end()) Detail::Reject(400, "X-File-Name is required.");
        Platform::String^ name = nullptr;
        const auto decoded = Detail::Unescape(found->second);
        if (!Detail::Utf8Filename(decoded)) Detail::Reject(400, "The filename must be valid UTF-8.");
        try { name = Detail::Unicode(decoded); }
        catch (Platform::Exception^) { Detail::Reject(400, "The filename must be valid UTF-8."); }
        if (!name || name->Length() > 120) Detail::Reject(400, "Filenames are limited to 120 characters.");
        auto self = shared_from_this();
        struct TemporaryFile { StorageFile^ file = nullptr; };
        auto temporary = std::make_shared<TemporaryFile>();
        return Body(c, request.length).then([self, c, temporary] {
            if (!self->Active(c->generation)) Detail::Reject(503, "LAN transfer has stopped.");
            StorageFolder^ inbox;
            { std::lock_guard<std::mutex> guard(self->mutex_); inbox = self->inbox_; }
            return inbox->CreateFileAsync(Detail::Unicode(".mihawk-upload-" + Detail::RandomToken() + ".part"), CreationCollisionOption::FailIfExists);
        }, task_continuation_context::use_arbitrary()).then([c, temporary](StorageFile^ file) {
            temporary->file = file; return file->OpenTransactedWriteAsync();
        }, task_continuation_context::use_arbitrary()).then([c](StorageStreamTransaction^ transaction) {
            transaction->Stream->Size = 0;
            auto writer = ref new DataWriter(transaction->Stream->GetOutputStreamAt(0));
            if (!c->bytes.empty()) writer->WriteBytes(Detail::Bytes(c->bytes.data(), static_cast<unsigned>(c->bytes.size())));
            return Detail::Drain(writer).then([writer] { return writer->FlushAsync(); }, task_continuation_context::use_arbitrary())
                .then([transaction, writer](bool flushed) {
                    if (!flushed) Detail::Reject(500, "The upload could not be flushed.");
                    writer->DetachStream(); return transaction->CommitAsync();
                }, task_continuation_context::use_arbitrary()).then([transaction, writer](task<void> done) {
                    try { done.get(); } catch (...) { delete transaction; throw; }
                    delete transaction;
                }, task_continuation_context::use_arbitrary());
        }, task_continuation_context::use_arbitrary()).then([self, c, temporary, name] {
            if (!self->Active(c->generation)) Detail::Reject(503, "LAN transfer has stopped.");
            // Atomic rename happens only after a complete transaction. A unique
            // name preserves every existing file; no delete/overwrite API exists.
            return temporary->file->RenameAsync(name, NameCollisionOption::GenerateUniqueName);
        }, task_continuation_context::use_arbitrary()).then([self, c, temporary, length = request.length] {
            auto object = ref new JsonObject(); Detail::Text(object, L"name", temporary->file->Name);
            object->SetNamedValue(L"saved", JsonValue::CreateBooleanValue(true));
            temporary->file = nullptr;
            { std::lock_guard<std::mutex> guard(self->mutex_); ++self->uploadedFiles_; self->uploadedBytes_ += length; }
            c->bytes.clear(); c->bytes.shrink_to_fit();
            return self->Json(c, 201, object);
        }, task_continuation_context::use_arbitrary()).then([temporary](task<void> done) -> task<void> {
            try { done.get(); return task_from_result(); }
            catch (...) {
                auto error = std::current_exception();
                if (!temporary->file) std::rethrow_exception(error);
                return create_task(temporary->file->DeleteAsync(StorageDeleteOption::PermanentDelete)).then([error](task<void> cleanup) {
                    try { cleanup.get(); } catch (...) { }
                    std::rethrow_exception(error);
                }, task_continuation_context::use_arbitrary());
            }
        }, task_continuation_context::use_arbitrary());
    }
    task<void> Route(const std::shared_ptr<Connection>& c, const Detail::Request& request) {
        if (!Active(c->generation)) Detail::Reject(503, "LAN transfer has stopped.");
        if (request.method == "GET" && request.target == "/")
            return Send(c, 200, "text/html; charset=utf-8", CryptographicBuffer::ConvertStringToBinary(Detail::Unicode(Detail::Page()), BinaryStringEncoding::Utf8));
        const auto auth = request.headers.find("authorization");
        if (auth == request.headers.end() || auth->second.substr(0, 7) != "Bearer " ||
            !Detail::EqualToken(auth->second.substr(7), c->token)) Detail::Reject(401, "The pairing code is missing or incorrect.");
        if (request.method == "GET" && request.target.find("/files?offset=") == 0)
            return List(c, static_cast<unsigned>(Detail::Decimal(request.target.substr(14), 1000000)));
        if (request.method == "GET" && request.target == "/files") return List(c, 0);
        if (request.method == "GET" && request.target.find("/download?id=") == 0) return Download(c, request.target.substr(13));
        if (request.method == "POST" && request.target == "/upload") return Upload(c, request);
        Detail::Reject(404, "This route does not exist.");
    }
    void Accept(StreamSocket^ socket) {
        std::shared_ptr<Connection> c;
        { std::lock_guard<std::mutex> guard(mutex_);
            if (!running_ || connections_.size() >= 2) { delete socket; return; }
            c = std::make_shared<Connection>(socket, generation_, host_, token_); connections_.push_back(c);
        }
        c->Arm(c);
        auto self = shared_from_this();
        // No synchronous waits or UI-thread I/O: reads, writes and storage all
        // continue on arbitrary worker contexts, with two bounded requests max.
        task_from_result().then([self, c] { return self->Headers(c); }, task_continuation_context::use_arbitrary())
            .then([self, c](const Detail::Request& request) { return self->Route(c, request); }, task_continuation_context::use_arbitrary())
            .then([self, c](task<void> result) -> task<void> {
                int code = 500; Platform::String^ message = L"The transfer failed. Please retry.";
                try { result.get(); return task_from_result(); }
                catch (const Detail::HttpError& error) { code = error.code; message = Detail::Unicode(error.message); }
                catch (Platform::Exception^ error) {
                    std::lock_guard<std::mutex> guard(self->mutex_); self->lastError_ = error->Message;
                }
                catch (...) { }
                auto object = ref new JsonObject(); Detail::Text(object, L"error", message);
                try { return self->Json(c, code, object); } catch (...) { return task_from_result(); }
            }, task_continuation_context::use_arbitrary()).then([self, c](task<void> completed) {
                try { completed.get(); } catch (...) { }
                c->Close();
                std::lock_guard<std::mutex> guard(self->mutex_);
                self->connections_.erase(std::remove(self->connections_.begin(), self->connections_.end(), c), self->connections_.end());
            }, task_continuation_context::use_arbitrary());
    }

public:
    ~Server() { try { Stop(); } catch (...) { } }
    JsonObject^ Status() { std::lock_guard<std::mutex> guard(mutex_); return Snapshot(); }
    JsonObject^ Stop() {
        StreamSocketListener^ listener;
        std::vector<std::shared_ptr<Connection>> connections;
        JsonObject^ result;
        { std::lock_guard<std::mutex> guard(mutex_);
            ++generation_; running_ = starting_ = false;
            listener = listener_; listener_ = nullptr;
            token_.clear(); host_.clear(); files_.clear(); inbox_ = nullptr;
            connections.swap(connections_); result = Snapshot();
        }
        if (listener) try { delete listener; } catch (...) { }
        for (const auto& connection : connections) connection->Close();
        return result;
    }
    task<JsonObject^> StartAsync(StorageFolder^ workspace) {
        std::uint64_t generation;
        { std::lock_guard<std::mutex> guard(mutex_);
            if (running_ || starting_) return task_from_result(Snapshot());
            generation = ++generation_; starting_ = true; lastError_ = L"";
        }
        auto self = shared_from_this();
        return task_from_result().then([self, workspace, generation]() -> task<void> {
            if (!workspace) Detail::Reject(500, "The workspace is not ready.");
            std::string address;
            for (auto host : NetworkInformation::GetHostNames()) {
                if (host->Type != HostNameType::Ipv4 || !host->IPInformation || !host->IPInformation->NetworkAdapter) continue;
                auto candidate = Detail::Utf8(host->CanonicalName);
                if (Detail::PrivateIpv4(candidate)) { address = candidate; break; }
            }
            if (address.empty()) Detail::Reject(503, "No private LAN IPv4 address is available. Connect the Xbox to your home network.");
            const auto pairing = Detail::RandomToken();
            return create_task(workspace->CreateFolderAsync(L"LAN Inbox", CreationCollisionOption::OpenIfExists))
                .then([self, generation, address, pairing](StorageFolder^ inbox) {
                    auto listener = ref new StreamSocketListener();
                    listener->Control->QualityOfService = SocketQualityOfService::Normal;
                    std::weak_ptr<Server> weak(self);
                    listener->ConnectionReceived += ref new TypedEventHandler<StreamSocketListener^, StreamSocketListenerConnectionReceivedEventArgs^>(
                        [weak](StreamSocketListener^, StreamSocketListenerConnectionReceivedEventArgs^ args) {
                            if (auto server = weak.lock()) {
                                try { server->Accept(args->Socket); } catch (...) { try { delete args->Socket; } catch (...) { } }
                            } else delete args->Socket;
                        });
                    { std::lock_guard<std::mutex> guard(self->mutex_);
                        if (self->generation_ != generation || !self->starting_) { delete listener; Detail::Reject(503, "LAN transfer start was cancelled."); }
                        self->listener_ = listener; self->inbox_ = inbox;
                        self->host_ = address + ":48080"; self->token_ = pairing;
                    }
                    return listener->BindEndpointAsync(ref new HostName(Detail::Unicode(address)), L"48080");
                }, task_continuation_context::use_arbitrary());
        }, task_continuation_context::use_arbitrary()).then([self, generation](task<void> started) {
            StreamSocketListener^ failed = nullptr;
            Platform::String^ message = L"";
            bool success = false;
            try { started.get(); success = true; }
            catch (const Detail::HttpError& error) { message = Detail::Unicode(error.message); }
            catch (Platform::Exception^ error) { message = error->Message; }
            catch (...) { message = L"LAN transfer could not start."; }
            JsonObject^ result;
            { std::lock_guard<std::mutex> guard(self->mutex_);
                if (self->generation_ == generation) {
                    self->starting_ = false; self->running_ = success;
                    self->lastError_ = message;
                    if (!success) { failed = self->listener_; self->listener_ = nullptr; self->inbox_ = nullptr; self->host_.clear(); self->token_.clear(); }
                }
                result = self->Snapshot();
            }
            if (failed) try { delete failed; } catch (...) { }
            return result;
        }, task_continuation_context::use_arbitrary());
    }
};
} // namespace MihawkLan
#endif
