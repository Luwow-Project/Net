local net = require("@Luwow/net")
local task = require("@Luwow/task")

local server = net.tcpListen(0, "127.0.0.1")
local port = server.localPort

server.connected:connect(function(socket)
    print("server: connection from", socket.remoteAddress)
    socket.received:connect(function(data)
        print("server: got", data)
        socket:send("echo:" .. data)
    end)
    socket.disconnected:connect(function()
        print("server: client left, closed =", socket.closed)
        server:close()
    end)
end)

local client = net.tcpConnect("127.0.0.1", port)
local replies = 0
client.received:connect(function(data)
    print("client: got", data)
    replies += 1
    if replies == 2 then
        client:close()
    end
end)

client:send("one")
task.wait(0.1)
client:send("two")

-- UDP events
local udp = net.udpBind(0, "127.0.0.1")
udp.received:connect(function(data, host, p)
    print("udp: got", data, "from", host)
    udp:close()
end)
local sender = net.udpBind(0, "127.0.0.1")
sender:sendTo("datagram", "127.0.0.1", udp.localPort)
sender:close()

print("main script done, events keep the program alive")
