#include "Sockets.h"
#include "lualib.h"

#include <string>

namespace Luwow::Net {
    using StateRelease = Luwow::Engine::StateRelease;

    // Reads a string or buffer argument without copying
    static const char* checkData(lua_State* L, int index, size_t* length) {
        if (lua_isbuffer(L, index)) {
            return static_cast<const char*>(lua_tobuffer(L, index, length));
        }
        return luaL_checklstring(L, index, length);
    }

    static int checkCount(lua_State* L, int index, int fallback) {
        int count = luaL_optinteger(L, index, fallback);
        if (count <= 0) luaL_argerror(L, index, "count must be positive");
        return count;
    }

    // SocketBase

    SocketBase::SocketBase(ILuauHost* host, SocketHandle handle) : host(host), handle(handle) {}

    SocketBase::~SocketBase() {
        releaseHandle();
    }

    void SocketBase::releaseHandle() {
        if (handle != InvalidSocket) {
            closeSocket(handle);
            handle = InvalidSocket;
        }
    }

    SocketBase::Operation::Operation(SocketBase& socket, lua_State* L) : socket(socket) {
        if (socket.closed) luaL_error(L, "socket is closed");
        ++socket.activeOperations;
    }

    SocketBase::Operation::~Operation() {
        --socket.activeOperations;
        if (socket.closed && socket.activeOperations == 0) socket.releaseHandle();
    }

    int SocketBase::setTimeout(lua_State* L) {
        if (lua_isnoneornil(L, 2)) {
            timeout = -1;
            return 0;
        }

        double seconds = luaL_checknumber(L, 2);
        if (seconds < 0) luaL_argerror(L, 2, "timeout must be non-negative, nil waits forever");
        timeout = seconds;
        return 0;
    }

    // Marked closed first, so a watcher or another thread waking up meanwhile sees it.
    // Shutting the socket down wakes operations waiting on it from other threads.
    int SocketBase::close(lua_State* L) {
        if (closed) return 0;

        closed = true;
        cancelWatcher(L);

        if (activeOperations > 0) {
            shutdownSocket(handle);
        } else {
            releaseHandle();
        }

        return 0;
    }

    int SocketBase::getClosed(lua_State* L) {
        lua_pushboolean(L, closed);
        return 1;
    }

    int SocketBase::getTimeout(lua_State* L) {
        if (timeout < 0) {
            lua_pushnil(L);
        } else {
            lua_pushnumber(L, timeout);
        }
        return 1;
    }

    // TcpSocket

    TcpSocket::TcpSocket(ILuauHost* host, SocketHandle handle) : SocketBase(host, handle) {
        Net::remoteAddress(handle, remoteHost, remotePort);
        Net::localAddress(handle, localHost, localPort);
    }

    void TcpSocket::RegisterClass(lua_State* L) {
        luaL_getmetatable(L, Name);
        bool registered = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (registered) return;

        Register(L, {
            { "send",       &TcpSocket::send },
            { "receive",    &TcpSocket::receive },
            { "setTimeout", &TcpSocket::setTimeout },
            { "close",      &TcpSocket::close }
        }, {
            { "remoteAddress", &TcpSocket::getRemoteAddress, nullptr },
            { "remotePort",    &TcpSocket::getRemotePort,    nullptr },
            { "localAddress",  &TcpSocket::getLocalAddress,  nullptr },
            { "localPort",     &TcpSocket::getLocalPort,     nullptr },
            { "timeout",       &TcpSocket::getTimeout,       nullptr },
            { "closed",        &TcpSocket::getClosed,        nullptr },
            { "received",      &TcpSocket::getReceived,      nullptr },
            { "disconnected",  &TcpSocket::getDisconnected,  nullptr }
        });
    }

    // A timeout raises an error, since part of the data may have been sent
    int TcpSocket::send(lua_State* L) {
        size_t length = 0;
        const char* data = checkData(L, 2, &length);
        Operation operation(*this, L);

        std::string error;
        int result;
        {
            // The data stays referenced from this call's stack, so it's safe to read without the lock
            StateRelease release(host);
            result = sendAll(handle, data, length, timeout, error);
        }

        if (result == 0) luaL_error(L, "send timed out");
        if (result < 0) luaL_error(L, "send failed: %s", error.c_str());
        return 0;
    }

    int TcpSocket::receive(lua_State* L) {
        int count = checkCount(L, 2, 4096);
        checkNotWatched(L);
        Operation operation(*this, L);

        std::string buffer(static_cast<size_t>(count), '\0');
        int received = 0;
        int ready;
        std::string error;
        {
            StateRelease release(host);
            ready = waitForSocket(handle, false, timeout);
            if (ready > 0) {
                received = static_cast<int>(::recv(handle, buffer.data(), count, 0));
                if (received < 0) error = lastSocketError();
            } else if (ready < 0) {
                error = lastSocketError();
            }
        }

        // A socket closed from another thread ends the wait like a closed connection
        if (closed) received = 0;
        else if (!error.empty()) luaL_error(L, "receive failed: %s", error.c_str());

        if (ready == 0) {
            lua_pushnil(L);
            return 1;
        }

        lua_pushlstring(L, buffer.data(), static_cast<size_t>(received));
        return 1;
    }

    int TcpSocket::getRemoteAddress(lua_State* L) {
        lua_pushlstring(L, remoteHost.data(), remoteHost.size());
        return 1;
    }

    int TcpSocket::getRemotePort(lua_State* L) {
        lua_pushinteger(L, remotePort);
        return 1;
    }

    int TcpSocket::getLocalAddress(lua_State* L) {
        lua_pushlstring(L, localHost.data(), localHost.size());
        return 1;
    }

    int TcpSocket::getLocalPort(lua_State* L) {
        lua_pushinteger(L, localPort);
        return 1;
    }

    // TcpServer

    TcpServer::TcpServer(ILuauHost* host, SocketHandle handle) : SocketBase(host, handle) {
        Net::localAddress(handle, localHost, localPort);
    }

    void TcpServer::RegisterClass(lua_State* L) {
        luaL_getmetatable(L, Name);
        bool registered = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (registered) return;

        Register(L, {
            { "accept",     &TcpServer::accept },
            { "setTimeout", &TcpServer::setTimeout },
            { "close",      &TcpServer::close }
        }, {
            { "localAddress", &TcpServer::getLocalAddress, nullptr },
            { "localPort",    &TcpServer::getLocalPort,    nullptr },
            { "timeout",      &TcpServer::getTimeout,      nullptr },
            { "closed",       &TcpServer::getClosed,       nullptr },
            { "connected",    &TcpServer::getConnected,    nullptr }
        });
    }

    int TcpServer::accept(lua_State* L) {
        checkNotWatched(L);
        Operation operation(*this, L);

        SocketHandle client = InvalidSocket;
        int ready;
        std::string error;
        {
            StateRelease release(host);
            ready = waitForSocket(handle, false, timeout);
            if (ready > 0) {
                client = ::accept(handle, nullptr, nullptr);
                if (client == InvalidSocket) error = lastSocketError();
            } else if (ready < 0) {
                error = lastSocketError();
            }
        }

        if (closed) {
            if (client != InvalidSocket) closeSocket(client);
            luaL_error(L, "server was closed while accepting");
        }
        if (!error.empty()) luaL_error(L, "accept failed: %s", error.c_str());

        if (ready == 0) {
            lua_pushnil(L);
            return 1;
        }

        TcpSocket::Push(L, host, client);
        return 1;
    }

    int TcpServer::getLocalAddress(lua_State* L) {
        lua_pushlstring(L, localHost.data(), localHost.size());
        return 1;
    }

    int TcpServer::getLocalPort(lua_State* L) {
        lua_pushinteger(L, localPort);
        return 1;
    }

    // UdpSocket

    UdpSocket::UdpSocket(ILuauHost* host, SocketHandle handle, int family) : SocketBase(host, handle), family(family) {
        Net::localAddress(handle, localHost, localPort);
    }

    void UdpSocket::RegisterClass(lua_State* L) {
        luaL_getmetatable(L, Name);
        bool registered = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (registered) return;

        Register(L, {
            { "sendTo",      &UdpSocket::sendTo },
            { "receiveFrom", &UdpSocket::receiveFrom },
            { "setTimeout",  &UdpSocket::setTimeout },
            { "close",       &UdpSocket::close }
        }, {
            { "localAddress", &UdpSocket::getLocalAddress, nullptr },
            { "localPort",    &UdpSocket::getLocalPort,    nullptr },
            { "timeout",      &UdpSocket::getTimeout,      nullptr },
            { "closed",       &UdpSocket::getClosed,       nullptr },
            { "received",     &UdpSocket::getReceived,     nullptr }
        });
    }

    // The target is resolved for the socket's own address family
    int UdpSocket::sendTo(lua_State* L) {
        size_t length = 0;
        const char* data = checkData(L, 2, &length);
        std::string targetHost = luaL_checkstring(L, 3);
        int targetPort = luaL_checkinteger(L, 4);
        if (targetPort < 0 || targetPort > 65535) luaL_argerror(L, 4, "port must be between 0 and 65535");
        Operation operation(*this, L);

        std::string error;
        {
            StateRelease release(host);
            AddressList list;
            if (resolveAddress(targetHost, targetPort, family, SOCK_DGRAM, false, list, error)) {
                int ready = waitForSocket(handle, true, timeout);
                if (ready == 0) {
                    error = "timed out";
                } else if (ready < 0) {
                    error = lastSocketError();
                } else if (sendDatagram(handle, data, length, list.head->ai_addr, static_cast<socklen_t>(list.head->ai_addrlen)) < 0) {
                    error = lastSocketError();
                }
            }
        }

        if (!error.empty()) luaL_error(L, "could not send to %s:%d: %s", targetHost.c_str(), targetPort, error.c_str());
        return 0;
    }

    int UdpSocket::receiveFrom(lua_State* L) {
        int count = checkCount(L, 2, 65536);
        checkNotWatched(L);
        Operation operation(*this, L);

        std::string buffer(static_cast<size_t>(count), '\0');
        sockaddr_storage sender{};
        socklen_t senderLength = sizeof(sender);
        int received = 0;
        int ready;
        std::string error;
        {
            StateRelease release(host);
            ready = waitForSocket(handle, false, timeout);
            if (ready > 0) {
                received = static_cast<int>(::recvfrom(handle, buffer.data(), count, 0, reinterpret_cast<sockaddr*>(&sender), &senderLength));
                if (received < 0) error = lastSocketError();
            } else if (ready < 0) {
                error = lastSocketError();
            }
        }

        if (closed) luaL_error(L, "socket was closed while receiving");
        if (!error.empty()) luaL_error(L, "receive failed: %s", error.c_str());

        if (ready == 0) {
            lua_pushnil(L);
            return 1;
        }

        std::string senderHost;
        int senderPort = 0;
        formatAddress(reinterpret_cast<sockaddr*>(&sender), senderLength, senderHost, senderPort);

        lua_pushlstring(L, buffer.data(), static_cast<size_t>(received));
        lua_pushlstring(L, senderHost.data(), senderHost.size());
        lua_pushinteger(L, senderPort);
        return 3;
    }

    int UdpSocket::getLocalAddress(lua_State* L) {
        lua_pushlstring(L, localHost.data(), localHost.size());
        return 1;
    }

    int UdpSocket::getLocalPort(lua_State* L) {
        lua_pushinteger(L, localPort);
        return 1;
    }
} // namespace Luwow::Net
