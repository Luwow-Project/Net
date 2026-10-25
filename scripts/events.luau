local net = require("@Luwow/net")
local task = require("@Luwow/task")

local server = net.tcpListen(0, "127.0.0.1")
local port = server.localPort

server.connected:Connect(function(socket)
    print("server: connection from", socket.remoteAddress)
    socket.received:Connect(function(data)
        print("server: got", data)
        socket:send("echo:" .. data)
    end)
    socket.disconnected:Connect(function()
        print("server: client left, closed =", socket.closed)
        server:close()
    end)
end)

local client = net.tcpConnect("127.0.0.1", port)
local replies = 0
client.received:Connect(function(data)
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
udp.received:Connect(function(data, host, p)
    print("udp: got", data, "from", host)
    udp:close()
end)
local sender = net.udpBind(0, "127.0.0.1")
sender:sendTo("datagram", "127.0.0.1", udp.localPort)
sender:close()

print("main script done, events keep the program alive")
