// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cstdint>
#include <limits>

namespace protondesktop::uwp
{
    // Winsock SOCKET is pointer-sized. The pinned upstream wake channel stores
    // signed 32-bit descriptors, so reject values that cannot round-trip.
    constexpr bool FitsUpstreamSocketDescriptor(std::uint64_t socket) noexcept
    {
        return socket <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    }
#ifdef _WIN32
    // Caller owns Winsock startup/cleanup. Success returns two real blocking,
    // connected IPv4 loopback TCP sockets usable with select/send/recv.
    // Failure returns SOCKET_ERROR, publishes {-1,-1}, closes all intermediate
    // sockets and retains WSAGetLastError. Timeout must be 1..60000 milliseconds.
    // Same-process AppContainer networking/capabilities need runtime testing;
    // API-family compilation or a desktop test does not qualify Xbox behavior.
    int CreateNativeSocketPair(std::int32_t sockets[2], std::uint32_t timeoutMilliseconds = 5000) noexcept;
    // Closes owned nonnegative descriptors once, resets both to -1, and keeps
    // the caller's WSA last error. Does not balance any WSAStartup reference.
    void CloseNativeSocketPair(std::int32_t sockets[2]) noexcept;
#endif
}
