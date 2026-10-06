#include "Library.h"
#include "Platform.h"
#include "Sockets.h"
#include "SignalCore.h"
#include "lualib.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace Luwow::Net {
    using StateRelease = Luwow::Engine::StateRelease;

    static Library* getModuleInstance(lua_State* L) {
        Library* net = static_cast<Library*>(lua_touserdata(L, lua_upvalueindex(1)));
        if (!net) {
            throw std::runtime_error("Net module not found!");
        }
        return net;
    }

    static int checkPort(lua_State* L, int index, int fallback) {
        int port = lua_isnoneornil(L, index) ? fallback : luaL_checkinteger(L, index);
        if (port < 0 || port > 65535) luaL_argerror(L, index, "port must be between 0 and 65535");
        return port;
    }

    ILuauModule* Library::initialize(ILuauHost* host) {
        initializeSockets();

        lua_State* L = host->getMainState();
        Signals::registerSignals(L);
        TcpSocket::RegisterClass(L);
        TcpServer::RegisterClass(L);
        UdpSocket::RegisterClass(L);

        Library* net = new Library();
        net->host = host;
        return net;
    }

    Luwow::Engine::RunMode Library::getRunMode() const {
        return LUWOW_MODULE_RUN_MODE;
    }

    // net.tcpConnect(host: string, port: number, timeout: number?) -> TcpSocket
    static int net_tcpConnect(lua_State* L) {
        Library* net = getModuleInstance(L);
        std::string host = luaL_checkstring(L, 1);
        int port = checkPort(L, 2, -1);
        double timeout = luaL_optnumber(L, 3, -1);

        SocketHandle handle;
        std::string error;
        {
            // Resolving and connecting can take a while, other threads may run Luau meanwhile
            StateRelease release(net->getHost());
            handle = connectTcp(host, port, timeout, error);
        }

        if (handle == InvalidSocket) luaL_error(L, "could not connect to %s:%d: %s", host.c_str(), port, error.c_str());
        TcpSocket::Push(L, net->getHost(), handle);
        return 1;
    }

    // net.tcpListen(port: number, host: string?, backlog: number?) -> TcpServer
    // Port 0 picks a free port, read it back from server.localPort. host defaults to every IPv4 interface.
    static int net_tcpListen(lua_State* L) {
        Library* net = getModuleInstance(L);
        int port = checkPort(L, 1, -1);
        std::string host = luaL_optstring(L, 2, "0.0.0.0");
        int backlog = luaL_optinteger(L, 3, SOMAXCONN);

        SocketHandle handle;
        std::string error;
        {
            StateRelease release(net->getHost());
            handle = listenTcp(host, port, backlog, error);
        }

        if (handle == InvalidSocket) luaL_error(L, "could not listen on %s:%d: %s", host.c_str(), port, error.c_str());
        TcpServer::Push(L, net->getHost(), handle);
        return 1;
    }

    // net.udpBind(port: number?, host: string?) -> UdpSocket
    // Port 0 (the default) picks a free port. host defaults to every IPv4 interface.
    static int net_udpBind(lua_State* L) {
        Library* net = getModuleInstance(L);
        int port = checkPort(L, 1, 0);
        std::string host = luaL_optstring(L, 2, "0.0.0.0");

        SocketHandle handle;
        int family = AF_UNSPEC;
        std::string error;
        {
            StateRelease release(net->getHost());
            handle = bindUdp(host, port, family, error);
        }

        if (handle == InvalidSocket) luaL_error(L, "could not bind to %s:%d: %s", host.c_str(), port, error.c_str());
        UdpSocket::Push(L, net->getHost(), handle, family);
        return 1;
    }

    // net.resolve(host: string) -> { string }
    static int net_resolve(lua_State* L) {
        Library* net = getModuleInstance(L);
        std::string host = luaL_checkstring(L, 1);

        std::vector<std::string> addresses;
        std::string error;
        bool resolved;
        {
            StateRelease release(net->getHost());
            resolved = resolveHost(host, addresses, error);
        }

        if (!resolved) luaL_error(L, "could not resolve %s: %s", host.c_str(), error.c_str());

        lua_createtable(L, static_cast<int>(addresses.size()), 0);
        for (size_t i = 0; i < addresses.size(); ++i) {
            lua_pushlstring(L, addresses[i].data(), addresses[i].size());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
        return 1;
    }

    const char* Library::getModuleName() const {
        return "net";
    }

    const char* Library::getModuleAlias() const {
        return "Luwow";
    }

    static LuauExport exports[] = {
        { "tcpConnect", net_tcpConnect },
        { "tcpListen", net_tcpListen },
        { "udpBind", net_udpBind },
        { "resolve", net_resolve },
        { nullptr, nullptr }
    };

    const LuauExport* Library::getExports() const {
        return exports;
    }
} // namespace Luwow::Net

LUWOW_REGISTER_MODULE(Luwow::Net::Library)
