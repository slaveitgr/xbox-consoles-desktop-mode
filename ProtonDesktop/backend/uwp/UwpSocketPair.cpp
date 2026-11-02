// SPDX-License-Identifier: GPL-2.0-only
#include "UwpSocketPair.h"
#ifndef _WIN32
#error UwpSocketPair.cpp requires Windows Winsock headers.
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

namespace protondesktop::uwp
{
    namespace
    {
        struct RestoreFailure
        {
            int error = 0;
            ~RestoreFailure() { if (error) WSASetLastError(error); }
            int Fail(int code) noexcept { error = code; return SOCKET_ERROR; }
        };
        struct OwnedSocket
        {
            SOCKET value = INVALID_SOCKET;
            ~OwnedSocket() { if (value != INVALID_SOCKET) closesocket(value); }
            SOCKET Release() noexcept { const auto result = value; value = INVALID_SOCKET; return result; }
            void Reset() noexcept { if (value != INVALID_SOCKET) closesocket(value); value = INVALID_SOCKET; }
        };
        struct Deadline
        {
            ULONGLONG started;
            DWORD milliseconds;
            DWORD Remaining() const noexcept
            {
                const auto elapsed = GetTickCount64() - started;
                return elapsed >= milliseconds ? 0 : milliseconds - static_cast<DWORD>(elapsed);
            }
        };
        bool SetBlocking(SOCKET socket, bool blocking) noexcept
        {
            u_long mode = blocking ? 0 : 1;
            return ioctlsocket(socket, FIONBIO, &mode) != SOCKET_ERROR;
        }
        bool WaitReady(SOCKET socket, bool writable, const Deadline& deadline) noexcept
        {
            for (;;)
            {
                const auto remaining = deadline.Remaining();
                if (!remaining) { WSASetLastError(WSAETIMEDOUT); return false; }
                fd_set ready{}, failed{};
                FD_ZERO(&ready); FD_SET(socket, &ready);
                FD_ZERO(&failed); FD_SET(socket, &failed);
                timeval timeout{};
                timeout.tv_sec = static_cast<long>(remaining / 1000);
                timeout.tv_usec = static_cast<long>((remaining % 1000) * 1000);
                const auto count = select(0, writable ? nullptr : &ready,
                    writable ? &ready : nullptr, &failed, &timeout);
                if (count == SOCKET_ERROR)
                {
                    if (WSAGetLastError() == WSAEINTR) continue;
                    return false;
                }
                if (!count) { WSASetLastError(WSAETIMEDOUT); return false; }
                if (FD_ISSET(socket, &failed))
                {
                    int error = 0, size = sizeof(error);
                    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == SOCKET_ERROR)
                        return false;
                    WSASetLastError(error ? error : WSAECONNABORTED);
                    return false;
                }
                if (FD_ISSET(socket, &ready)) return true;
            }
        }
    }
    int CreateNativeSocketPair(std::int32_t sockets[2], std::uint32_t timeoutMilliseconds) noexcept
    {
        if (!sockets) { WSASetLastError(WSAEINVAL); return SOCKET_ERROR; }
        sockets[0] = sockets[1] = -1;
        if (!timeoutMilliseconds || timeoutMilliseconds > 60000)
        { WSASetLastError(WSAEINVAL); return SOCKET_ERROR; }
        // Declared first so captured errors are restored after every socket's
        // cleanup, including closesocket calls on partially-built pairs.
        RestoreFailure failure;
        OwnedSocket listener, client, peer;
        const Deadline deadline{GetTickCount64(), timeoutMilliseconds};
        listener.value = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
        if (listener.value == INVALID_SOCKET) return failure.Fail(WSAGetLastError());
        const BOOL exclusive = TRUE;
        if (setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR)
            return failure.Fail(WSAGetLastError());
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
            return failure.Fail(WSAGetLastError());
        int size = sizeof(address);
        if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size) == SOCKET_ERROR)
            return failure.Fail(WSAGetLastError());
        if (listen(listener.value, 1) == SOCKET_ERROR || !SetBlocking(listener.value, false))
            return failure.Fail(WSAGetLastError());
        client.value = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
        if (client.value == INVALID_SOCKET || !SetBlocking(client.value, false))
            return failure.Fail(WSAGetLastError());
        if (connect(client.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
        {
            const auto error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEALREADY)
                return failure.Fail(error);
            if (!WaitReady(client.value, true, deadline)) return failure.Fail(WSAGetLastError());
            int connectError = 0; size = sizeof(connectError);
            if (getsockopt(client.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connectError), &size) == SOCKET_ERROR)
                return failure.Fail(WSAGetLastError());
            if (connectError) return failure.Fail(connectError);
        }
        sockaddr_in clientLocal{}; size = sizeof(clientLocal);
        if (getsockname(client.value, reinterpret_cast<sockaddr*>(&clientLocal), &size) == SOCKET_ERROR)
            return failure.Fail(WSAGetLastError());
        for (;;)
        {
            if (!WaitReady(listener.value, false, deadline)) return failure.Fail(WSAGetLastError());
            sockaddr_in acceptedAddress{}; size = sizeof(acceptedAddress);
            peer.value = accept(listener.value, reinterpret_cast<sockaddr*>(&acceptedAddress), &size);
            if (peer.value == INVALID_SOCKET)
            {
                if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
                return failure.Fail(WSAGetLastError());
            }
            // A local process can race a TCP listener. Only accept the endpoint
            // belonging to the socket we just connected, never an unrelated peer.
            if (size == sizeof(acceptedAddress) && acceptedAddress.sin_family == AF_INET &&
                acceptedAddress.sin_addr.s_addr == clientLocal.sin_addr.s_addr &&
                acceptedAddress.sin_port == clientLocal.sin_port) break;
            peer.Reset();
        }
        if (!FitsUpstreamSocketDescriptor(static_cast<std::uint64_t>(client.value)) ||
            !FitsUpstreamSocketDescriptor(static_cast<std::uint64_t>(peer.value)))
            return failure.Fail(WSAEMFILE);
        const BOOL noDelay = TRUE;
        if (setsockopt(client.value, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay)) == SOCKET_ERROR ||
            setsockopt(peer.value, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay)) == SOCKET_ERROR ||
            !SetBlocking(client.value, true) || !SetBlocking(peer.value, true))
            return failure.Fail(WSAGetLastError());
        sockets[0] = static_cast<std::int32_t>(client.Release());
        sockets[1] = static_cast<std::int32_t>(peer.Release());
        return 0;
    }
    void CloseNativeSocketPair(std::int32_t sockets[2]) noexcept
    {
        const auto error = WSAGetLastError();
        if (sockets)
        {
            const auto first = sockets[0], second = sockets[1];
            sockets[0] = sockets[1] = -1;
            if (first >= 0) closesocket(static_cast<SOCKET>(first));
            if (second >= 0 && second != first) closesocket(static_cast<SOCKET>(second));
        }
        WSASetLastError(error);
    }
}
