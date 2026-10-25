#include "Library.h"
#include "Http.h"
#include "Platform.h"
#include "Sockets.h"
#include "SignalCore.h"
#include "OffThread.h"
#include "lualib.h"

#include <cctype>
#include <memory>
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
        Signals::registerSignals(L, host);
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

    // The string field of the request table at index 1, raising when it's there but isn't a string
    static bool optionalString(lua_State* L, const char* name, std::string& out) {
        lua_getfield(L, 1, name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        size_t length = 0;
        const char* value = lua_isstring(L, -1) ? lua_tolstring(L, -1, &length) : nullptr;
        if (!value) luaL_error(L, "the request's %s must be a string", name);
        out.assign(value, length);
        lua_pop(L, 1);
        return true;
    }

    static void pushResponse(lua_State* L, const HttpResponse& response);

    // net.request(options: {url, method?, headers?, body?, timeout?, saveTo?}) -> {ok, status, statusMessage, headers, body}
    // Any status the server answers with is a response. Failing to get one raises.
    static int net_request(lua_State* L) {
        Library* net = getModuleInstance(L);
        luaL_checktype(L, 1, LUA_TTABLE);

        HttpRequest request;
        if (!optionalString(L, "url", request.url)) luaL_error(L, "the request needs a url");
        if (optionalString(L, "method", request.method)) {
            for (char& c : request.method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        lua_getfield(L, 1, "headers");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) luaL_error(L, "the request's headers must be a table");
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                if (lua_type(L, -2) != LUA_TSTRING || !lua_isstring(L, -1)) luaL_error(L, "the request's headers must map names to strings");
                request.headers.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "body");
        if (lua_isbuffer(L, -1)) {
            size_t length = 0;
            const char* data = static_cast<const char*>(lua_tobuffer(L, -1, &length));
            request.body.assign(data, length);
        } else if (!lua_isnil(L, -1)) {
            size_t length = 0;
            const char* data = lua_isstring(L, -1) ? lua_tolstring(L, -1, &length) : nullptr;
            if (!data) luaL_error(L, "the request's body must be a string or a buffer");
            request.body.assign(data, length);
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "timeout");
        if (!lua_isnil(L, -1)) {
            if (!lua_isnumber(L, -1)) luaL_error(L, "the request's timeout must be a number");
            request.timeout = lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
        optionalString(L, "saveTo", request.saveTo);

        // Run the network request off the thread, since they can take a while or fail.
        return Luwow::Engine::runOffThread(L, net->getHost(), [request]() {
            Luwow::Engine::OffThreadResult result;
            auto response = std::make_shared<HttpResponse>();
            std::string error;
            if (!sendHttpRequest(request, *response, error)) {
                result.error = "could not request " + request.url + ": " + error;
                return result;
            }
            result.push = [response](lua_State* L) {
                pushResponse(L, *response);
                return 1;
            };
            return result;
        });
    }

    // The response table net.request returns
    static void pushResponse(lua_State* L, const HttpResponse& response) {
        lua_createtable(L, 0, 6);
        lua_pushboolean(L, response.status >= 200 && response.status < 300);
        lua_setfield(L, -2, "ok");
        lua_pushinteger(L, response.status);
        lua_setfield(L, -2, "status");
        lua_pushlstring(L, response.statusMessage.data(), response.statusMessage.size());
        lua_setfield(L, -2, "statusMessage");

        // Names are lowercase, since HTTP's are case insensitive. A repeated header's values are joined.
        lua_createtable(L, 0, static_cast<int>(response.headers.size()));
        for (const auto& [name, value] : response.headers) {
            std::string key = name;
            for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            lua_getfield(L, -1, key.c_str());
            std::string joined = lua_isstring(L, -1) ? std::string(lua_tostring(L, -1)) + ", " + value : value;
            lua_pop(L, 1);
            lua_pushlstring(L, joined.data(), joined.size());
            lua_setfield(L, -2, key.c_str());
        }
        lua_setfield(L, -2, "headers");

        lua_pushlstring(L, response.body.data(), response.body.size());
        lua_setfield(L, -2, "body");
        lua_pushnumber(L, static_cast<double>(response.bodyLength));
        lua_setfield(L, -2, "bodyLength");
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
        { "request", net_request, Luwow::Engine::finishOffThread },
        { nullptr, nullptr }
    };

    const LuauExport* Library::getExports() const {
        return exports;
    }
} // namespace Luwow::Net

LUWOW_REGISTER_MODULE(Luwow::Net::Library)
