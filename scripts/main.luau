local net = require("@Luwow/net")

-- A TCP echo server on a free port, and a client talking to it
local server = net.tcpListen(0, "127.0.0.1")
print("Listening on port", server.localPort)

local client = net.tcpConnect("127.0.0.1", server.localPort, 5)
local connection = server:accept()

client:send("Hello over TCP!")
local message = connection:receive()
connection:send(message)
print("Echoed:", client:receive())

client:close()
connection:close()
server:close()

-- UDP between two sockets
local receiver = net.udpBind(0, "127.0.0.1")
local sender = net.udpBind(0, "127.0.0.1")

sender:sendTo("Hello over UDP!", "127.0.0.1", receiver.localPort)
local data, host, port = receiver:receiveFrom()
print("Received", data, "from", host, port)

receiver:close()
sender:close()

print("localhost is", table.concat(net.resolve("localhost"), ", "))
