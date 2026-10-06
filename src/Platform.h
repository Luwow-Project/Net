#pragma once

#include <string>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
#endif

namespace Luwow::Net {
#if defined(_WIN32)
    using SocketHandle = SOCKET;
    inline constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
    using SocketHandle = int;
    inline constexpr SocketHandle InvalidSocket = -1;
#endif

    // getaddrinfo results, freed when the list goes out of scope
    struct AddressList {
        addrinfo* head = nullptr;

        AddressList() = default;
        ~AddressList() { if (head) freeaddrinfo(head); }
        AddressList(const AddressList&) = delete;
        AddressList& operator=(const AddressList&) = delete;
    };

    // Starts the platform's socket library once per process (Winsock on Windows)
    void initializeSockets();

    SocketHandle createSocket(int family, int type, int protocol);
    void closeSocket(SocketHandle socket);

    // Stops reads and writes, which also wakes another thread waiting on the socket
    void shutdownSocket(SocketHandle socket);

    // Message of the last socket error on this thread
    std::string lastSocketError();

    // Waits until the socket can be read or written. Returns 1 when ready, 0 on timeout, -1 on error.
    // A negative timeout waits forever.
    int waitForSocket(SocketHandle socket, bool forWrite, double timeoutSeconds);

    // Sends everything, waiting for the socket between parts. Returns 1 when done, 0 on timeout, -1 on error.
    int sendAll(SocketHandle socket, const char* data, size_t length, double timeoutSeconds, std::string& error);

    // Sends one datagram, returns the bytes sent or -1 on error
    int sendDatagram(SocketHandle socket, const char* data, size_t length, const sockaddr* address, socklen_t addressLength);

    // Formats a socket address as a numeric host and port
    bool formatAddress(const sockaddr* address, socklen_t length, std::string& host, int& port);

    // The local or remote address of a socket
    bool localAddress(SocketHandle socket, std::string& host, int& port);
    bool remoteAddress(SocketHandle socket, std::string& host, int& port);

    // Resolves a host and port. family may be AF_UNSPEC, passive is for addresses to bind to.
    bool resolveAddress(const std::string& host, int port, int family, int socketType, bool passive, AddressList& list, std::string& error);

    // The numeric addresses of a host name, without duplicates
    bool resolveHost(const std::string& host, std::vector<std::string>& addresses, std::string& error);

    // Connects to the first address of host that accepts, InvalidSocket with an error otherwise
    SocketHandle connectTcp(const std::string& host, int port, double timeoutSeconds, std::string& error);

    // A listening TCP socket bound to host and port
    SocketHandle listenTcp(const std::string& host, int port, int backlog, std::string& error);

    // A UDP socket bound to host and port, family receives its address family
    SocketHandle bindUdp(const std::string& host, int port, int& family, std::string& error);
} // namespace Luwow::Net
