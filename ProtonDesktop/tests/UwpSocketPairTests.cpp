// SPDX-License-Identifier: GPL-2.0-only
#include "backend/uwp/UwpSocketPair.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#endif
using namespace protondesktop::uwp;
namespace
{
    unsigned checks = 0;
    void Require(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
#ifdef _WIN32
            throw std::runtime_error(std::string(message) + " (WSA " + std::to_string(WSAGetLastError()) + ")");
#else
            throw std::runtime_error(message);
#endif
        }
    }
    void PolicyChecks()
    {
        Require(FitsUpstreamSocketDescriptor(0), "SOCKET zero remains valid");
        Require(FitsUpstreamSocketDescriptor(0x7FFFFFFFULL), "maximum signed32 SOCKET is representable");
        Require(!FitsUpstreamSocketDescriptor(0x80000000ULL), "signed32 overflow is rejected");
        Require(!FitsUpstreamSocketDescriptor(0x100000001ULL), "pointer-sized SOCKET is not truncated");
        Require(!FitsUpstreamSocketDescriptor(0xFFFFFFFFFFFFFFFFULL), "INVALID_SOCKET cannot become a descriptor");
    }
#ifdef _WIN32
    struct Winsock
    {
        bool active = false;
        ~Winsock() { if (active) WSACleanup(); }
    };
    struct Pair
    {
        std::int32_t sockets[2]{-1,-1};
        ~Pair() { CloseNativeSocketPair(sockets); }
        SOCKET At(unsigned index) const { return static_cast<SOCKET>(sockets[index]); }
    };
    bool Ready(SOCKET socket, bool writable)
    {
        fd_set set{};
        FD_ZERO(&set); FD_SET(socket, &set);
        timeval timeout{2,0};
        const auto result = select(0, writable ? nullptr : &set, writable ? &set : nullptr, nullptr, &timeout);
        return result == 1 && FD_ISSET(socket, &set);
    }
    void SendAll(SOCKET socket, const char* bytes, int size)
    {
        for (int offset = 0; offset < size;)
        {
            Require(Ready(socket, true), "real select reports write readiness within deadline");
            const auto result = send(socket, bytes + offset, size - offset, 0);
            Require(result > 0, "real send makes progress");
            offset += result;
        }
    }
    void ReceiveExact(SOCKET socket, const char* expected, int size)
    {
        std::array<char,64> actual{};
        Require(size <= static_cast<int>(actual.size()), "bounded diagnostic payload");
        for (int offset = 0; offset < size;)
        {
            Require(Ready(socket, false), "real select reports read readiness within deadline");
            const auto result = recv(socket, actual.data() + offset, size - offset, 0);
            Require(result > 0, "real recv makes progress");
            offset += result;
        }
        Require(std::memcmp(expected, actual.data(), size) == 0, "exact stream bytes including embedded NUL");
    }
    sockaddr_in Address(SOCKET socket, bool peer)
    {
        sockaddr_in address{};
        int size = sizeof(address);
        const auto result = peer ? getpeername(socket, reinterpret_cast<sockaddr*>(&address), &size) :
                                   getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size);
        Require(result == 0 && size == sizeof(address), "real connected socket address");
        Require(address.sin_family == AF_INET && address.sin_addr.s_addr == htonl(INADDR_LOOPBACK),
                "wake channel stays on IPv4 loopback");
        return address;
    }
    void RequireClosed(SOCKET socket)
    {
        int type = 0, size = sizeof(type);
        WSASetLastError(0);
        Require(getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size) == SOCKET_ERROR &&
                WSAGetLastError() == WSAENOTSOCK, "released endpoint no longer names an open socket");
    }
    void NativeChecks()
    {
        WSASetLastError(0);
        Require(CreateNativeSocketPair(nullptr) == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL,
                "null output fails explicitly");
        std::int32_t invalid[2]{123,456};
        Require(CreateNativeSocketPair(invalid, 0) == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL &&
                invalid[0] == -1 && invalid[1] == -1, "zero timeout rejects and clears output");
        Require(CreateNativeSocketPair(invalid, 60001) == SOCKET_ERROR && WSAGetLastError() == WSAEINVAL &&
                invalid[0] == -1 && invalid[1] == -1, "unbounded creation timeout is rejected");
        Require(CreateNativeSocketPair(invalid) == SOCKET_ERROR && WSAGetLastError() == WSANOTINITIALISED &&
                invalid[0] == -1 && invalid[1] == -1, "missing caller WSAStartup preserves failure without fake descriptors");
        Winsock startup;
        WSADATA data{};
        const auto start = WSAStartup(MAKEWORD(2,2), &data);
        Require(start == 0, "caller starts real Winsock 2.2");
        startup.active = true;
        Require(data.wVersion == MAKEWORD(2,2), "provider negotiates Winsock 2.2");
        Pair pair;
        Require(CreateNativeSocketPair(pair.sockets) == 0 && pair.sockets[0] >= 0 && pair.sockets[1] >= 0 &&
                pair.sockets[0] != pair.sockets[1], "create two distinct real upstream-compatible sockets");
        const auto local0 = Address(pair.At(0), false), peer0 = Address(pair.At(0), true);
        const auto local1 = Address(pair.At(1), false), peer1 = Address(pair.At(1), true);
        Require(local0.sin_port == peer1.sin_port && local1.sin_port == peer0.sin_port,
                "both endpoints identify each other");
        for (unsigned index = 0; index < 2; ++index)
        {
            int type = 0, size = sizeof(type);
            Require(getsockopt(pair.At(index), SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &size) == 0 &&
                    type == SOCK_STREAM, "endpoint is a real TCP stream");
            u_long mode = 1;
            Require(ioctlsocket(pair.At(index), FIONBIO, &mode) == 0, "caller can enable nonblocking wake semantics");
            char byte = 0;
            Require(recv(pair.At(index), &byte, 1, 0) == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK,
                    "empty nonblocking wake stream reports real would-block");
        }
        const char left[] = {'w','a','k','e','\0','0'};
        const char right[] = {'w','a','k','e','\0','1'};
        SendAll(pair.At(0), left, sizeof(left));
        ReceiveExact(pair.At(1), left, sizeof(left));
        SendAll(pair.At(1), right, sizeof(right));
        ReceiveExact(pair.At(0), right, sizeof(right));
        Require(shutdown(pair.At(0), SD_SEND) == 0, "real half-close shuts down one send direction");
        Require(Ready(pair.At(1), false), "EOF is reported as readable");
        char byte = 0;
        Require(recv(pair.At(1), &byte, 1, 0) == 0, "peer observes real stream EOF");
        SendAll(pair.At(1), right, sizeof(right));
        ReceiveExact(pair.At(0), right, sizeof(right));
        Require(send(pair.At(0), left, sizeof(left), 0) == SOCKET_ERROR && WSAGetLastError() == WSAESHUTDOWN,
                "send after half-close reports actual shutdown error");
        const SOCKET closed0 = pair.At(0), closed1 = pair.At(1);
        WSASetLastError(WSAEACCES);
        CloseNativeSocketPair(pair.sockets);
        Require(pair.sockets[0] == -1 && pair.sockets[1] == -1 && WSAGetLastError() == WSAEACCES,
                "close clears descriptors and preserves caller error");
        RequireClosed(closed0); RequireClosed(closed1);
        WSASetLastError(WSAECONNRESET);
        CloseNativeSocketPair(pair.sockets); CloseNativeSocketPair(nullptr);
        Require(WSAGetLastError() == WSAECONNRESET, "repeated and null cleanup preserve error");
        for (unsigned iteration = 0; iteration < 8; ++iteration)
        {
            Pair repeated;
            Require(CreateNativeSocketPair(repeated.sockets) == 0, "repeat real pair lifecycle");
            const auto first = repeated.At(0), second = repeated.At(1);
            CloseNativeSocketPair(repeated.sockets);
            RequireClosed(first); RequireClosed(second);
        }
        Require(WSACleanup() == 0, "caller releases its Winsock reference");
        startup.active = false;
        Require(CreateNativeSocketPair(invalid) == SOCKET_ERROR && WSAGetLastError() == WSANOTINITIALISED &&
                invalid[0] == -1 && invalid[1] == -1, "pair implementation does not leak hidden WSAStartup references");
    }
#endif
}
int main()
{
    try
    {
        PolicyChecks();
#ifdef _WIN32
        NativeChecks();
        std::printf("UWP socketpair: %u checks passed (actual Windows Winsock; no AppContainer or Xbox execution)\n", checks);
#else
        std::printf("UWP socket descriptor policy: %u checks passed (Windows networking not executed)\n", checks);
#endif
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "UWP socketpair check %u failed: %s\n", checks, error.what());
        return 1;
    }
}
