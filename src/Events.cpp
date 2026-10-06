#include "Sockets.h"
#include "SignalCore.h"
#include "lualib.h"

#include <string>

/*
    Each socket's signals live in a registry table keyed by the socket, created on first access.
    A watcher thread then asks the scheduler to resume it whenever the socket is readable,
    fires the signals, and asks again, until the socket closes.
*/
namespace Luwow::Net {
    using Message = Luwow::Engine::Message;
    namespace Topics = Luwow::Engine::Topics;

    static constexpr const char* EventsRegistryKey = "Luwow.NetEvents";

    // Pushes the registry table mapping sockets to their events
    static void pushEventsRegistry(lua_State* L) {
        lua_getfield(L, LUA_REGISTRYINDEX, EventsRegistryKey);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, LUA_REGISTRYINDEX, EventsRegistryKey);
        }
    }

    int SocketBase::pushEvent(lua_State* L, const char* name) {
        // A closed socket never fires, its signal is only there so scripts can connect without checking
        if (closed) {
            Signals::pushSignal(L);
            return 1;
        }

        pushEventsRegistry(L);
        lua_pushvalue(L, 1);
        lua_rawget(L, -2);

        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);

            lua_newtable(L);
            for (const char* const* event = eventNames(); *event; ++event) {
                Signals::pushSignal(L);
                lua_setfield(L, -2, *event);
            }

            lua_pushvalue(L, 1);
            lua_pushvalue(L, -2);
            lua_rawset(L, -4);

            startWatcher(L, lua_gettop(L));
        }

        lua_getfield(L, -1, name);
        return 1;
    }

    void SocketBase::checkNotWatched(lua_State* L) const {
        if (watcher) luaL_error(L, "this socket is read through its event signals, use them instead");
    }

    void SocketBase::closeHandle() {
        if (closed) return;
        closed = true;
        
        if (activeOperations > 0) {
            shutdownSocket(handle);
        } else {
            releaseHandle();
        }
    }

    // The watcher's closure holds the socket (keeping it alive while watched), a pointer to it, and its events
    void SocketBase::startWatcher(lua_State* L, int eventsIndex) {
        lua_State* thread = lua_newthread(L);

        lua_pushvalue(L, 1);
        lua_pushlightuserdata(L, this);
        lua_pushvalue(L, eventsIndex);
        lua_pushcclosurek(L, watchStart, "socketWatcher", 3, watchContinue);
        lua_xmove(L, thread, 1);

        watcher = thread;
        int status = lua_resume(thread, L, 0);

        if (status != LUA_YIELD && status != LUA_OK) {
            const char* message = lua_tostring(thread, -1);
            std::string error = message ? message : "could not watch the socket";
            lua_pop(L, 1);
            luaL_error(L, "%s", error.c_str());
        }
        lua_pop(L, 1);
    }

    // Cancels a watcher waiting for the scheduler and lets it clean up. A running watcher is the one
    // that fired the handler closing the socket, it notices the socket closed on its own.
    void SocketBase::cancelWatcher(lua_State* L) {
        lua_State* thread = watcher;
        if (!thread || lua_costatus(L, thread) != LUA_COSUS) return;

        Message request;
        request.topic = Topics::SchedulerUnwatch;
        request.state = thread;
        host->request(request);

        lua_pushboolean(thread, false);
        lua_resume(thread, L, 1);
    }

    // Runs on the watcher. Disconnects the events, so nothing keeps the socket alive.
    void SocketBase::stopWatching(lua_State* L) {
        watcher = nullptr;

        for (const char* const* event = eventNames(); *event; ++event) {
            lua_getfield(L, lua_upvalueindex(3), *event);
            Signals::disconnectAll(L, -1);
            lua_pop(L, 1);
        }

        pushEventsRegistry(L);
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_pushnil(L);
        lua_rawset(L, -3);
        lua_pop(L, 1);
    }

    int SocketBase::requestWatch(lua_State* L) {
        SocketBase* socket = static_cast<SocketBase*>(lua_touserdata(L, lua_upvalueindex(2)));

        Message request;
        request.topic = Topics::SchedulerWatch;
        request.state = L;
        request.data = std::to_string(static_cast<unsigned long long>(socket->handle));

        if (!socket->host->request(request) || request.result != 1) {
            socket->stopWatching(L);
            luaL_error(L, "Socket events require a scheduler to run.");
        }
        return lua_yield(L, 0);
    }

    int SocketBase::watchStart(lua_State* L) {
        return requestWatch(L);
    }

    // The scheduler resumes the watcher with true once the socket is readable, close resumes it with false
    int SocketBase::watchContinue(lua_State* L, int status) {
        SocketBase* socket = static_cast<SocketBase*>(lua_touserdata(L, lua_upvalueindex(2)));
        bool ready = lua_toboolean(L, -1);
        lua_settop(L, 0);

        if (!ready || socket->closed || !socket->onReadable(L, lua_upvalueindex(3)) || socket->closed) {
            socket->stopWatching(L);
            return 0;
        }
        return requestWatch(L);
    }

    // TcpServer

    static const char* const TcpServerEvents[] = { "connected", nullptr };

    const char* const* TcpServer::eventNames() const {
        return TcpServerEvents;
    }

    int TcpServer::getConnected(lua_State* L) {
        return pushEvent(L, "connected");
    }

    // A failed accept, such as a connection reset before it was accepted, keeps the server watched
    bool TcpServer::onReadable(lua_State* L, int eventsIndex) {
        SocketHandle client = ::accept(handle, nullptr, nullptr);
        if (client == InvalidSocket) return true;

        lua_getfield(L, eventsIndex, "connected");
        TcpSocket::Push(L, host, client);
        Signals::fireSignal(L, -2, 1);
        lua_pop(L, 1);
        return true;
    }

    // TcpSocket

    static const char* const TcpSocketEvents[] = { "received", "disconnected", nullptr };

    const char* const* TcpSocket::eventNames() const {
        return TcpSocketEvents;
    }

    int TcpSocket::getReceived(lua_State* L) {
        return pushEvent(L, "received");
    }

    int TcpSocket::getDisconnected(lua_State* L) {
        return pushEvent(L, "disconnected");
    }

    // The socket closes before disconnected fires, so its handlers see socket.closed
    bool TcpSocket::onReadable(lua_State* L, int eventsIndex) {
        std::string buffer(65536, '\0');
        int received = static_cast<int>(::recv(handle, buffer.data(), static_cast<int>(buffer.size()), 0));

        if (received > 0) {
            lua_getfield(L, eventsIndex, "received");
            lua_pushlstring(L, buffer.data(), static_cast<size_t>(received));
            Signals::fireSignal(L, -2, 1);
            lua_pop(L, 1);
            return true;
        }

        closeHandle();
        lua_getfield(L, eventsIndex, "disconnected");
        Signals::fireSignal(L, -1, 0);
        lua_pop(L, 1);
        return false;
    }

    // UdpSocket

    static const char* const UdpSocketEvents[] = { "received", nullptr };

    const char* const* UdpSocket::eventNames() const {
        return UdpSocketEvents;
    }

    int UdpSocket::getReceived(lua_State* L) {
        return pushEvent(L, "received");
    }

    // Receive errors are skipped, on Windows an unreachable port from an earlier send shows up as one
    bool UdpSocket::onReadable(lua_State* L, int eventsIndex) {
        std::string buffer(65536, '\0');
        sockaddr_storage sender{};
        socklen_t senderLength = sizeof(sender);
        int received = static_cast<int>(::recvfrom(handle, buffer.data(), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&sender), &senderLength));
        if (received < 0) return true;

        std::string senderHost;
        int senderPort = 0;
        formatAddress(reinterpret_cast<sockaddr*>(&sender), senderLength, senderHost, senderPort);

        lua_getfield(L, eventsIndex, "received");
        lua_pushlstring(L, buffer.data(), static_cast<size_t>(received));
        lua_pushlstring(L, senderHost.data(), senderHost.size());
        lua_pushinteger(L, senderPort);
        Signals::fireSignal(L, -4, 3);
        lua_pop(L, 1);
        return true;
    }
} // namespace Luwow::Net
