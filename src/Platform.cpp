#include "Platform.h"

#include <climits>
#include <cstdlib>
#include <mutex>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <cerrno>
    #include <cstring>
    #include <fcntl.h>
    #include <poll.h>
    #include <unistd.h>
#endif

namespace Luwow::Net {
    // Sending to a closed connection raises SIGPIPE on Linux and macOS, which would end the process
#if defined(MSG_NOSIGNAL)
    static constexpr int SendFlags = MSG_NOSIGNAL;
#else
    static constexpr int SendFlags = 0;
#endif

    static int lastSocketErrorCode() {
#if defined(_WIN32)
        return WSAGetLastError();
#else
        return errno;
#endif
    }

    static std::string socketErrorMessage(int code) {
#if defined(_WIN32)
        char* buffer = nullptr;
        DWORD length = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<char*>(&buffer), 0, nullptr
        );
        std::string message = length ? std::string(buffer, length) : "socket error " + std::to_string(code);
        LocalFree(buffer);

        while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) message.pop_back();
        return message;
#else
        return std::strerror(code);
#endif
    }

    // A non-blocking connect that hasn't finished yet
    static bool isConnectInProgress(int code) {
#if defined(_WIN32)
        return code == WSAEWOULDBLOCK;
#else
        return code == EINPROGRESS;
#endif
    }

    static bool setBlocking(SocketHandle socket, bool blocking) {
#if defined(_WIN32)
        u_long nonBlocking = blocking ? 0 : 1;
        return ioctlsocket(socket, FIONBIO, &nonBlocking) == 0;
#else
        int flags = fcntl(socket, F_GETFL, 0);
        if (flags < 0) return false;
        flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
        return fcntl(socket, F_SETFL, flags) == 0;
#endif
    }

    void initializeSockets() {
#if defined(_WIN32)
        static std::once_flag once;
        std::call_once(once, [] {
            WSADATA data;
            WSAStartup(MAKEWORD(2, 2), &data);
        });
#endif
    }

    SocketHandle createSocket(int family, int type, int protocol) {
        SocketHandle socket = ::socket(family, type, protocol);
#if defined(SO_NOSIGPIPE)
        // macOS has no MSG_NOSIGNAL, SIGPIPE is turned off per socket instead
        if (socket != InvalidSocket) {
            int on = 1;
            setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
        }
#endif
        return socket;
    }

    void closeSocket(SocketHandle socket) {
#if defined(_WIN32)
        closesocket(socket);
#else
        close(socket);
#endif
    }

    void shutdownSocket(SocketHandle socket) {
#if defined(_WIN32)
        shutdown(socket, SD_BOTH);
#else
        shutdown(socket, SHUT_RDWR);
#endif
    }

    std::string lastSocketError() {
        return socketErrorMessage(lastSocketErrorCode());
    }

    int waitForSocket(SocketHandle socket, bool forWrite, double timeoutSeconds) {
        pollfd target{};
        target.fd = socket;
        target.events = forWrite ? POLLOUT : POLLIN;
        int timeoutMs = timeoutSeconds < 0 ? -1 : static_cast<int>(timeoutSeconds * 1000.0);

#if defined(_WIN32)
        int result = WSAPoll(&target, 1, timeoutMs);
#else
        int result;
        do {
            result = poll(&target, 1, timeoutMs);
        } while (result < 0 && errno == EINTR);
#endif

        if (result < 0) return -1;
        return result > 0 ? 1 : 0;
    }

    int sendAll(SocketHandle socket, const char* data, size_t length, double timeoutSeconds, std::string& error) {
        size_t sent = 0;
        while (sent < length) {
            int ready = waitForSocket(socket, true, timeoutSeconds);
            if (ready <= 0) {
                if (ready < 0) error = lastSocketError();
                return ready;
            }

            size_t remaining = length - sent;
            int chunk = remaining > static_cast<size_t>(INT_MAX) ? INT_MAX : static_cast<int>(remaining);
            int result = static_cast<int>(::send(socket, data + sent, chunk, SendFlags));
            if (result < 0) {
                error = lastSocketError();
                return -1;
            }
            sent += static_cast<size_t>(result);
        }
        return 1;
    }

    int sendDatagram(SocketHandle socket, const char* data, size_t length, const sockaddr* address, socklen_t addressLength) {
        return static_cast<int>(::sendto(socket, data, static_cast<int>(length), SendFlags, address, addressLength));
    }

    bool formatAddress(const sockaddr* address, socklen_t length, std::string& host, int& port) {
        char hostBuffer[NI_MAXHOST];
        char portBuffer[NI_MAXSERV];
        if (getnameinfo(address, length, hostBuffer, sizeof(hostBuffer), portBuffer, sizeof(portBuffer), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
            return false;
        }
        host = hostBuffer;
        port = std::atoi(portBuffer);
        return true;
    }

    bool localAddress(SocketHandle socket, std::string& host, int& port) {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) return false;
        return formatAddress(reinterpret_cast<sockaddr*>(&address), length, host, port);
    }

    bool remoteAddress(SocketHandle socket, std::string& host, int& port) {
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        if (getpeername(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) return false;
        return formatAddress(reinterpret_cast<sockaddr*>(&address), length, host, port);
    }

    bool resolveAddress(const std::string& host, int port, int family, int socketType, bool passive, AddressList& list, std::string& error) {
        addrinfo hints{};
        hints.ai_family = family;
        hints.ai_socktype = socketType;
        if (passive) hints.ai_flags = AI_PASSIVE;

        std::string service = std::to_string(port);
        int result = getaddrinfo(host.c_str(), service.c_str(), &hints, &list.head);
        if (result != 0) {
#if defined(_WIN32)
            error = gai_strerrorA(result);
#else
            error = gai_strerror(result);
#endif
            while (!error.empty() && (error.back() == ' ' || error.back() == '.')) error.pop_back();
            return false;
        }
        return true;
    }

    bool resolveHost(const std::string& host, std::vector<std::string>& addresses, std::string& error) {
        AddressList list;
        if (!resolveAddress(host, 0, AF_UNSPEC, SOCK_STREAM, false, list, error)) return false;

        for (addrinfo* entry = list.head; entry; entry = entry->ai_next) {
            std::string address;
            int port = 0;
            if (!formatAddress(entry->ai_addr, static_cast<socklen_t>(entry->ai_addrlen), address, port)) continue;

            bool known = false;
            for (const std::string& existing : addresses) known = known || existing == address;
            if (!known) addresses.push_back(address);
        }
        return true;
    }

    // Waits for a non-blocking connect to finish. Returns 1 when done (check SO_ERROR), 0 on timeout, -1 on error.
    // WSAPoll doesn't report a failed connect on Windows, select does through its exception set.
    static int waitForConnect(SocketHandle socket, double timeoutSeconds) {
#if defined(_WIN32)
        fd_set writable;
        fd_set failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(socket, &writable);
        FD_SET(socket, &failed);

        timeval timeout{};
        timeout.tv_sec = static_cast<long>(timeoutSeconds);
        timeout.tv_usec = static_cast<long>((timeoutSeconds - timeout.tv_sec) * 1000000.0);

        int result = select(0, nullptr, &writable, &failed, &timeout);
        if (result < 0) return -1;
        return result > 0 ? 1 : 0;
#else
        return waitForSocket(socket, true, timeoutSeconds);
#endif
    }

    // Connects without blocking, so the attempt can give up after timeoutSeconds
    static bool connectWithTimeout(SocketHandle socket, const sockaddr* address, socklen_t length, double timeoutSeconds, std::string& error) {
        if (timeoutSeconds < 0) {
            if (::connect(socket, address, length) == 0) return true;
            error = lastSocketError();
            return false;
        }

        if (!setBlocking(socket, false)) {
            error = lastSocketError();
            return false;
        }

        if (::connect(socket, address, length) != 0) {
            int code = lastSocketErrorCode();
            if (!isConnectInProgress(code)) {
                error = socketErrorMessage(code);
                return false;
            }

            int ready = waitForConnect(socket, timeoutSeconds);
            if (ready <= 0) {
                error = ready == 0 ? "timed out" : lastSocketError();
                return false;
            }

            int socketError = 0;
            socklen_t errorLength = sizeof(socketError);
            getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &errorLength);
            if (socketError != 0) {
                error = socketErrorMessage(socketError);
                return false;
            }
        }

        setBlocking(socket, true);
        return true;
    }

    SocketHandle connectTcp(const std::string& host, int port, double timeoutSeconds, std::string& error) {
        AddressList list;
        if (!resolveAddress(host, port, AF_UNSPEC, SOCK_STREAM, false, list, error)) return InvalidSocket;

        for (addrinfo* entry = list.head; entry; entry = entry->ai_next) {
            SocketHandle socket = createSocket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (socket == InvalidSocket) {
                error = lastSocketError();
                continue;
            }

            if (connectWithTimeout(socket, entry->ai_addr, static_cast<socklen_t>(entry->ai_addrlen), timeoutSeconds, error)) {
                return socket;
            }
            closeSocket(socket);
        }
        return InvalidSocket;
    }

    SocketHandle listenTcp(const std::string& host, int port, int backlog, std::string& error) {
        AddressList list;
        if (!resolveAddress(host, port, AF_UNSPEC, SOCK_STREAM, true, list, error)) return InvalidSocket;

        for (addrinfo* entry = list.head; entry; entry = entry->ai_next) {
            SocketHandle socket = createSocket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (socket == InvalidSocket) {
                error = lastSocketError();
                continue;
            }

#if !defined(_WIN32)
            // Lets a restarted server bind its port right away. On Windows this option allows stealing a
            // port that's in use, so it's left off.
            int on = 1;
            setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif

            if (bind(socket, entry->ai_addr, static_cast<socklen_t>(entry->ai_addrlen)) == 0 && listen(socket, backlog) == 0) {
                return socket;
            }
            error = lastSocketError();
            closeSocket(socket);
        }
        return InvalidSocket;
    }

    SocketHandle bindUdp(const std::string& host, int port, int& family, std::string& error) {
        AddressList list;
        if (!resolveAddress(host, port, AF_UNSPEC, SOCK_DGRAM, true, list, error)) return InvalidSocket;

        for (addrinfo* entry = list.head; entry; entry = entry->ai_next) {
            SocketHandle socket = createSocket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (socket == InvalidSocket) {
                error = lastSocketError();
                continue;
            }

            if (bind(socket, entry->ai_addr, static_cast<socklen_t>(entry->ai_addrlen)) == 0) {
                family = entry->ai_family;
                return socket;
            }
            error = lastSocketError();
            closeSocket(socket);
        }
        return InvalidSocket;
    }
} // namespace Luwow::Net
