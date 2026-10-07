-- wpexec audio-output.lua '{"output":"<auto|hdmi|spdif|analog|usb|bt|list>"}'
-- makes the first PipeWire output of that kind the default, switching the
-- profile of an ALSA card when none is there yet; WirePlumber keeps both.
-- auto removes the chosen default and puts each ALSA card back on its best
-- available profile, so that WirePlumber picks the output
local args = ...
if args and args.parse then
	args = args:parse()
end
local wanted = args and args.output or "list"

local devices = ObjectManager { Interest { type = "device", Constraint { "media.class", "=", "Audio/Device" } } }
local sinks = ObjectManager { Interest { type = "node", Constraint { "media.class", "=", "Audio/Sink" } } }
local metadata = ObjectManager { Interest { type = "metadata", Constraint { "metadata.name", "=", "default" } } }

local function sink_class(node)
	local p = node.properties
	local name = p["node.name"] or ""
	if p["device.api"] == "bluez5" or name:find("^bluez_output") then
		return "bt"
	elseif name:find("^alsa_output%.usb%-") then
		return "usb"
	elseif name:find("hdmi") then
		return "hdmi"
	elseif name:find("iec958") then
		return "spdif"
	elseif name:find("analog") then
		return "analog"
	end
	return "other"
end

local profile_part = { hdmi = "hdmi", spdif = "iec958", analog = "analog" }

local function profiles(device)
	local list = {}
	for p in device:iterate_params("EnumProfile") do
		local pr = p:parse().properties
		table.insert(list, pr)
	end
	return list
end

local function set_default(node)
	local m = metadata:lookup()
	m:set(0, "default.configured.audio.sink", "Spa:String:JSON",
		Json.Object { name = node.properties["node.name"] }:to_string())
	print("output: " .. node.properties["node.description"])
end

local function find_sink(class)
	for node in sinks:iterate() do
		if sink_class(node) == class then
			return node
		end
	end
end

local function switch_profile(class)
	local part = profile_part[class]
	if not part then
		return false
	end
	local best, best_device, best_rank
	for device in devices:iterate() do
		if device.properties["device.api"] == "alsa" and device.properties["device.bus"] ~= "usb" then
			for _, pr in ipairs(profiles(device)) do
				local rank = (pr.available == "no" and 0 or 100000) + (pr.priority or 0)
				if pr.name and pr.name:find("^output:" .. part) and (not best or rank > best_rank) then
					best, best_device, best_rank = pr, device, rank
				end
			end
		end
	end
	if not best then
		return false
	end
	best_device:set_param("Profile", Pod.Object {
		"Spa:Pod:Object:Param:Profile", "Profile", index = best.index, save = true })
	return true
end

local function run()
	if wanted == "list" then
		for node in sinks:iterate() do
			print("sink " .. sink_class(node) .. " " .. node.properties["node.name"])
		end
		for device in devices:iterate() do
			for _, pr in ipairs(profiles(device)) do
				print("profile " .. device.properties["device.name"] .. " " .. tostring(pr.name) .. " " .. tostring(pr.available))
			end
		end
		Core.quit()
		return
	end
	if wanted == "auto" then
		metadata:lookup():set(0, "default.configured.audio.sink", nil, nil)
		for device in devices:iterate() do
			if device.properties["device.api"] == "alsa" then
				local best
				for _, pr in ipairs(profiles(device)) do
					if pr.name and pr.name ~= "off" and pr.name ~= "pro-audio" and pr.available ~= "no"
							and (not best or (pr.priority or 0) > (best.priority or 0)) then
						best = pr
					end
				end
				if best then
					device:set_param("Profile", Pod.Object {
						"Spa:Pod:Object:Param:Profile", "Profile", index = best.index, save = true })
				end
			end
		end
		print("output: chosen by WirePlumber")
		Core.quit()
		return
	end
	local node = find_sink(wanted)
	if node then
		set_default(node)
		Core.quit()
		return
	end
	if not switch_profile(wanted) then
		print("no " .. wanted .. " output")
		Core.quit()
		return
	end
	sinks:connect("object-added", function(_, n)
		if sink_class(n) == wanted then
			set_default(n)
			Core.quit()
		end
	end)
	Core.timeout_add(5000, function()
		print("no " .. wanted .. " output after switching the profile")
		Core.quit()
		return false
	end)
end

devices:activate()
sinks:activate()
metadata:activate()
Core.sync(function() run() end)
