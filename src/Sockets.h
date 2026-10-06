#pragma once

#include "Platform.h"
#include "ILuauHost.h"
#include "Userdata.h"

#include <string>

namespace Luwow::Net {
    using ILuauHost = Luwow::Engine::ILuauHost;

    // What every socket shares: the handle, its timeout, and closing safely while another thread waits on it.
    // Methods run under the state lock and only release it to wait, so the bookkeeping below needs no extra lock.
    class SocketBase {
    public:
        SocketBase(ILuauHost* host, SocketHandle handle);
        virtual ~SocketBase();

        // socket:setTimeout(seconds: number?), nil waits forever
        int setTimeout(lua_State* L);
        // socket:close(), closing twice is a no-op
        int close(lua_State* L);
        int getClosed(lua_State* L);
        int getTimeout(lua_State* L);

    protected:
        // Pushes the socket's event signal `name`, for a getter with the socket at index 1.
        // The first access creates the socket's signals and starts its watcher.
        int pushEvent(lua_State* L, const char* name);

        // Raises when the watcher reads the socket, for operations that would compete with it
        void checkNotWatched(lua_State* L) const;

        // The socket type's event names, ending with nullptr
        virtual const char* const* eventNames() const = 0;

        // Runs on the watcher when the socket is readable, with the socket's events table at `eventsIndex`.
        // Fires the matching signals, returns false to stop watching.
        virtual bool onReadable(lua_State* L, int eventsIndex) = 0;

        // Closes the socket, or only shuts it down while an operation still waits on it
        void closeHandle();

        // Marks an operation in progress for its scope, raising if the socket is closed.
        // A close during the operation only shuts the socket down, the handle is freed when the operation ends.
        class Operation {
        public:
            Operation(SocketBase& socket, lua_State* L);
            ~Operation();

            Operation(const Operation&) = delete;
            Operation& operator=(const Operation&) = delete;

        private:
            SocketBase& socket;
        };

        ILuauHost* host;
        SocketHandle handle;
        double timeout = -1;
        bool closed = false;
        int activeOperations = 0;

        // The coroutine watching the socket for its events, while it runs
        lua_State* watcher = nullptr;

    private:
        void releaseHandle();

        // The watcher coroutine: its body requests a watch from the scheduler and yields, and its
        // continuation runs each time the scheduler resumes it, then watches again
        void startWatcher(lua_State* L, int eventsIndex);
        void cancelWatcher(lua_State* L);
        void stopWatching(lua_State* L);
        static int requestWatch(lua_State* L);
        static int watchStart(lua_State* L);
        static int watchContinue(lua_State* L, int status);
    };

    class TcpSocket : public Luwow::Engine::Userdata<TcpSocket>, public SocketBase {
    public:
        static constexpr const char* Name = "TcpSocket";

        TcpSocket(ILuauHost* host, SocketHandle handle);

        // Registers the TcpSocket metatable once per Luau state
        static void RegisterClass(lua_State* L);

        // socket:send(data: string | buffer), sends all of it
        int send(lua_State* L);
        // socket:receive(count: number?) -> string?, up to count bytes, "" once the peer closed, nil on timeout
        int receive(lua_State* L);

        int getRemoteAddress(lua_State* L);
        int getRemotePort(lua_State* L);
        int getLocalAddress(lua_State* L);
        int getLocalPort(lua_State* L);

        // socket.received: Signal<(data: string)>, socket.disconnected: Signal<()>
        int getReceived(lua_State* L);
        int getDisconnected(lua_State* L);

    protected:
        const char* const* eventNames() const override;
        bool onReadable(lua_State* L, int eventsIndex) override;

    private:
        std::string remoteHost;
        int remotePort = 0;
        std::string localHost;
        int localPort = 0;
    };

    class TcpServer : public Luwow::Engine::Userdata<TcpServer>, public SocketBase {
    public:
        static constexpr const char* Name = "TcpServer";

        TcpServer(ILuauHost* host, SocketHandle handle);

        static void RegisterClass(lua_State* L);

        // server:accept() -> TcpSocket?, nil on timeout
        int accept(lua_State* L);

        int getLocalAddress(lua_State* L);
        int getLocalPort(lua_State* L);

        // server.connected: Signal<(socket: TcpSocket)>
        int getConnected(lua_State* L);

    protected:
        const char* const* eventNames() const override;
        bool onReadable(lua_State* L, int eventsIndex) override;

    private:
        std::string localHost;
        int localPort = 0;
    };

    class UdpSocket : public Luwow::Engine::Userdata<UdpSocket>, public SocketBase {
    public:
        static constexpr const char* Name = "UdpSocket";

        UdpSocket(ILuauHost* host, SocketHandle handle, int family);

        static void RegisterClass(lua_State* L);

        // socket:sendTo(data: string | buffer, host: string, port: number)
        int sendTo(lua_State* L);
        // socket:receiveFrom(count: number?) -> (string?, string?, number?), data and sender, nil on timeout
        int receiveFrom(lua_State* L);

        int getLocalAddress(lua_State* L);
        int getLocalPort(lua_State* L);

        // socket.received: Signal<(data: string, host: string, port: number)>
        int getReceived(lua_State* L);

    protected:
        const char* const* eventNames() const override;
        bool onReadable(lua_State* L, int eventsIndex) override;

    private:
        int family;
        std::string localHost;
        int localPort = 0;
    };
} // namespace Luwow::Net
