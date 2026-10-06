# Net
A TCP and UDP networking library for Luwow-Project.

This repository is not stand-alone. You must build it externally from within another
repository like https://github.com/Luwow-Project/Release

## Project Overview

This project contains the sources for Luwow Net library.

It uses the platform's sockets directly (Winsock on Windows, BSD sockets on macOS and Linux) and directly depends on the [Signal](https://github.com/Luwow-Project/Signal) library. Every operation is synchronous with an optional timeout, and operations that wait (resolving, connecting, accepting, receiving, sending) release the Luau state lock, so parallel libraries keep running meanwhile. The library only supports the `SERIAL` run mode.

Errors raise Luau errors. Timeouts return nil, except for `send`, which raises since part of the data may already be sent.

## Library Documentation

### `net.tcpConnect(host: string, port: number, timeout: number?) -> TcpSocket`

Connects to a TCP server, trying each address of `host` until one accepts. `timeout` limits each attempt, in seconds.

### `net.tcpListen(port: number, host: string?, backlog: number?) -> TcpServer`

Starts a TCP server. Port 0 picks a free port, read it from `server.localPort`. `host` defaults to `"0.0.0.0"` (every IPv4 interface), use `"::"` for IPv6.

### `net.udpBind(port: number?, host: string?) -> UdpSocket`

Opens a UDP socket. Port 0, the default, picks a free port. `host` defaults to `"0.0.0.0"`.

### `net.resolve(host: string) -> { string }`

The numeric IPv4 and IPv6 addresses of a host name.

## Sockets

Every socket has:

- `socket.localAddress: string`, `socket.localPort: number`
- `socket.timeout: number?`, nil when operations wait forever (the default)
- `socket.closed: boolean`
- `socket:setTimeout(seconds: number?)`, nil waits forever
- `socket:close()`, closing twice does nothing. An operation waiting on the socket from another thread is woken up.

Sockets are also closed when garbage collected.

### TcpSocket

- `socket.remoteAddress: string`, `socket.remotePort: number`
- `socket:send(data: string | buffer)` sends all of the data.
- `socket:receive(count: number?) -> string?` receives up to `count` bytes (default 4096), waiting for at least one. Returns `""` once the other end closed the connection, nil on timeout.

### TcpServer

- `server:accept() -> TcpSocket?` waits for the next connection, nil on timeout.

### UdpSocket

- `socket:sendTo(data: string | buffer, host: string, port: number)` sends one datagram.
- `socket:receiveFrom(count: number?) -> (string?, string?, number?)` waits for the next datagram (up to `count` bytes, default 65536), returning its data and sender, or nil on timeout.

## Events

Instead of calling `accept`, `receive` or `receiveFrom`, a script can connect to a socket's signals (see the [Signal](https://github.com/Luwow-Project/Signal) library):

| Socket | Signal | Fired with |
|---|---|---|
| `TcpServer` | `server.connected` | `(socket: TcpSocket)` for each new connection |
| `TcpSocket` | `socket.received` | `(data: string)` each time data arrives |
| `TcpSocket` | `socket.disconnected` | `()` once, when the other end closes the connection or it fails. The socket is closed by then. |
| `UdpSocket` | `socket.received` | `(data: string, host: string, port: number)` for each datagram |

```luau
local net = require("@Luwow/net")
local task = require("@Luwow/task")

local server = net.tcpListen(8080)
server.connected:connect(function(socket)
    socket.received:connect(function(data)
        socket:send(data) -- echo
    end)
end)
```

- **Events need a task scheduler.** Reading a socket's signal starts a watcher, thus a scheduler must watch the socket in its event loop and resume the watcher when it's readable, which then reads and fires the signal. Without a scheduler, reading a signal raises an error.
- **The program stays alive** while any socket is watched, after the main script ends. Closing the socket stops its watcher, disconnects its handlers, and lets the program exit.
- **A watched socket is read through its signals only.** Its `receive` / `accept` / `receiveFrom` raise an error, since they'd compete with the watcher for the same data. `send` and `sendTo` still work.
- Handlers run in their own luau threads, so they may yield.
