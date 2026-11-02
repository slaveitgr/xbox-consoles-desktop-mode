// SPDX-License-Identifier: MIT
// Standalone hostile-input checks: c++ -std=c++17 -fsanitize=address,undefined
// -fno-omit-frame-pointer tests/lan-transfer-tests.cpp -o /tmp/mihawk-lan-tests
#define MIHAWK_LAN_PORTABLE_ONLY
#include "../desktop/native/LanTransfer.h"
#include <cstdlib>
#include <iostream>

using namespace MihawkLan::Detail;
unsigned checks = 0;
void Check(bool value, const char* label) {
    ++checks;
    if (!value) { std::cerr << "FAILED: " << label << '\n'; std::exit(1); }
}
template<class Function> void Rejected(Function operation, const char* label) {
    bool rejected = false;
    try { operation(); } catch (const HttpError&) { rejected = true; }
    Check(rejected, label);
}
std::string RequestText(const std::string& extra = {}, const std::string& method = "GET", const std::string& route = "/files") {
    return method + " " + route + " HTTP/1.1\r\nHost: 192.0.2.1:48080\r\n" + extra + "\r\n";
}
int main() {
    constexpr char host[] = "192.0.2.1:48080";
    auto valid = Parse(RequestText("Authorization: Bearer 0123456789abcdef0123456789abcdef\r\n"), host);
    Check(valid.method == "GET" && valid.target == "/files" && valid.length == 0, "valid request");
    Check(valid.headers.at("authorization") == "Bearer 0123456789abcdef0123456789abcdef", "authorization preserved");
    Check(Parse(RequestText("Origin: http://192.0.2.1:48080\r\n"), host).length == 0, "same origin");
    Check(Parse(RequestText("Content-Length: 16777216\r\n", "POST", "/upload"), host).length == UploadLimit, "upload limit boundary");
    Check(Parse(RequestText("Content-Length: 0\r\n", "POST", "/upload"), host).length == 0, "empty upload");
    Rejected([&] { Parse(RequestText("Content-Length: 16777217\r\n", "POST"), host); }, "oversized content length");
    Rejected([&] { Parse(RequestText("Content-Length: 0\r\nContent-Length: 1\r\n", "POST"), host); }, "duplicate length");
    Rejected([&] { Parse(RequestText("Authorization: first\r\nauthorization: second\r\n"), host); }, "duplicate case folded authorization");
    Rejected([&] { Parse(RequestText("Transfer-Encoding: chunked\r\n", "POST"), host); }, "chunked upload");
    Rejected([&] { Parse(RequestText("Transfer-Encoding: identity\r\n", "POST"), host); }, "all transfer encodings rejected");
    Rejected([&] { Parse(RequestText("Origin: https://attacker.example\r\n"), host); }, "cross origin");
    Rejected([&] { Parse(RequestText("Origin: null\r\n"), host); }, "opaque origin");
    Rejected([&] { Parse(RequestText("Origin: \r\n"), host); }, "empty origin");
    Rejected([&] { Parse(RequestText(), "attacker.example:48080"); }, "wrong host");
    Rejected([&] { Parse("GET / HTTP/1.1\r\n\r\n", host); }, "missing host");
    Rejected([&] { Parse(RequestText("Folded: first\r\n second\r\n"), host); }, "folded headers rejected");
    Rejected([&] { Parse(RequestText("Bad Name: value\r\n"), host); }, "header name whitespace");
    Rejected([&] { Parse(RequestText("X-A: value\tvalue\r\n"), host); }, "control in header value");
    Rejected([&] { Parse(RequestText("Content-Length: 1\r\n"), host); }, "GET body");
    Rejected([&] { Parse(RequestText({}, "DELETE"), host); }, "delete unsupported");
    Rejected([&] { Parse(RequestText({}, "GET", "http://attacker.example/"), host); }, "absolute target");
    Rejected([&] { Parse(RequestText({}, "GET", "/#secret"), host); }, "fragment in HTTP request");
    Rejected([&] { Parse(RequestText({}, "GET", "/one two"), host); }, "space in target");
    Rejected([&] { Parse("GET / HTTP/1.1\r\n", host); }, "incomplete headers");
    Rejected([&] { Parse(RequestText() + "extra", host); }, "bytes after header framing");
    Rejected([&] { Parse(RequestText("X-Pad: " + std::string(HeaderLimit, 'a') + "\r\n"), host); }, "header total bound");
    std::string many;
    for (unsigned i = 0; i < 65; ++i) many += "X-" + std::to_string(i) + ": a\r\n";
    Rejected([&] { Parse(RequestText(many), host); }, "header count bound");
    for (const auto& bad : {"", "-1", "+1", "1x", " 1", "1 ", "18446744073709551616", "99999999999999999999999999999999999"})
        Rejected([&] { Decimal(bad, UINT64_MAX); }, "malformed integer");
    Check(Decimal("18446744073709551615", UINT64_MAX) == UINT64_MAX, "uint64 maximum");
    Check(Decimal("000001", UploadLimit) == 1, "decimal leading zeroes");
    const std::string token = "0123456789abcdef0123456789abcdef";
    Check(EqualToken(token, token), "correct token");
    Check(!EqualToken("", token) && !EqualToken(token + "x", token), "token length");
    for (std::size_t i = 0; i < token.size(); ++i) { auto wrong = token; wrong[i] = 'x'; Check(!EqualToken(wrong, token), "each token byte checked"); }
    Check(!EqualToken("", ""), "no empty token authentication");
    Check(Unescape("photo%20one.jpg") == "photo one.jpg", "filename spaces");
    Check(Unescape("%CE%91%CF%81%CF%87%CE%B5%CE%AF%CE%BF.txt") == "Αρχείο.txt", "UTF-8 filename");
    Check(Unescape("%252e%252e.txt") == "%2e%2e.txt", "single decoding only");
    for (const auto& bad : {"", ".", "..", "../file", "..%2ffile", "..%5Cfile", "%00x", "%0dx", "C%3Afile", "hello%3F.txt",
        "name.", "name%20", "CON", "con.txt", "con%20.txt", "LPT1.txt", "COM9.zip", ".mihawk-upload-x.part", "%GG", "%1"})
        Rejected([&] { Unescape(bad); }, "unsafe filename");
    Check(Unescape("comfortable.txt") == "comfortable.txt", "ordinary reserved prefix");
    Check(Utf8Filename("Αρχείο.txt") && Utf8Filename("emoji-\xF0\x9F\x8E\xAE.png"), "valid Unicode scalars");
    for (const auto& bad : {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xE2\x82", "\x80", "\xC2x"})
        Check(!Utf8Filename(bad), "invalid UTF-8 scalar");
    Check(Escape("hello\r\n.txt") == "hello%0D%0A.txt", "attachment header injection encoded");
    for (const auto& good : {"10.0.0.1", "172.16.0.1", "172.31.255.254", "192.168.42.10"}) Check(PrivateIpv4(good), "private LAN address");
    for (const auto& bad : {"127.0.0.1", "169.254.0.1", "0.0.0.0", "8.8.8.8", "172.15.0.1", "172.32.0.1", "192.169.0.1", "10.0.0.256", "10.0.0", "10.0.0.1.2", "10..0.1", "10.0.0.-1", "10.0.0.1evil"})
        Check(!PrivateIpv4(bad), "non-LAN address");
    // Deterministic malformed headers exercise parser bounds under ASan/UBSan.
    std::uint32_t random = 0x5A17BEEF;
    const auto original = RequestText("Authorization: Bearer " + token + "\r\n");
    for (unsigned run = 0; run < 25000; ++run) {
        auto mutation = original;
        for (unsigned edit = 0; edit < run % 9 + 1; ++edit) {
            random = random * 1664525U + 1013904223U;
            mutation[random % mutation.size()] = static_cast<char>(random >> 24);
        }
        try { Parse(mutation, host); } catch (const HttpError&) { }
    }
    std::cout << checks << " LAN parser/security checks passed; 25000 malformed headers exercised.\n";
}
