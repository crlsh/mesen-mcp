local socket = require("socket.core")

local server = nil
local client = nil
local port = 12345

function init_server()
	server = socket.tcp()
	server:setoption("reuseaddr", true)
	local res, err = server:bind("127.0.0.1", port)
	if not res then
		emu.log("Failed to bind: " .. tostring(err))
		return
	end
	server:listen(1)
	server:settimeout(0)
	emu.log("MCP Bridge listening on port " .. port)
end

function serialize(val)
	if type(val) == "table" then
		local res = "{"
		for k, v in pairs(val) do
			res = res .. "[" .. serialize(k) .. "]=" .. serialize(v) .. ","
		end
		return res .. "}"
	elseif type(val) == "string" then
		return string.format("%q", val)
	else
		return tostring(val)
	end
end

function handle_command(line)
	-- The line is expected to be a Lua expression that returns a value
	-- e.g. "emu.read(0x8000, emu.memType.nesMemory, false)"
	local func, err = load("return " .. line)
	if not func then
		return "Error parsing command: " .. tostring(err)
	end
	
	local success, result = pcall(func)
	if success then
		return serialize(result)
	else
		return "Error executing command: " .. tostring(result)
	end
end

function process_commands()
	if not server then return end
	
	if not client then
		local new_client, err = server:accept()
		if new_client then
			client = new_client
			client:settimeout(0)
			emu.log("MCP Client connected")
		end
	end
	
	if client then
		local line, err = client:receive()
		if line then
			local result = handle_command(line)
			client:send(result .. "\n")
		elseif err == "closed" then
			client = nil
			emu.log("MCP Client disconnected")
		elseif err ~= "timeout" then
			emu.log("Socket error: " .. tostring(err))
			client = nil
		end
	end
end

init_server()
emu.addEventCallback(process_commands, emu.eventType.endFrame)
