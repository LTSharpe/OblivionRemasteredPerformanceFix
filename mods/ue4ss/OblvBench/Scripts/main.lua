-- OblvBench: in-game side of tools\Run-Benchmark.ps1 (investigation tool, not part of the shipped fix).
-- This bootstrap polls bench-commands.txt in the runtime folder and forwards new lines to bench.lua in the
-- project folder, which is re-read on "reload" so it can change without restarting the game.
-- Log lines (bench.log in the runtime folder) start with os.clock() seconds (wall clock since process start).
--
-- Everything runs on the game thread, from one UE4SS frame loop (LoopInGameThreadAfterFrames):
-- bench.lua's Frame every frame (flight, walking), the command poll every 50 ms (every 0.5 s when
-- no command came for 30 s), and bench.lua's Tick at the poll rate while it is busy. No Lua of
-- this mod runs on UE4SS's async thread in normal operation: the async thread and the game thread
-- share one Lua state and Lua is not thread-safe, which is what silently killed the old
-- LoopAsync flight loop. If the frame loop is not available or
-- never runs, the bootstrap falls back to the old LoopAsync + ExecuteInGameThread poll
-- ("driver async": commands work, flight does not). From that fallback it registers a new frame loop from the game
-- thread every few seconds; the first loop that runs takes over and the async poll stops (2026-10-31: a game started
-- through its first-launch shader screen, which OblivionPerfFix's ShaderScreenAfterDriverChange brings back after a driver
-- update, never ran the loop registered at mod start).

-- Both folders are written into the deployed copy of this file by tools\Install-UE4SS.ps1 (the repo, and the runtime
-- folder from tools\GamePaths.ps1 Get-BenchRuntimeDir: run\ in the public repo).
local PROJECT = "@PROJECT@"
local RUNTIME = "@RUNTIME@"
OBLV_RUNTIME = RUNTIME   -- for bench.lua
local CMD_FILE = RUNTIME .. "bench-commands.txt"
local LOG_FILE = RUNTIME .. "bench.log"
local HEARTBEAT_FILE = RUNTIME .. "bench-heartbeat.txt"   -- "<os.clock> <frames> <driver> <pid-less stamp>", rewritten once a second
local IMPL_FILE = PROJECT .. "mods/ue4ss/OblvBench/Scripts/bench.lua"
local POLL_MS = 50
local IDLE_AFTER_S, IDLE_POLL_S = 30, 0.5
local FRAME_STEP = 1      -- LoopInGameThreadAfterFrames(n): 1 should mean every frame; "ping" shows loops_per_s to compare with the fps

local function log(msg)
    local f = io.open(LOG_FILE, "a")
    if f then f:write(string.format("%.3f %s\n", os.clock(), msg)); f:close() end
end

-- Errors from code that runs every frame: the first one is logged, then at most one line a second.
local errAt, errCount = {}, {}
local function LogError(where, err)
    local now = os.clock()
    errCount[where] = (errCount[where] or 0) + 1
    if not errAt[where] or now - errAt[where] >= 1 then
        errAt[where] = now
        log(string.format("ERROR %s (x%d): %s", where, errCount[where], (tostring(err):gsub("[\r\n]+", " | "))))
        errCount[where] = 0
    end
end

-- Shared with bench.lua (Init's second argument): how it is driven, for `ping` and for verbs that
-- need a per-frame callback.
local driver = { mode = "starting", frames = 0, polls = 0, started = os.clock() }

-- Heartbeat for the runner's watchdog: proves the game thread still runs frames (a hung or crashed game
-- stops rewriting it). One small file write a second; never fails loudly.
local nextBeat = 0
local function Heartbeat(now)
    if now < nextBeat then return end
    nextBeat = now + 1.0
    local f = io.open(HEARTBEAT_FILE, "w")
    if f then f:write(string.format("%.3f %d %s %s", now, driver.frames, driver.mode, driver.state or "")); f:close() end
end

local impl = nil
local function LoadImpl()
    if impl and impl.Unload then                    -- e.g. stop a flight and restore the pawn
        local ok, err = pcall(impl.Unload)
        if not ok then log("ERROR unloading bench.lua: " .. tostring(err)) end
    end
    local ok, result = pcall(dofile, IMPL_FILE)
    if ok and type(result) == "table" then
        impl = result
        impl.Init(log, driver)
        log("LOADED bench.lua (driver " .. driver.mode .. ")")
    else
        log("ERROR loading bench.lua: " .. tostring(result))
    end
end

-- Commands written before this process started are not ours: begin at the end of the file.
local offset = 0
do
    local f = io.open(CMD_FILE, "rb")
    if f then offset = f:seek("end"); f:close() end
end

local function ReadCommands()
    local lines = {}
    local f = io.open(CMD_FILE, "rb")
    if not f then return lines end
    local ok, err = pcall(function()
        local size = f:seek("end")
        if size < offset then offset = 0 end          -- file was truncated: a new run
        if size > offset then
            f:seek("set", offset)
            local data = f:read(size - offset) or ""
            local lastNl = data:match(".*()\n")       -- only consume complete lines
            if lastNl then
                offset = offset + lastNl
                for line in data:sub(1, lastNl):gmatch("[^\r\n]+") do lines[#lines + 1] = line end
            end
        end
    end)
    f:close()
    if not ok then error(err) end
    return lines
end

local function RunLines(lines)
    for _, line in ipairs(lines) do
        if line:match("^%s*%d*%s*reload%s*$") then
            LoadImpl()
            local id = line:match("^%s*(%d+)")
            if id then log("ACK " .. id .. " OK reloaded") end
        elseif impl then
            local ok, err = pcall(impl.Run, line)
            if not ok then log("ERROR " .. (tostring(err):gsub("[\r\n]+", " | ")) .. " for: " .. line) end
        end
    end
end

local function Busy() return impl and impl.WantsTick and impl.WantsTick() end

local lastCommand, lastPoll = -1000, -1000

-- ---------------------------------------------------------------- frame driver (normal)

local nextPoll, loaded = 0, false
local function OnFrame()
    if driver.mode ~= "frame" then return end         -- fell back to the async poll: never run both
    driver.frames = driver.frames + 1
    local now = os.clock()
    pcall(Heartbeat, now)
    if not loaded then loaded = true; LoadImpl() end
    if impl and impl.Frame then
        local ok, err = pcall(impl.Frame, now)
        if not ok then LogError("frame", err) end
    end
    if now < nextPoll then return end
    nextPoll = now + POLL_MS / 1000
    local busy = Busy()
    if not busy and now - lastCommand > IDLE_AFTER_S and now - lastPoll < IDLE_POLL_S then return end
    lastPoll = now
    driver.polls = driver.polls + 1
    local ok, lines = pcall(ReadCommands)
    if not ok then LogError("poll", lines); lines = {} end
    if #lines > 0 then lastCommand = now; RunLines(lines) end
    if (busy or #lines > 0) and impl and impl.Tick then
        local ok2, err = pcall(impl.Tick)
        if not ok2 then LogError("tick", err) end
    end
end

-- Each registered loop has an id; only the active one (driver.loopId) drives OnFrame. A loop registered from the async
-- fallback (rearm) becomes the active one the first time it runs; any other loop that runs later does nothing.
local loopSeq, loopHandles = 0, {}
local function StartFrameDriver(rearm)
    if type(LoopInGameThreadAfterFrames) ~= "function" then return false, "LoopInGameThreadAfterFrames is missing" end
    loopSeq = loopSeq + 1
    local id = loopSeq
    local ok, handle = pcall(LoopInGameThreadAfterFrames, FRAME_STEP, function()
        if driver.loopId ~= id then
            if driver.mode ~= "async" or driver.loopId ~= nil then          -- not the active loop: cancel it if possible
                if loopHandles[id] ~= nil and type(CancelDelayedAction) == "function" then pcall(CancelDelayedAction, loopHandles[id]); loopHandles[id] = nil end
                return
            end
            driver.loopId = id; driver.mode = "frame"
            log(string.format("frame loop running (loop %d, %.1f s after start): the async poll stops", id, os.clock() - driver.started))
        end
        local ok2, err = pcall(OnFrame)              -- never let an error reach UE4SS (it may drop the loop)
        if not ok2 then LogError("driver", err) end
    end)
    if not ok then return false, tostring(handle) end
    loopHandles[id] = handle
    if not rearm then driver.mode = "frame"; driver.loopId = id; driver.handle = handle end
    return true
end

-- ---------------------------------------------------------------- async driver (fallback)
-- The pre-2026-10-04 bootstrap: a 50 ms LoopAsync poll that hands work to the game thread.

-- Test switch for the fallback above (lab: tools\Run-Benchmark.ps1 -TestSkipFrameLoop <s>; off unless the file exists):
-- bench-test-frameloop.txt in the runtime folder ("skip <s>"), read and removed at start, so only the next game is
-- affected. The frame loop is not registered at start (as in a game started through the shader screen: the loop never
-- runs), so the 20 s check falls back to the async poll; its re-arms begin <s> seconds after the mod started.
local testSkip, testRearmAt = false, 0
do
    local path = RUNTIME .. "bench-test-frameloop.txt"
    local f = io.open(path, "r")
    if f then
        local text = f:read("*a") or ""; f:close(); os.remove(path)
        local n = tonumber(text:match("skip%s+(%d+)"))
        if n then testSkip = true; testRearmAt = n end
    end
end

local function StartAsyncDriver()
    driver.mode = "async"; driver.loopId = nil
    local queue, pending, pendingSince = {}, false, 0
    local nextRearm, rearms = math.max(os.clock() + 5, driver.started + testRearmAt), 0
    LoopAsync(POLL_MS, function()
        if driver.mode == "frame" then                   -- a re-armed frame loop took over: this poll ends
            if #queue > 0 then
                local left = queue; queue = {}
                log(string.format("%d queued command line(s) handed to the frame loop", #left))
                ExecuteInGameThread(function() RunLines(left) end)
            end
            return true
        end
        local ok, err = pcall(function()
            local now = os.clock()
            pcall(Heartbeat, now)
            -- try the frame loop again, registered from the game thread, every 5 s (at most 120 times: 10 min)
            if now >= nextRearm and rearms < 120 and type(ExecuteInGameThread) == "function" then
                nextRearm = now + 5; rearms = rearms + 1
                ExecuteInGameThread(function()
                    if driver.mode ~= "async" then return end
                    local okR, whyR = StartFrameDriver(true)
                    if not okR and rearms == 1 then log("WARN frame loop re-arm failed: " .. tostring(whyR)) end
                end)
            end
            if pending and now - pendingSince > 2 then pending = false; log("WARN game-thread callback lost, re-armed") end
            local busy = Busy()
            if not busy and now - lastCommand > IDLE_AFTER_S and now - lastPoll < IDLE_POLL_S then return end
            lastPoll = now
            driver.polls = driver.polls + 1
            local lines = ReadCommands()
            if #lines > 0 then
                lastCommand = now
                for _, line in ipairs(lines) do queue[#queue + 1] = line end
            end
            if (#queue > 0 or busy) and not pending then
                pending = true; pendingSince = now
                ExecuteInGameThread(function()
                    local batch = queue
                    queue = {}
                    local ok2, err2 = pcall(function()
                        RunLines(batch)
                        if impl and impl.Tick then impl.Tick() end
                    end)
                    if not ok2 then LogError("tick", err2) end
                    pending = false
                end)
            end
        end)
        if not ok then LogError("poll", err) end
        return false                                  -- keep the loop
    end)
    if not loaded then loaded = true; ExecuteInGameThread(LoadImpl) end
end

-- ---------------------------------------------------------------- start

local started, why
if testSkip then
    log(string.format("TEST frame loop not registered at start (bench-test-frameloop.txt): fallback after 20 s, re-arms from %d s", testRearmAt))
    driver.mode = "frame"; started = true                -- as a registered loop that never runs
else
    started, why = StartFrameDriver()
end
if started then
    -- One-off check that the frame loop really runs; this tiny async callback removes itself as soon
    -- as it has seen a frame (normally on its first call).
    local t0 = os.clock()
    LoopAsync(1000, function()
        if driver.frames > 0 then return true end
        if os.clock() - t0 > 20 then
            log("WARN the frame loop did not run within 20 s: falling back to the async poll")
            if driver.handle ~= nil and type(CancelDelayedAction) == "function" then pcall(CancelDelayedAction, driver.handle) end
            StartAsyncDriver()
            return true
        end
        return false
    end)
else
    log("WARN no frame loop (" .. tostring(why) .. "): using the async poll")
    StartAsyncDriver()
end
log("READY OblvBench bootstrap (driver " .. driver.mode .. ", watching " .. CMD_FILE .. ")")
