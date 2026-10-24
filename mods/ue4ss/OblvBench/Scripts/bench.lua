-- OblvBench command handling; loaded by the OblvBench bootstrap from the project folder and
-- re-read on "reload". One command per line in bench-commands.txt (runtime folder):
--     <id> <verb> [arguments]
-- answered in bench.log (runtime folder) by "ACK <id> OK <result>" or "ACK <id> ERR <message>".
-- Positions are in Oblivion units (what `player.getpos` prints); headings are Oblivion
-- degrees (0 = north, clockwise). UE location = Oblivion * 1.428571 with Y negated,
-- UE yaw = heading - 90.

local M = {}
local log = print
local driver = { mode = "legacy", frames = 0 }   -- set by the bootstrap (main.lua); "frame" = M.Frame runs every frame
local S = 1.428571

-- ---------------------------------------------------------------- object access

local cache, missAt = {}, {}
local function Obj(cls)
    local o = cache[cls]
    if o and o:IsValid() then return o end
    cache[cls] = nil
    -- FindFirstOf scans every object when the class has no instance: at most once a second
    local now = os.clock()
    if missAt[cls] and now - missAt[cls] < 1.0 then return nil end
    o = FindFirstOf(cls)
    if o and o:IsValid() then cache[cls] = o; missAt[cls] = nil; return o end
    missAt[cls] = now
    return nil
end

local function PC()
    local pc = cache.__pc
    if pc and pc:IsValid() then return pc end
    cache.__pc = nil
    for _, p in ipairs(FindAllOf("PlayerController") or {}) do
        if p:IsValid() then
            local ok, has = pcall(function() return p.Player and p.Player:IsValid() end)
            if ok and has then cache.__pc = p; return p end
        end
    end
    return nil
end

local function Pawn()
    local pc = PC()
    if not pc then return nil end
    local ok, pawn = pcall(function() return pc.Pawn end)
    if ok and pawn and pawn:IsValid() then return pawn end
    return nil
end

local function Kismet()
    local k = cache.__kismet
    if k and k:IsValid() then return k end
    k = StaticFindObject("/Script/Engine.Default__KismetSystemLibrary")
    cache.__kismet = k
    return k
end

-- The HUD. The primary game layout holds the gameplay HUD ("GameStack": compass, reticle, icons,
-- notifications) next to the menu, modal, fade and loading-screen stacks. VAltarHud:ToggleHUDVisibility
-- hides the whole layout, and then door transitions hang half-way (frozen behind a dark screen until it
-- is shown again) and activation finds nothing. So only GameStack is hidden, by render opacity: it stays
-- logically visible (activation works) and fades and loading screens show as usual. The game's software
-- cursor appears once the HUD is out of the way; it resets the cursor's visibility, not its opacity.
-- (VAltarUISubsystem:IsHUDVisible says true whatever is done.)
local function HudStack()
    local hud = Obj("VAltarHud")
    if not hud then return nil end
    local ok, w = pcall(function() return hud:GetPrimaryGameLayout().GameStack end)     -- the layout's bound widget
    if ok and w and w:IsValid() then return w end
    return nil
end

local function HudShown()
    local w = HudStack()
    if not w then return false end
    local ok, v = pcall(function() return w:GetRenderOpacity() end)
    return not ok or v > 0.01
end

local function SetHud(show)
    local w = HudStack()
    if w then w:SetRenderOpacity(show and 1.0 or 0.0) end
    pcall(function() Obj("VAltarCursor"):SetRenderOpacity(show and 1.0 or 0.0) end)
    -- the game's FPS / VRAM counter (top right) shows even with Altar.GraphicsOptions.ShowFPS 0
    pcall(function() Obj("AltarGraphicsSettingsDebugInfo"):SetRenderOpacity(show and 1.0 or 0.0) end)
    return w ~= nil
end

local function Cvar(name)
    local ok, v = pcall(function() return Kismet():GetConsoleVariableFloatValue(name) end)
    return ok and v or nil
end

-- The UFunction's parameter count as seen by UE4SS differs between builds: try the known
-- call shapes and remember the first one that works (same approach as OblvLab).
local execVariant = nil
local EXEC_VARIANTS = {
    function(k, pc, cmd) k:ExecuteConsoleCommand(pc, cmd, pc, nil) end,
    function(k, pc, cmd) k:ExecuteConsoleCommand(pc, cmd, pc) end,
    function(k, pc, cmd) k:ExecuteConsoleCommand(pc, cmd, nil, nil) end,
}
local function Exec(cmd)
    local k, pc = Kismet(), PC()
    if not pc then error("no PlayerController") end
    if execVariant then return EXEC_VARIANTS[execVariant](k, pc, cmd) end
    local errs = {}
    for i, fn in ipairs(EXEC_VARIANTS) do
        local ok, err = pcall(fn, k, pc, cmd)
        if ok then execVariant = i; return end
        errs[#errs + 1] = tostring(err):gsub("\n.*", "")
    end
    error("ExecuteConsoleCommand failed: " .. table.concat(errs, " | "))
end

-- Oblivion (old engine) console command, through the cheat manager's exec function
local function Obv(cmd) Exec("ObvConsole " .. cmd) end

-- ---------------------------------------------------------------- state

local function num(v) return string.format("%.1f", v) end

local function LevelName(pc)
    -- ".../Game/Maps/World/L_CheydinhalWorld.L_CheydinhalWorld:PersistentLevel.BP_AltarPlayerController_C_1"
    return pc:GetFullName():match("([%w_]+):PersistentLevel") or "?"
end

-- The old engine's cell of the player, as the pairing layer reports it (empty or "?" if unknown).
-- Changes as the player crosses exterior cells if the old engine follows the UE pawn.
local function Cell(pawn)
    local ok, v = pcall(function() return pawn.TESRefComponent.CellNameAtRuntime:ToString() end)
    return ok and v or "?"
end

-- Exterior cell grid coordinates of a position (cells are 4096 units)
local function Grid(x, y) return string.format("%d,%d", math.floor(x / 4096), math.floor(y / 4096)) end

local function State(light)
    local s = {}
    local function put(k, v) s[#s + 1] = k .. "=" .. tostring(v) end
    local function try(k, fn)
        local ok, v = pcall(fn)
        if ok and v ~= nil then put(k, v) end
    end
    local pc = PC()
    put("pc", pc and 1 or 0)
    if pc then
        put("level", LevelName(pc))
        local pawn = Pawn()
        put("pawn", pawn and 1 or 0)
        if pawn then
            local l = pawn:K2_GetActorLocation()
            put("x", num(l.X / S)); put("y", num(-l.Y / S)); put("z", num(l.Z / S))
            local r = pc:GetControlRotation()
            put("heading", num((r.Yaw + 90) % 360)); put("pitch", num(r.Pitch))
            try("sub", function() return pawn:GetSubLevelName():ToString() end)
        end
        try("pov", function() return pc.POV end)
    end
    try("hour", function() return string.format("%.2f", Obj("ATMSubsystem"):GetTime()) end)
    local lcd = Obj("VLevelChangeData")
    if lcd then
        try("travel", function() return (lcd.bIsFastTravelling or lcd.bIsSeamlessTravelling) and 1 or 0 end)
        try("subloaded", function() return lcd.bIsCurrentSubLevelLoaded and 1 or 0 end)
    end
    if light then return table.concat(s, " ") end
    local ui = Obj("VAltarUISubsystem")
    if ui then
        try("menu", function() return ui:IsMenuVisible() and 1 or 0 end)
        try("hud", function() return HudShown() and 1 or 0 end)
        try("loadctx", function() return ui:GetCurrentLoadingScreenContext() end)
    end
    put("loadscreen", Obj("VLoadingScreen") and 1 or 0)
    try("frozen", function() return Obj("VFreezeInMenuSubsystem"):IsFreezing() and 1 or 0 end)
    local mm = Obj("VMainMenuViewModel")
    put("mainmenu", mm and 1 or 0)
    if mm then
        try("movie", function() return mm:GetIsMoviePlaying() and 1 or 0 end)
        try("cont", function() return mm:GetContinueVisibility() and 1 or 0 end)
        try("bgloaded", function() return mm:IsBackgroundLoaded() and 1 or 0 end)
    end
    return table.concat(s, " ")
end

-- ---------------------------------------------------------------- watcher
-- While watching, log "EV <fields>" whenever the coarse state changes (level, sublevel, old-engine
-- cell, loading/travel/menu flags, teleports). Gives the phases of a door transition, and the cell
-- changes along a flight, without hooks.

local watching, lastSig, lastPos = false, nil, nil
local function WatchTick()
    local parts = {}
    local pc = PC()
    local pawn = pc and Pawn() or nil
    parts[#parts + 1] = "level=" .. (pc and LevelName(pc) or "none")
    parts[#parts + 1] = "pawn=" .. (pawn and 1 or 0)
    local function try(k, fn)
        local ok, v = pcall(fn)
        if ok and v ~= nil then parts[#parts + 1] = k .. "=" .. tostring(v) end
    end
    if pawn then
        try("sub", function() return pawn:GetSubLevelName():ToString() end)
        local l = pawn:K2_GetActorLocation()
        parts[#parts + 1] = "grid=" .. Grid(l.X / S, -l.Y / S)
    end
    local lcd = Obj("VLevelChangeData")
    if lcd then
        try("travel", function() return (lcd.bIsFastTravelling or lcd.bIsSeamlessTravelling) and 1 or 0 end)
        -- which of the two flags (fast travel, seamless travel): a fast-transition exit leaves one set (doors2)
        try("tv", function() return (lcd.bIsFastTravelling and "1" or "0") .. (lcd.bIsSeamlessTravelling and "1" or "0") end)
        try("subloaded", function() return lcd.bIsCurrentSubLevelLoaded and 1 or 0 end)
    end
    local ui = Obj("VAltarUISubsystem")
    if ui then
        try("menu", function() return ui:IsMenuVisible() and 1 or 0 end)
        try("loadctx", function() return ui:GetCurrentLoadingScreenContext() end)
    end
    try("frozen", function() return Obj("VFreezeInMenuSubsystem"):IsFreezing() and 1 or 0 end)
    -- probes can add fields: a Lua file run with `luafile` sets BenchWatchExtra = function() return "k=v ..." end
    -- (called at the watch rate, pcall-guarded; nil it to stop)
    if BenchWatchExtra then
        local ok, v = pcall(BenchWatchExtra)
        parts[#parts + 1] = ok and tostring(v) or "xerr=1"
    end
    local sig = table.concat(parts, " ")
    local jump = ""
    if pawn then
        local l = pawn:K2_GetActorLocation()
        local p = { l.X / S, -l.Y / S, l.Z / S }
        if lastPos then
            local d = math.sqrt((p[1] - lastPos[1]) ^ 2 + (p[2] - lastPos[2]) ^ 2 + (p[3] - lastPos[3]) ^ 2)
            if d > 400 then jump = string.format(" jump=%.0f to=%.0f,%.0f,%.0f", d, p[1], p[2], p[3]) end
        end
        lastPos = p
    end
    if sig ~= lastSig or jump ~= "" then
        lastSig = sig
        log("EV " .. sig .. jump)
    end
end

local function NormAngle(a)
    a = a % 360
    if a > 180 then a = a - 360 end
    return a
end

local function Dist3(ax, ay, az, bx, by, bz) return math.sqrt((ax - bx) ^ 2 + (ay - by) ^ 2 + (az - bz) ^ 2) end

local function Action(path)
    local a = cache[path]
    if a and a:IsValid() then return a end
    a = StaticFindObject(path)
    if not (a and a:IsValid()) then error("input action not found: " .. path) end
    cache[path] = a
    return a
end

-- The game's own input action for one frame, exactly what the key does; works with the window in the background.
local function Inject(path)
    local sub = Obj("EnhancedInputLocalPlayerSubsystem")
    if not sub then error("no EnhancedInputLocalPlayerSubsystem") end
    sub:InjectInputVectorForAction(Action(path), { X = 1.0, Y = 0.0, Z = 0.0 }, {}, {})
end

local IA = "/Game/Dev/Input/GamePlay/InputActions/"
local ACTIVATE_ACTION = IA .. "Actions/IA_Game_Actions_Activate.IA_Game_Actions_Activate"
local FORWARD_ACTION = IA .. "Movement/IA_Game_Movement_MovementForward.IA_Game_Movement_MovementForward"
local AUTOMOVE_ACTION = IA .. "Movement/IA_Game_Movement_AutoMove.IA_Game_Movement_AutoMove"

-- ---------------------------------------------------------------- walking a route
-- Steers the control rotation toward the next waypoint every frame; forward motion comes from the
-- game's MoveForward input action injected every frame like a held key (mode "inject", default),
-- from APawn::AddMovementInput (mode "input"), or from the game's auto-move toggle (mode "auto").
-- Runs from M.Frame with the frame driver, from M.Tick (20 Hz) with the legacy bootstrap.

local walk = nil

local function WalkStop(reason)
    if not walk then return end
    local w = walk
    walk = nil
    local pc = PC()
    if w.mode == "auto" and pc then pcall(function() pc:DisableAutoMoveToggle() end) end
    local secs = os.clock() - w.t0
    log(string.format("EV walk=done reason=%s waypoint=%d/%d secs=%.1f dist=%.0f speed=%.0f frames=%d pos=%.0f,%.0f,%.0f",
        reason, w.i - 1, #w.pts, secs, w.dist, secs > 0 and w.dist / secs or 0, w.frames, w.x or 0, w.y or 0, w.z or 0))
end

local function WalkTick(now)
    local pc, pawn = PC(), Pawn()
    if not (pc and pawn) then return WalkStop("nopawn") end
    if now > walk.deadline then return WalkStop("timeout") end
    local l = pawn:K2_GetActorLocation()
    local x, y, z = l.X / S, -l.Y / S, l.Z / S
    if walk.x then walk.dist = walk.dist + math.sqrt((x - walk.x) ^ 2 + (y - walk.y) ^ 2) end
    walk.x, walk.y, walk.z = x, y, z
    walk.frames = walk.frames + 1
    if not walk.sx then walk.sx, walk.sy, walk.st = x, y, now end
    if now - walk.st >= 3 then                                -- stuck: under 100 units in 3 s
        if math.sqrt((x - walk.sx) ^ 2 + (y - walk.sy) ^ 2) < 100 then return WalkStop("stuck") end
        walk.sx, walk.sy, walk.st = x, y, now
    end
    local t = walk.pts[walk.i]
    local dx, dy = t[1] - x, t[2] - y
    if math.sqrt(dx * dx + dy * dy) < walk.radius then
        walk.i = walk.i + 1
        if walk.i > #walk.pts then return WalkStop("arrived") end
        t = walk.pts[walk.i]; dx, dy = t[1] - x, t[2] - y
    end
    local wantYaw = math.deg(math.atan(dx, dy)) - 90          -- heading -> UE yaw
    local r = pc:GetControlRotation()
    local dt = math.min(0.25, now - walk.last); walk.last = now
    local delta = NormAngle(wantYaw - r.Yaw)
    local maxTurn = walk.turnRate * dt
    if delta > maxTurn then delta = maxTurn elseif delta < -maxTurn then delta = -maxTurn end
    local yaw = r.Yaw + delta
    pc:SetControlRotation({ Pitch = walk.pitch, Yaw = yaw, Roll = 0.0 })
    if walk.mode == "inject" then
        Inject(FORWARD_ACTION)
    elseif walk.mode == "input" then
        local a = math.rad(yaw)
        pawn:AddMovementInput({ X = math.cos(a), Y = math.sin(a), Z = 0.0 }, 1.0, false)
    end
    if now - walk.lastProg >= 1 then                          -- every second (2 s until 2026-10-14)
        log(string.format("EV walk=prog t=%.1f wp=%d/%d pos=%.0f,%.0f,%.0f speed=%.0f cell=%s", now - walk.t0, walk.i, #walk.pts,
            x, y, z, (walk.dist - walk.progDist) / (now - walk.lastProg), Grid(x, y)))
        walk.lastProg, walk.progDist = now, walk.dist
    end
end

-- ---------------------------------------------------------------- flying
-- The pawn is a stock UE Character (VPairedPawn, CharacterMovementComponent), so flight uses the
-- movement component the way the engine's `ghost` cheat does (compiled out of this Shipping build):
-- no actor collision, gravity scale 0, MOVE_Flying. Every frame, before the engine ticks, the velocity
-- is set and the engine integrates the position itself, so the old engine sees ordinary continuous
-- motion. The engine's step is the previous frame's duration, so the position read in a frame is
-- where the player was meant to be one frame ago: the velocity is (timetable(previous frame + h) -
-- position) / h with h = max(tau, frame time). On schedule that is exactly the flight speed, a
-- long frame is made up by the engine's own long step, and an error decays within `tau`.
-- (Teleporting the pawn from the UE side every frame, the first attempt, lost the player.)
-- The timetable depends only on elapsed time, so every run shows the same place
-- at the same second. A flight first lifts the player to the first waypoint and hovers there for
-- `hold` seconds ("EV fly=start" marks the start of the timed part; "EV fly=done" its end).
--
-- Options (defaults below; "flyopt k=v ..." changes them until the next reload, "fly ... k=v" for one flight):
--   drive     vel (set the movement component's Velocity) | launch (LaunchCharacter, with mode=falling)
--             | input (AddMovementInput, approximate) | obv (old engine `player.setpos` at 20 Hz)
--             | tp (K2_SetActorLocation every frame: the first attempt, for comparison)
--   mode      flying (MOVE_Flying) | falling (MOVE_Falling with gravity 0: keeps velocity without friction)
--   tau       seconds in which an error decays               cap   top speed as a multiple of the flight speed
--   lift      1 = lift to the first waypoint first, 0 = start at once from where the player is
--   hold      seconds to hover at the first waypoint         liftspeed  units per second while lifting
--   collision 1 = keep the actor's collision on              brake 1 = keep BrakingDecelerationFlying
--   prog      seconds between "EV fly=prog" lines (0 = none)
--   ease      seconds to speed up from standing still at the start   easeout  seconds to slow down to a stop at the end
--   turn      degrees per second the view may turn
--   holdyaw   heading (Oblivion degrees) to look at while hovering, -1 = along the path; holdpitch  pitch then (999 = the flight's)
--   fadein    seconds to fade in from black at the start of the timed flight (0 = no fade; send "fade 1" before the flight)
--   fadeout   seconds to fade to black before the end      stay  1 = keep hovering at the end (no fall) until "flystop"
--   clip      1 = trace the camera's path every frame and log "EV fly=clip" where it passes through something solid
--   camlook   seconds ahead along the path the spectator camera aims at (0 = max(800 units, one second))
--   camtau    seconds in which the spectator camera's turning settles     camsmooth  cascaded smoothing stages (1 or more:
--             2 = no sudden change in turn rate either)
--   cameye    spectator camera height above the path points in Oblivion units during the timed flight (0 = the eye height
--             measured from the game's camera when the camera went on, 75-146 UE units depending on its state; set it for
--             runs that must render the same view, e.g. 55 = a standing player's eye over a path at the capsule centre)
-- Waypoints are x,y,z or x,y,z,pitch or x,y,z,pitch,speed (all the same): a pitch per waypoint is the camera's pitch
-- there, a speed the flight's speed there (both spline-interpolated); with speeds the timetable follows them (no easing).

local MOVE_FALLING, MOVE_FLYING = 3, 5
local function Mode(m) return tonumber(m.MovementMode) or -1 end
local FLY_DEFAULTS = { drive = "vel", mode = "flying", tau = 0.25, cap = 2.5, lift = 1, hold = 1.0, liftspeed = 1200,
                       collision = 0, brake = 0, prog = 2.0, ease = 1.5, easeout = 1.5, turn = 60, holdyaw = -1, holdpitch = 999,
                       fadein = 0, fadeout = 0, stay = 0, clip = 0, camlook = 0, camtau = 0.55, camsmooth = 1, cameye = 0 }

-- ---------------------------------------------------------------- screen fade
-- The camera manager's fade: black over the 3D view (menus and loading screens stay on top), to hide set-up
-- (start cell, teleport, hover) from someone watching. Applied every frame from our own timeline, so a level
-- change or a new camera manager cannot show what it hides. Rendering goes on as usual underneath.
local fade = nil         -- { from, to, t0, secs }
local BLACK = { R = 0.0, G = 0.0, B = 0.0, A = 1.0 }

local function FadeAlpha(f, now)
    if f.secs <= 0 then return f.to end
    return f.from + (f.to - f.from) * math.min(1, (now - f.t0) / f.secs)
end

local function FadeTo(to, secs, now)
    now = now or os.clock()
    fade = { from = fade and FadeAlpha(fade, now) or 0, to = to, t0 = now, secs = math.max(0, secs or 0) }
end

local function FadeFrame(now)
    local f = fade
    local pc = PC()
    if not pc then return end
    local cm = pc.PlayerCameraManager
    if not (cm and cm:IsValid()) then return end
    local a = FadeAlpha(f, now)
    if a <= 0 and now - f.t0 >= f.secs then cm:StopCameraFade(); fade = nil; return end
    cm:SetManualCameraFade(a, BLACK, false)
end
local flyOpts = {}

local fly = nil          -- the flight or flight test in progress (driven by M.Frame)
local saved = nil        -- movement settings before flight mode, restored by FlightOff
local lastTest = "none"  -- summary of the last flight test

local function ParseOpts(text, into)
    for k, v in text:gmatch("([%a_]+)=(%S+)") do
        if FLY_DEFAULTS[k] == nil then error("unknown flight option " .. k) end
        into[k] = tonumber(v) or v
    end
    return into
end

local function FlyOptions(text)
    local o = {}
    for k, v in pairs(FLY_DEFAULTS) do o[k] = v end
    for k, v in pairs(flyOpts) do o[k] = v end
    ParseOpts(text or "", o)
    if o.drive == "ue" then o.drive = "tp" end                 -- the old name
    if o.drive == "launch" then o.mode = "falling" end         -- LaunchCharacter switches to falling anyway
    return o
end

local function OptString(o)
    local keys = {}
    for k in pairs(FLY_DEFAULTS) do keys[#keys + 1] = k end
    table.sort(keys)
    local s = {}
    for _, k in ipairs(keys) do s[#s + 1] = k .. "=" .. tostring(o[k]) end
    return table.concat(s, " ")
end

local function GetMovement(pawn)
    local ok, m = pcall(function() return pawn.CharacterMovement end)
    if ok and m and m:IsValid() then return m end
    ok, m = pcall(function() return pawn.PairedPawnMovementComponent end)
    if ok and m and m:IsValid() then return m end
    error("no movement component on the pawn")
end

-- Velocity is an FVector property: assign a table, or write the fields of the struct in place.
-- The first form that reads back correctly is remembered.
local velVariant = nil
local VEL_VARIANTS = {
    function(m, x, y, z) m.Velocity = { X = x, Y = y, Z = z } end,
    function(m, x, y, z) local v = m.Velocity; v.X = x; v.Y = y; v.Z = z end,
}
local function SetVelocity(m, x, y, z)
    if velVariant then return VEL_VARIANTS[velVariant](m, x, y, z) end
    local probe = math.abs(x) + math.abs(y) + math.abs(z) > 10   -- a zero vector proves nothing
    local errs = {}
    for i, fn in ipairs(VEL_VARIANTS) do
        local ok, err = pcall(fn, m, x, y, z)
        if ok and not probe then return end
        if ok then
            local v = m.Velocity
            if math.abs(v.X - x) < 1 and math.abs(v.Y - y) < 1 and math.abs(v.Z - z) < 1 then
                velVariant = i; log("velocity write: form " .. i); return
            end
            err = string.format("read back %.0f,%.0f,%.0f", v.X, v.Y, v.Z)
        end
        errs[#errs + 1] = i .. ": " .. tostring(err):gsub("\n.*", "")
    end
    error("cannot set Velocity: " .. table.concat(errs, " | "))
end

local function FlightOn(pawn, m, o)
    if not saved then
        saved = { gravity = m.GravityScale, brake = m.BrakingDecelerationFlying, maxfly = m.MaxFlySpeed }
        local ok, c = pcall(function() return pawn:GetActorEnableCollision() end)
        saved.collision = not ok or c ~= false
        -- a player's mouse or keys (on-screen kit runs) must not steer the flight: the controller ignores look and move
        -- input until FlightOff. The flight itself is unaffected: it sets the control rotation directly and drives the
        -- pawn by velocity, LaunchCharacter or AddMovementInput(bForce = true), none of which checks these flags
        -- (UE 5.3 PlayerController.cpp AddYawInput/AddPitchInput, PawnMovementComponent Internal_AddMovementInput).
        local pc = PC()
        if pc then saved.input = pcall(function() pc:SetIgnoreLookInput(true); pc:SetIgnoreMoveInput(true) end) end
    end
    if o.collision == 0 then pawn:SetActorEnableCollision(false) end
    m.GravityScale = 0.0
    if o.brake == 0 then m.BrakingDecelerationFlying = 0.0 end
    m:SetMovementMode(o.mode == "falling" and MOVE_FALLING or MOVE_FLYING, 0)
    m:StopMovementImmediately()
end

-- Back to normal movement: gravity, collision, falling (lands; god mode is on). "" or " restore_failed=..."
local function FlightOff()
    if not saved then return "" end
    local s = saved
    saved = nil
    if s.input then
        local pc = PC()
        if pc then pcall(function() pc:ResetIgnoreLookInput(); pc:ResetIgnoreMoveInput() end) end
    end
    local pawn = Pawn()
    if not pawn then return " restore=nopawn" end
    local errs = {}
    local function try(k, fn) if not pcall(fn) then errs[#errs + 1] = k end end
    local m = nil
    try("cmc", function() m = GetMovement(pawn) end)
    if m then
        try("stop", function() m:StopMovementImmediately() end)
        try("gravity", function() m.GravityScale = s.gravity or 1.0 end)
        try("brake", function() if s.brake then m.BrakingDecelerationFlying = s.brake end end)
        if s.override then
            try("override", function() m:ResetOverrideMaxSpeed() end)
            try("maxfly", function() if s.maxfly then m.MaxFlySpeed = s.maxfly end end)
        end
    end
    try("collision", function() pawn:SetActorEnableCollision(s.collision ~= false) end)
    if m then try("mode", function() m:SetMovementMode(MOVE_FALLING, 0) end) end
    return #errs > 0 and (" restore_failed=" .. table.concat(errs, ",")) or ""
end

-- Catmull-Rom point on segment i (between pts[i] and pts[i+1]) at t in 0..1
local function Spline(pts, i, t)
    local p0, p1, p2, p3 = pts[math.max(1, i - 1)], pts[i], pts[i + 1], pts[math.min(#pts, i + 2)]
    local out = {}
    for k = 1, #p1 do
        out[k] = 0.5 * ((2 * p1[k]) + (-p0[k] + p2[k]) * t + (2 * p0[k] - 5 * p1[k] + 4 * p2[k] - p3[k]) * t * t
            + (-p0[k] + 3 * p1[k] - 3 * p2[k] + p3[k]) * t * t * t)
    end
    return out
end

-- The spline's own parameter is not uniform in distance (half speed on the end segments), so the
-- curve is sampled into a table of { length so far, segment, t }: the timetable is distance along the curve.
-- With speeds (5th coordinate) each entry also has the time at which the flight gets there ({ length, segment, t, time }).
local ARC_STEPS = 16
local function ArcTable(pts)
    local timed = #pts[1] >= 5
    local arc = { { 0, 1, 0, 0 } }
    local prev, len, time = Spline(pts, 1, 0), 0, 0
    for i = 1, #pts - 1 do
        for k = 1, ARC_STEPS do
            local p = Spline(pts, i, k / ARC_STEPS)
            local ds = math.sqrt((p[1] - prev[1]) ^ 2 + (p[2] - prev[2]) ^ 2 + (p[3] - prev[3]) ^ 2)
            len = len + ds
            if timed then time = time + ds / math.max(50, (p[5] + prev[5]) / 2) end
            arc[#arc + 1] = { len, i, k / ARC_STEPS, time }
            prev = p
        end
    end
    return arc, len, timed and time or nil
end

-- Point at distance `dist` along the curve
local function FlyPoint(f, dist)
    local arc = f.arc
    local i = f.ai
    while i < #arc - 1 and dist > arc[i + 1][1] do i = i + 1 end
    f.ai = i
    local a, b = arc[i], arc[i + 1]
    local span = b[1] - a[1]
    local u = span > 0 and math.max(0, math.min(1, (dist - a[1]) / span)) or 1
    local t0 = (a[2] == b[2]) and a[3] or 0          -- a is the end (t = 1) of the previous segment
    return Spline(f.pts, b[2], t0 + (b[3] - t0) * u)
end

-- Distance along the curve `t` seconds into the timed flight: constant acceleration for `ease` seconds,
-- cruise at the flight speed, constant deceleration for `easeout` seconds (f.secs = the whole flight).
local function FlyDist(f, t)
    local v, ei, eo = f.speed, f.ei, f.eo
    if t <= 0 then return 0 end
    if f.timed then                                   -- speeds per waypoint: look the time up in the arc table
        local arc = f.arc
        if t >= arc[#arc][4] then return f.total end
        local lo, hi = 1, #arc                        -- binary search: called for several times per frame
        while hi - lo > 1 do
            local mid = math.floor((lo + hi) / 2)
            if arc[mid][4] <= t then lo = mid else hi = mid end
        end
        local a, b = arc[lo], arc[hi]
        return a[1] + (b[1] - a[1]) * (t - a[4]) / math.max(1e-6, b[4] - a[4])
    end
    if t < ei then return v * t * t / (2 * ei) end
    local cruise = f.secs - ei - eo
    if t < ei + cruise then return v * (ei / 2 + t - ei) end
    local u = math.min(eo, t - ei - cruise)
    return math.min(f.total, v * (ei / 2 + cruise + u - (eo > 0 and u * u / (2 * eo) or 0)))
end

local function LogFlyDone(f, reason, now, restore)
    local secs = f.t0 and now - f.t0 or 0
    log(string.format("EV fly=done reason=%s phase=%s dist=%.0f/%.0f secs=%.2f frames=%d fps=%.1f maxdt=%.0fms err_max=%.0f err_rms=%.0f err_end=%.0f capped=%d resets=%d gravresets=%d lift=%.1f drift=%.0f clips=%d%s",
        reason, f.phase, f.t0 and FlyDist(f, secs) or 0, f.total, secs, f.runFrames,
        secs > 0 and f.runFrames / secs or 0, 1000 * f.maxDt, f.errMax, f.nErr > 0 and math.sqrt(f.errSq / f.nErr) or 0,
        f.errLast, f.capped, f.resets, f.gravResets, f.liftSecs or -1, f.drift, f.clips or 0, restore))
end

local function FlyStop(reason)
    local f = fly
    fly = nil
    local restore = FlightOff()
    if not f then return restore end
    local now = os.clock()
    if f.kind == "path" then
        if f.airhold then log("EV airhold=released reason=" .. reason .. restore)            -- not a flight: no fly=done
        elseif f.phase == "stay" then log("EV fly=released reason=" .. reason .. restore)     -- "done" was logged on arrival
        else LogFlyDone(f, reason, now, restore) end
    else
        log("EV flytest=" .. f.kind .. " stopped reason=" .. reason .. restore)
    end
    return restore
end

-- Send the velocity (Oblivion units per second) to the pawn the way the flight's `drive` says.
-- `at` is the point the timetable wants now (for the position-setting drives).
local SWEEP_OUT = {}
local function Drive(f, pawn, m, vx, vy, vz, at, now)
    local d = f.o.drive
    if d == "vel" then
        SetVelocity(m, vx * S, -vy * S, vz * S)
    elseif d == "launch" then
        pawn:LaunchCharacter({ X = vx * S, Y = -vy * S, Z = vz * S }, true, true)
    elseif d == "input" then
        local v = math.sqrt(vx * vx + vy * vy + vz * vz)
        if v > 1 then pawn:AddMovementInput({ X = vx / v, Y = -vy / v, Z = vz / v }, math.min(1, v / f.vmax), true) end
    elseif d == "obv" then
        m:StopMovementImmediately()
        if now - (f.lastObv or 0) >= 0.05 then
            f.lastObv = now
            Obv(string.format("player.setpos x %.1f", at[1])); Obv(string.format("player.setpos y %.1f", at[2])); Obv(string.format("player.setpos z %.1f", at[3]))
        end
    elseif d == "tp" then
        m:StopMovementImmediately()
        -- one shared 'Out' table: UE4SS (issue #1446) pins every out table it is given, so a new one per frame leaks
        pawn:K2_SetActorLocation({ X = at[1] * S, Y = -at[2] * S, Z = at[3] * S }, false, SWEEP_OUT, true)
    else
        error("unknown drive " .. tostring(d))
    end
end

-- Velocity that closes the gap to `target` in h seconds, capped at vmax (Oblivion units).
local function Pursuit(x, y, z, target, vmax, h)
    local vx, vy, vz = (target[1] - x) / h, (target[2] - y) / h, (target[3] - z) / h
    local v = math.sqrt(vx * vx + vy * vy + vz * vz)
    if v > vmax then
        local k = vmax / v
        vx, vy, vz = vx * k, vy * k, vz * k
        return vx, vy, vz, true
    end
    return vx, vy, vz, false
end

local function FlyFrame(now)
    local f = fly
    local pc, pawn = PC(), Pawn()
    if not (pc and pawn) then return FlyStop("nopawn") end
    local m = f.m
    if not (m and m:IsValid()) then m = GetMovement(pawn); f.m = m end
    local l = pawn:K2_GetActorLocation()
    local x, y, z = l.X / S, -l.Y / S, l.Z / S
    local dt = now - f.last                   -- also the step the engine takes in this frame
    f.last = now
    if f.px then
        -- something else moved the pawn (a teleport, a kill-Z or border reset): stop rather than fly on from there.
        -- What it moved since the last frame is the last frame's velocity times the last frame's step.
        local moved = Dist3(x, y, z, f.px, f.py, f.pz)
        if moved > 1500 + 1.5 * f.vmax * f.lastDt and f.o.drive ~= "obv" and f.o.drive ~= "tp" then
            return FlyStop(string.format("jumped moved=%.0f from=%.0f,%.0f,%.0f to=%.0f,%.0f,%.0f", moved, f.px, f.py, f.pz, x, y, z))
        end
    end
    f.px, f.py, f.pz = x, y, z
    f.lastDt = dt
    local h = math.max(f.o.tau, dt)
    if now - f.lastCheck >= 0.5 then
        f.lastCheck = now
        local lvl = LevelName(pc)
        if lvl ~= f.level then return FlyStop("level " .. f.level .. "->" .. lvl) end
        if m.GravityScale ~= 0 then m.GravityScale = 0.0; f.gravResets = f.gravResets + 1 end
    end
    if Mode(m) ~= f.wantMode then f.resets = f.resets + 1; m:SetMovementMode(f.wantMode, 0) end

    local at, target, vmax, wantYaw
    if f.phase == "run" and now - f.t0 >= f.secs then
        f.errLast = Dist3(x, y, z, f.pts[#f.pts][1], f.pts[#f.pts][2], f.pts[#f.pts][3])
        if f.o.stay ~= 1 then return FlyStop("arrived") end
        LogFlyDone(f, "arrived", now, "")
        f.phase = "stay"                     -- hover at the end point (camera held where the path ended) until flystop
    end
    if f.phase == "stay" then
        at = f.pts[#f.pts]; target = at; vmax = f.o.liftspeed; wantYaw = f.yaw
    elseif f.phase == "run" then
        local tr = now - f.t0
        local dist = FlyDist(f, tr)
        if f.o.fadeout > 0 and not f.fadingOut and tr >= f.secs - f.o.fadeout then
            f.fadingOut = true; FadeTo(1, f.o.fadeout, now)
        end
        f.runFrames = f.runFrames + 1
        if dt > f.maxDt then f.maxDt = dt end
        local was = FlyDist(f, tr - dt)                                   -- the timetable one frame ago
        local due = FlyPoint(f, was)                                      -- where the player should be before this frame's step
        local ai = f.ai
        at = FlyPoint(f, dist)
        local ahead = FlyPoint(f, math.min(f.total, dist + 300))          -- look toward a point a little ahead
        target = FlyPoint(f, math.min(f.total, FlyDist(f, tr - dt + h)))
        -- for the cinematic camera: the exact timetable point now, and a point about a second ahead to look at
        f.camAt = at
        f.camAhead = FlyPoint(f, math.min(f.total, f.o.camlook > 0 and FlyDist(f, tr + f.o.camlook) or dist + math.max(800, f.speed)))
        f.ai = ai
        local e = Dist3(x, y, z, due[1], due[2], due[3])
        f.errLast = e; f.errSq = f.errSq + e * e; f.nErr = f.nErr + 1
        if e > f.errMax then f.errMax = e end
        vmax = f.vmax
        wantYaw = math.deg(math.atan(ahead[1] - at[1], ahead[2] - at[2])) - 90 + f.lookOffset
        if f.o.prog > 0 and now - f.lastProg >= f.o.prog then
            log(string.format("EV fly=prog t=%.1f dist=%.0f err=%.0f pos=%.0f,%.0f,%.0f speed=%.0f mode=%d frames=%d cell=%s", now - f.t0, dist, e,
                x, y, z, Dist3(x, y, z, f.progX, f.progY, f.progZ) / (now - f.lastProg), Mode(m), f.runFrames, Grid(x, y)))
            f.lastProg, f.progX, f.progY, f.progZ = now, x, y, z
        end
    else
        at = f.pts[1]; target = at; vmax = f.o.liftspeed; wantYaw = f.holdYaw or f.yaw0
        local e = Dist3(x, y, z, at[1], at[2], at[3])
        if f.phase == "lift" then
            if e < 50 then
                f.phase = "hold"; f.tHold = now; f.liftSecs = now - f.tPhase
                log(string.format("EV " .. (f.airhold and "airhold" or "fly") .. "=lifted secs=%.1f from=%.0f,%.0f,%.0f", f.liftSecs, f.fromX, f.fromY, f.fromZ))
            elseif now - f.tPhase > f.liftTimeout then
                return FlyStop(string.format("lift-timeout left=%.0f pos=%.0f,%.0f,%.0f", e, x, y, z))
            end
        else
            if e > f.drift then f.drift = e end
            if now - f.tHold >= f.o.hold then
                f.phase = "run"; f.t0 = now; f.last = now
                f.lastProg, f.progX, f.progY, f.progZ = now, x, y, z
                if f.o.fadein > 0 then FadeTo(0, f.o.fadein, now) end
                log(string.format("EV fly=start lift=%.1f hold=%.1f drift=%.0f pos=%.0f,%.0f,%.0f", f.liftSecs or 0, now - f.tHold, f.drift, x, y, z))
            end
        end
    end
    local vx, vy, vz, capped = Pursuit(x, y, z, target, vmax, h)
    if capped and f.phase == "run" then f.capped = f.capped + 1 end
    Drive(f, pawn, m, vx, vy, vz, at, now)

    local delta = NormAngle(wantYaw - f.yaw)
    local maxTurn = f.o.turn * math.min(0.1, dt)
    if delta > maxTurn then delta = maxTurn elseif delta < -maxTurn then delta = -maxTurn end
    f.yaw = f.yaw + delta
    local wantPitch = (f.phase ~= "run" and f.holdPitch) or f.pitch
    local dp = wantPitch - f.curPitch
    local maxTilt = 0.5 * f.o.turn * math.min(0.1, dt)
    f.curPitch = f.curPitch + math.max(-maxTilt, math.min(maxTilt, dp))
    pc:SetControlRotation({ Pitch = f.curPitch, Yaw = f.yaw % 360, Roll = 0.0 })
end

local function NeedFrames(what)
    if driver.mode ~= "frame" then
        error(what .. " needs the per-frame driver of the current main.lua (driver=" .. tostring(driver.mode) ..
            "): run tools\\Install-UE4SS.ps1 -ModsOnly and restart the game")
    end
end

local function StartFlight(o, list, speed, pitch, off)
    NeedFrames("flight")
    if fly then FlyStop("replaced") end
    local pc, pawn = PC(), Pawn()
    if not (pc and pawn) then error("no player pawn") end
    local m = GetMovement(pawn)
    local arc, total, timedSecs = ArcTable(list)
    if timedSecs then                                   -- the flight speed for the top-speed cap: the fastest waypoint
        for _, p in ipairs(list) do speed = math.max(speed, p[5]) end
    end
    local l = pawn:K2_GetActorLocation()
    local now = os.clock()
    local yaw0 = math.deg(math.atan(list[2][1] - list[1][1], list[2][2] - list[1][2])) - 90 + off
    local f = { kind = "path", o = o, pts = list, arc = arc, total = total, ai = 1, speed = speed, pitch = pitch,
                lookOffset = off, yaw0 = yaw0, yaw = pc:GetControlRotation().Yaw, vmax = o.cap * speed,
                wantMode = o.mode == "falling" and MOVE_FALLING or MOVE_FLYING, level = LevelName(pc), m = m,
                phase = o.lift == 1 and "lift" or "run", tPhase = now, last = now, lastCheck = now, lastProg = now,
                fromX = l.X / S, fromY = -l.Y / S, fromZ = l.Z / S, drift = 0, runFrames = 0, maxDt = 0,
                errMax = 0, errSq = 0, nErr = 0, errLast = 0, capped = 0, resets = 0, gravResets = 0, lastDt = 0 }
    f.liftTimeout = 10 + 2 * Dist3(f.fromX, f.fromY, f.fromZ, list[1][1], list[1][2], list[1][3]) / o.liftspeed
    -- speeding up and slowing down: the distance covered while easing is half the flight speed times the time
    f.ei, f.eo = math.max(0, o.ease), math.max(0, o.easeout)
    if total / speed < (f.ei + f.eo) / 2 then f.ei, f.eo = 0, 0 end
    f.secs = total / speed + (f.ei + f.eo) / 2
    if timedSecs then f.timed, f.ei, f.eo, f.secs = true, 0, 0, timedSecs end
    if o.holdyaw >= 0 then f.holdYaw = o.holdyaw - 90 end                 -- Oblivion heading -> control yaw
    if o.holdpitch ~= 999 then f.holdPitch = o.holdpitch end
    local cp = pc:GetControlRotation().Pitch
    f.curPitch = (cp > 180) and cp - 360 or cp
    if f.phase == "run" then
        f.t0 = now; f.progX, f.progY, f.progZ = f.fromX, f.fromY, f.fromZ
        if o.fadein > 0 then FadeTo(0, o.fadein, now) end
        log(string.format("EV fly=start lift=0 hold=0 drift=0 pos=%.0f,%.0f,%.0f", f.fromX, f.fromY, f.fromZ))
    end
    FlightOn(pawn, m, o)
    if o.drive == "input" then
        pcall(function() m:SetOverrideMaxSpeed(f.vmax * S) end)
        pcall(function() m.MaxFlySpeed = f.vmax * S end)
        saved.override = true
    end
    fly = f
    return f
end

-- ---------------------------------------------------------------- flight tests (one verb each)
-- hover: flight mode, nothing else; does the pawn stay put, and does the mode stay MOVE_Flying?
-- lift:  hold a point dz above, steered every frame (the lift phase of a flight)
-- coast: set the velocity once; does it persist (or is it zeroed, capped, or braked)?
-- They sample once a second ("EV flytest=<name> ...") and end with a summary line; the pawn stays
-- in flight mode afterwards until "flytest off" (or "flystop").

local function TestFrame(now)
    local f = fly
    local pawn = Pawn()
    if not pawn then return FlyStop("nopawn") end
    local m = f.m
    if not (m and m:IsValid()) then m = GetMovement(pawn); f.m = m end
    local l = pawn:K2_GetActorLocation()
    local x, y, z = l.X / S, -l.Y / S, l.Z / S
    local mode = Mode(m)
    if mode ~= f.lastMode then f.modes[#f.modes + 1] = string.format("%d@%.1f", mode, now - f.t0); f.lastMode = mode end
    if f.kind == "lift" then
        local e = Dist3(x, y, z, f.target[1], f.target[2], f.target[3])
        if not f.reached and e < 50 then f.reached = now - f.t0 end
        if f.reached and e > f.drift then f.drift = e end
        if mode ~= MOVE_FLYING and f.o.mode == "flying" then m:SetMovementMode(MOVE_FLYING, 0); f.resets = f.resets + 1 end
        local vx, vy, vz = Pursuit(x, y, z, f.target, f.o.liftspeed, math.max(f.o.tau, now - (f.lastT or now)))
        f.lastT = now
        Drive(f, pawn, m, vx, vy, vz, f.target, now)
    end
    if now - f.lastSample >= 1 then
        local moved = Dist3(x, y, z, f.sx, f.sy, f.sz)
        local sp = moved / (now - f.lastSample)
        f.speeds[#f.speeds + 1] = string.format("%.0f", sp)
        log(string.format("EV flytest=%s t=%.1f pos=%.0f,%.0f,%.0f dz=%.0f speed=%.0f mode=%d", f.kind, now - f.t0, x, y, z, z - f.z0, sp, mode))
        f.lastSample, f.sx, f.sy, f.sz = now, x, y, z
    end
    if now - f.t0 >= f.secs then
        lastTest = string.format("%s secs=%.0f dz=%.0f moved=%.0f speeds=%s modes=%s grav=%.2f", f.kind, now - f.t0, z - f.z0,
            Dist3(x, y, z, f.x0, f.y0, f.z0), table.concat(f.speeds, ","), table.concat(f.modes, ","), m.GravityScale)
        if f.kind == "lift" then lastTest = lastTest .. string.format(" reached=%s drift=%.0f resets=%d", f.reached and string.format("%.1f", f.reached) or "no", f.drift, f.resets) end
        log("EV flytest=" .. f.kind .. " done " .. lastTest)
        fly = nil                                              -- stays in flight mode until "flytest off"
    end
end

local function StartTest(kind, secs, setup)
    NeedFrames("flytest " .. kind)
    if fly then FlyStop("replaced") end
    local pawn = Pawn()
    if not pawn then error("no player pawn") end
    local m = GetMovement(pawn)
    local o = FlyOptions("")
    FlightOn(pawn, m, o)
    local l = pawn:K2_GetActorLocation()
    local now = os.clock()
    local f = { kind = kind, o = o, m = m, t0 = now, secs = secs, lastSample = now, speeds = {}, modes = {}, resets = 0, drift = 0,
                x0 = l.X / S, y0 = -l.Y / S, z0 = l.Z / S, vmax = o.liftspeed }
    f.sx, f.sy, f.sz = f.x0, f.y0, f.z0
    if setup then
        local ok, err = pcall(setup, f, pawn, m)
        if not ok then error(tostring(err) .. FlightOff()) end
    end
    fly = f
    return string.format("%s for %.0f s from %.0f,%.0f,%.0f (%s)", kind, secs, f.x0, f.y0, f.z0, OptString(o))
end

local function Probe()
    local pawn = Pawn()
    if not pawn then error("no player pawn") end
    local m = GetMovement(pawn)
    local out = {}
    local function put(k, fn) local ok, v = pcall(fn); out[#out + 1] = k .. "=" .. (ok and tostring(v) or "?") end
    put("driver", function() return driver.mode end)
    put("pawn", function() return pawn:GetClass():GetFName():ToString() end)
    put("cmc", function() return m:GetClass():GetFName():ToString() end)
    put("pos", function() local l = pawn:K2_GetActorLocation(); return string.format("%.0f,%.0f,%.0f", l.X / S, -l.Y / S, l.Z / S) end)
    put("mode", function() return m.MovementMode end)
    put("custom", function() return m.CustomMovementMode end)
    put("grav", function() return string.format("%.2f", m.GravityScale) end)
    put("cheatfly", function() return m.bCheatFlying end)
    put("coll", function() return pawn:GetActorEnableCollision() end)
    put("v", function() local v = m.Velocity; return string.format("%.0f,%.0f,%.0f", v.X / S, -v.Y / S, v.Z / S) end)
    put("maxspeed", function() return string.format("%.0f", m:GetMaxSpeed() / S) end)
    put("maxfly", function() return string.format("%.0f", m.MaxFlySpeed / S) end)
    put("brakefly", function() return m.BrakingDecelerationFlying end)
    put("frictionfactor", function() return m.BrakingFrictionFactor end)
    put("fluidfriction", function() return m:GetPhysicsVolume().FluidFriction end)
    put("watervol", function() return m:GetPhysicsVolume().bWaterVolume end)
    put("locked", function() return m:IsMovementLocked() end)
    put("canfly", function() return pawn:CanFly() end)
    put("ghost", function() return pawn.OblivionActorStatePairingComponent:IsGhost() end)
    put("cell", function() return (Cell(pawn):gsub("%s", "_")) end)
    put("paired", function()                                  -- old engine's transform vs the UE pawn
        local t = pawn.TransformPairingComponent:GetLatestPairedTransform()
        local l = pawn:K2_GetActorLocation()
        local p = t.Translation
        return string.format("%.0f,%.0f,%.0f(off=%.0f)", p.X / S, -p.Y / S, p.Z / S, Dist3(p.X, p.Y, p.Z, l.X, l.Y, l.Z) / S)
    end)
    put("flying", function() return fly and (fly.kind .. "/" .. tostring(fly.phase)) or (saved and "mode-on" or "no") end)
    return table.concat(out, " ")
end

-- Objects the old engine has loaded (actors with a TES reference component), relative to the player:
-- if the old engine follows the player, they surround him; if not, they stay around where he was.
local function Refs(radius)
    local pawn = Pawn()
    if not pawn then error("no player pawn") end
    local l = pawn:K2_GetActorLocation()
    local n, near, nearest, sx, sy = 0, 0, math.huge, 0, 0
    local classes = {}
    for _, c in ipairs(FindAllOf("VTESObjectRefComponent") or {}) do
        local ok, d, cls, ox, oy = pcall(function()
            if not c:IsValid() then return nil end
            local o = c:GetOwner()
            if not (o and o:IsValid()) then return nil end
            local p = o:K2_GetActorLocation()
            return Dist3(p.X, p.Y, p.Z, l.X, l.Y, l.Z) / S, o:GetClass():GetFName():ToString(), p.X / S, -p.Y / S
        end)
        if ok and d and d > 1 then                             -- d = 0 is the player himself
            n = n + 1; sx = sx + ox; sy = sy + oy
            if d < nearest then nearest = d end
            if d < radius then near = near + 1; classes[cls] = (classes[cls] or 0) + 1 end
        end
    end
    local top = {}
    for k, v in pairs(classes) do top[#top + 1] = { k, v } end
    table.sort(top, function(a, b) return a[2] > b[2] end)
    local names = {}
    for i = 1, math.min(6, #top) do names[i] = top[i][1] .. ":" .. top[i][2] end
    return string.format("refs=%d within%d=%d nearest=%.0f centre=%.0f,%.0f player=%.0f,%.0f grid=%s near=%s", n, radius, near,
        n > 0 and nearest or -1, n > 0 and sx / n or 0, n > 0 and sy / n or 0, l.X / S, -l.Y / S, Grid(l.X / S, -l.Y / S), table.concat(names, ","))
end

-- "flytest doors [radius]": the door refs around the player (class name contains "Door"), nearest first, with the
-- heading from the player to each one: for placing a door scene so the Activate ray meets the door.
local function Doors(radius)
    local pawn = Pawn()
    if not pawn then error("no player pawn") end
    local l = pawn:K2_GetActorLocation()
    local px, py, pz = l.X / S, -l.Y / S, l.Z / S
    local found = {}
    for _, c in ipairs(FindAllOf("VTESObjectRefComponent") or {}) do
        pcall(function()
            if not c:IsValid() then return end
            local o = c:GetOwner()
            if not (o and o:IsValid()) then return end
            local cls = o:GetClass():GetFName():ToString()
            if not cls:find("Door") then return end
            local p = o:K2_GetActorLocation()
            local x, y, z = p.X / S, -p.Y / S, p.Z / S
            local d = Dist3(p.X, p.Y, p.Z, l.X, l.Y, l.Z) / S
            if d <= radius then
                local yaw = 0
                pcall(function() yaw = o:K2_GetActorRotation().Yaw end)
                found[#found + 1] = { d = d, s = string.format("%s d=%.0f at=%.0f,%.0f,%.0f heading=%.1f yaw=%.1f", cls, d, x, y, z,
                    (math.deg(math.atan(x - px, y - py)) + 360) % 360, (yaw + 90) % 360) }
            end
        end)
    end
    table.sort(found, function(a, b) return a.d < b.d end)
    local out = {}
    for i = 1, math.min(8, #found) do out[i] = found[i].s end
    return string.format("player=%.0f,%.0f,%.0f doors=%d: %s", px, py, pz, #found, table.concat(out, " | "))
end

-- ---------------------------------------------------------------- cinematic camera ("spectator")
-- The game's view target is its own VAltarCameraActor, which the camera system moves with the head
-- (head bob, idle sway, the flying pose) and with the velocity pursuit of a flight. For a steady,
-- repeatable picture the view target becomes our own CameraActor instead:
--   follow  the pawn's position plus the eye height, smoothed (no bob, no steps), looking along the
--           control rotation (smoothly);
--   path    during a timed flight: exactly the flight's timetable point (no pursuit error at all),
--           looking at a point about a second (flight option camlook) ahead, with smooth critically damped turning.
-- Post-process settings (brightness calibration, exposure) and the field of view are copied from the
-- game's camera once a second, so the image matches the game's own camera (luma 11.88 vs 11.95, SSIM
-- 0.97 at the canal, the rest is fireflies). The player's body is hidden while the camera is on
-- (spectator). If the camera actor goes away (level change, door) it is re-created once the player is
-- back. Teleports snap the camera instead of gliding across the map.

local cam = { on = false }
local CAM_POS_TAU, CAM_ROT_TAU = 0.15, 0.30          -- the path camera turns with the flight options camtau, camsmooth

-- Flight option clip=1: a line trace (visibility channel, simple collision) along the camera's move in this frame.
-- A hit means the camera entered something solid (a wall, a bridge pillar, a tree with collision): one
-- "EV fly=clip" per entry, with the flight time, the place (Oblivion units) and what was hit.
local CLIP_HIT, CLIP_IGNORE, CLIP_COLOR = {}, {}, { R = 1.0, G = 0.0, B = 0.0, A = 1.0 }
local function ClipTrace(f, now, pc)
    local p = f.clipFrom
    f.clipFrom = { cam.x, cam.y, cam.z }
    if not p then return end
    local hit = Kismet():LineTraceSingle(pc, { X = p[1], Y = p[2], Z = p[3] }, { X = cam.x, Y = cam.y, Z = cam.z },
        0, false, CLIP_IGNORE, 0, CLIP_HIT, true, CLIP_COLOR, CLIP_COLOR, 0.0)
    if hit and not f.inClip then
        f.clips = (f.clips or 0) + 1
        local what = "?"
        pcall(function() what = CLIP_HIT.Component:Get():GetFullName():match("[^%s]+$") end)
        log(string.format("EV fly=clip t=%.2f pos=%.0f,%.0f,%.0f what=%s", now - f.t0, cam.x / S, -cam.y / S, cam.z / S, what))
    end
    f.inClip = hit and true or false
end

-- Unity-style SmoothDamp: critically damped, continuous velocity
local function SmoothDamp(cur, target, vel, tau, dt)
    if tau <= 0 or dt <= 0 then return target, 0 end
    local omega = 2 / tau
    local x = omega * dt
    local e = 1 / (1 + x + 0.48 * x * x + 0.235 * x * x * x)
    local change = cur - target
    local temp = (vel + omega * change) * dt
    vel = (vel - omega * temp) * e
    return target + (change + temp) * e, vel
end

local function GameCamera() return Obj("VAltarCameraActor") end

-- Moves the spectator camera actor. K2_TeleportTo has no 'Out' parameter: UE4SS builds before 3.0.1-1181 never
-- release the registry reference they take for each 'Out' table (upstream issue #1446, fixed by PR #1452), so calling
-- K2_SetActorLocationAndRotation(..., {}, true) every frame pinned one FHitResult table per frame (~1 GB/h).
-- For a CameraActor both do the same: its root is a plain SceneComponent, so UE 5.3 AActor::TeleportTo sets the world
-- location and rotation directly with ETeleportType::TeleportPhysics and no sweep (Actor.cpp), as the old call did.
-- BenchCamMove = "setloc" (set with the lua verb) brings the old call back, for leak tests.
local function MoveCam(a, loc, rot)
    if BenchCamMove == "setloc" then a:K2_SetActorLocationAndRotation(loc, rot, false, {}, true)
    else a:K2_TeleportTo(loc, rot) end
end

local function CamCopyLook(a)
    local g = GameCamera()
    if not (g and a and a:IsValid()) then return end
    pcall(function()
        local gc, c = g.CameraComponent, a.CameraComponent
        c.PostProcessSettings = gc.PostProcessSettings
        c.PostProcessBlendWeight = gc.PostProcessBlendWeight
        c.FieldOfView = gc.FieldOfView
    end)
end

local function CamSpawn(pc)
    local gs = StaticFindObject("/Script/Engine.Default__GameplayStatics")
    local cm = pc.PlayerCameraManager
    local loc, rot = cm:GetCameraLocation(), cm:GetCameraRotation()
    local xf = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = loc.X, Y = loc.Y, Z = loc.Z }, Scale3D = { X = 1, Y = 1, Z = 1 } }
    -- UE 5.3 signatures: (context, class, transform, collision handling, owner, scale method) / (actor, transform, scale method)
    local a = gs:BeginDeferredActorSpawnFromClass(pc, StaticFindObject("/Script/Engine.CameraActor"), xf, 1, nil, 1)
    a = gs:FinishSpawningActor(a, xf, 1)
    MoveCam(a, loc, rot)
    CamCopyLook(a)
    return a
end

local function CamHidePawn(pawn, hide)
    if not pawn then return end
    pcall(function() pawn:SetActorHiddenInGame(hide) end)
    cam.hiddenPawn = hide and pawn or nil
end

local function CamOff()
    if not cam.on then return "cam=off" end
    cam.on = false
    local pc = PC()
    local g = GameCamera()
    if pc and g then pcall(function() pc:SetViewTargetWithBlend(g, 0, 0, 0, false) end) end
    if cam.hiddenPawn and cam.hiddenPawn:IsValid() then CamHidePawn(cam.hiddenPawn, false) end
    cam.hiddenPawn = nil
    if cam.actor and cam.actor:IsValid() then pcall(function() cam.actor:K2_DestroyActor() end) end
    cam.actor = nil
    return "cam=off"
end

local function CamFrame(now)
    local pc, pawn = PC(), Pawn()
    if not (pc and pawn) then return end
    local lcd = Obj("VLevelChangeData")
    if lcd and (lcd.bIsFastTravelling or lcd.bIsSeamlessTravelling) then cam.snap = true; return end
    local dt = math.min(0.1, now - (cam.last or now)); cam.last = now
    local a = cam.actor
    local level = LevelName(pc)
    if not (a and a:IsValid()) or cam.level ~= level then
        if now < (cam.retryAt or 0) then return end
        cam.retryAt = now + 1
        if a and a:IsValid() then pcall(function() a:K2_DestroyActor() end) end
        local ok, res = pcall(CamSpawn, pc)
        if not ok then log("WARN cam spawn: " .. (tostring(res):gsub("[\r\n].*", ""))); return end
        a = res; cam.actor = a; cam.level = level; cam.snap = true; cam.spawns = (cam.spawns or 0) + 1
    end
    local cm = pc.PlayerCameraManager
    if cm.ViewTarget.Target ~= a then pc:SetViewTargetWithBlend(a, 0, 0, 0, false); cam.retargets = (cam.retargets or 0) + 1 end
    if cam.hide and cam.hiddenPawn ~= pawn then CamHidePawn(pawn, true) end
    if now - (cam.lastCopy or 0) >= 1 then
        cam.lastCopy = now; CamCopyLook(a)
        -- a level change brings a fresh, visible HUD and the software cursor back: hide them again
        if cam.noHud and HudShown() then SetHud(false); cam.hudToggles = (cam.hudToggles or 0) + 1 end
        if cam.noHud then
            pcall(function() Obj("VAltarCursor"):SetRenderOpacity(0.0) end)
            pcall(function() Obj("AltarGraphicsSettingsDebugInfo"):SetRenderOpacity(0.0) end)
        end
    end

    -- where to be and where to look (UE units / degrees)
    local tx, ty, tz, tyaw, tpitch, posTau, rotTau, stages
    local f = fly
    if f and f.kind == "path" and (f.phase == "run" or f.phase == "stay") and f.camAt then
        tx, ty, tz = f.camAt[1] * S, -f.camAt[2] * S, f.camAt[3] * S + ((tonumber(f.o.cameye) or 0) > 0 and f.o.cameye * S or cam.eyeZ)
        local ah = f.camAhead
        tyaw = math.deg(math.atan(ah[1] - f.camAt[1], ah[2] - f.camAt[2])) - 90 + (f.lookOffset or 0)
        tpitch = f.camAt[4] or f.pitch
        posTau, rotTau, stages = 0, f.o.camtau, math.max(1, math.floor(f.o.camsmooth))
    else
        local l = pawn:K2_GetActorLocation()
        tx, ty, tz = l.X, l.Y, l.Z + cam.eyeZ
        local r = pc:GetControlRotation()
        tyaw, tpitch = r.Yaw, (r.Pitch > 180) and r.Pitch - 360 or r.Pitch
        posTau, rotTau, stages = CAM_POS_TAU, CAM_ROT_TAU, 1
    end
    if cam.snap or not cam.x or Dist3(tx, ty, tz, cam.x, cam.y, cam.z) > 3000 then
        cam.x, cam.y, cam.z, cam.yaw, cam.pitch = tx, ty, tz, tyaw, tpitch
        cam.vx, cam.vy, cam.vz, cam.vyaw, cam.vpitch = 0, 0, 0, 0, 0
        cam.stage = {}
        cam.snap = false; cam.snaps = (cam.snaps or 0) + 1
    else
        cam.x, cam.vx = SmoothDamp(cam.x, tx, cam.vx, posTau, dt)
        cam.y, cam.vy = SmoothDamp(cam.y, ty, cam.vy, posTau, dt)
        cam.z, cam.vz = SmoothDamp(cam.z, tz, cam.vz, posTau, dt)
        -- camsmooth: the target first passes through stages-1 smoothing stages (each tau/stages), so the turn rate
        -- itself changes smoothly too (no kink when the path starts or stops bending)
        local tau = rotTau / stages
        cam.stage = cam.stage or {}
        for k = 1, stages - 1 do
            local st = cam.stage[k]
            if not st then st = { y = cam.yaw, vy = 0, p = cam.pitch, vp = 0 }; cam.stage[k] = st end
            st.y, st.vy = SmoothDamp(st.y, st.y + NormAngle(tyaw - st.y), st.vy, tau, dt)
            st.p, st.vp = SmoothDamp(st.p, tpitch, st.vp, tau, dt)
            tyaw, tpitch = st.y, st.p
        end
        for k = #cam.stage, stages, -1 do cam.stage[k] = nil end          -- fewer stages than last frame
        cam.yaw, cam.vyaw = SmoothDamp(cam.yaw, cam.yaw + NormAngle(tyaw - cam.yaw), cam.vyaw, tau, dt)
        cam.pitch, cam.vpitch = SmoothDamp(cam.pitch, tpitch, cam.vpitch, tau, dt)
    end
    if f and f.kind == "path" and f.phase == "run" and f.o.clip == 1 then
        local okC, errC = pcall(ClipTrace, f, now, pc)
        if not okC then f.o.clip = 0; log("WARN clip trace off: " .. (tostring(errC):gsub("[\r\n].*", ""))) end
    end
    MoveCam(a, { X = cam.x, Y = cam.y, Z = cam.z }, { Pitch = cam.pitch, Yaw = cam.yaw % 360, Roll = 0.0 })
end

local function CamOn(hide)
    local pc, pawn = PC(), Pawn()
    if not (pc and pawn) then error("no player pawn") end
    NeedFrames("cam")
    if not cam.on then
        -- eye height above the pawn's origin, from the game's camera while it is still the view target
        local ok, dz = pcall(function() return pc.PlayerCameraManager:GetCameraLocation().Z - pawn:K2_GetActorLocation().Z end)
        cam.eyeZ = (ok and dz and dz > 20 and dz < 150) and dz or 75
    end
    cam.on, cam.hide, cam.snap = true, hide, true
    return string.format("cam=on eyeZ=%.0f hide=%s", cam.eyeZ, tostring(hide))
end

-- ---------------------------------------------------------------- verbs

local V = {}

-- "fly <drive> <speed> <pitch> <lookOffset> [option=value ...] x,y,z x,y,z ...": lift to the first point,
-- hover `hold` s, then fly the path on its timetable. Events: fly=lifted, fly=start, fly=prog, fly=done.
-- drive "default" uses the flyopt setting; "ue" is the old name of "tp".
function V.fly(rest)
    local opts = {}
    for kv in rest:gmatch("[%a_]+=%S+") do opts[#opts + 1] = kv end
    local optText = table.concat(opts, " ")
    local plain = rest:gsub("%s*[%a_]+=%S+", "")
    local drive, speed, pitch, off, pts = plain:match("^%s*(%S+)%s+(%S+)%s+(%S+)%s+(%S+)%s+(.+)$")
    local list = {}
    for tok in (pts or ""):gmatch("%S+") do
        local p = {}
        for v in tok:gmatch("[^,]+") do p[#p + 1] = tonumber(v) end
        if #p < 3 or #p > 5 or #p ~= #(list[1] or p) then error("bad waypoint " .. tok .. " (x,y,z[,pitch[,speed]], all alike)") end
        list[#list + 1] = p
    end
    if #list < 2 or not tonumber(speed) then error("usage: fly <drive|default> <speed> <pitch> <lookOffset> [k=v ...] x,y,z[,pitch] x,y,z[,pitch] ...") end
    if drive ~= "default" then optText = "drive=" .. drive .. " " .. optText end
    local o = FlyOptions(optText)
    local f = StartFlight(o, list, tonumber(speed), tonumber(pitch), tonumber(off))
    return string.format("flying %.0f units secs=%.1f lift=%d hold=%.1f drive=%s mode=%s clock=%.3f", f.total, f.secs, o.lift, o.hold, o.drive, o.mode, os.clock())
end

-- "fade <0..1> [secs]": black over the view (1) or clear (0), over `secs` (default 0); "fade off": clear at once.
-- A flight with fadein=... clears it by itself when its timed part starts.
function V.fade(rest)
    local amount, secs = rest:match("^%s*(%S+)%s*(%S*)")
    if amount == "off" then
        fade = nil
        pcall(function() PC().PlayerCameraManager:StopCameraFade() end)
        return "fade=off"
    end
    if not tonumber(amount) then error("usage: fade <0..1> [secs] | fade off") end
    FadeTo(math.max(0, math.min(1, tonumber(amount))), tonumber(secs) or 0)
    return string.format("fade=%s secs=%s", amount, tonumber(secs) or 0)
end

-- "flystop": end a flight or flight test and restore normal movement (also when only flight mode is left on)
function V.flystop() return "ok" .. FlyStop("stopped") end

-- "flyopt": show the flight options; "flyopt k=v ...": change them (until reload); "flyopt reset"
function V.flyopt(rest)
    if rest == "reset" then flyOpts = {} elseif rest ~= "" then ParseOpts(rest, flyOpts) end
    return OptString(FlyOptions(""))
end

-- "flytest probe" | "hover [secs]" | "lift [dz] [secs]" | "coast [speed] [heading] [secs]"
-- | "line [dist] [speed] [heading] [pitch]" | "refs [radius]" | "off" | "result"   (distances in units, speeds in units/s)
function V.flytest(rest)
    local verb, a, b, c, d = rest:match("^(%S*)%s*(%S*)%s*(%S*)%s*(%S*)%s*(%S*)")
    if verb == "probe" or verb == "" then return Probe() end
    if verb == "result" then return lastTest end
    if verb == "refs" then return Refs(tonumber(a) or 6000) end
    if verb == "doors" then return Doors(tonumber(a) or 1500) end
    if verb == "off" then return "off" .. FlyStop("stopped") end
    if verb == "hover" then
        return StartTest("hover", tonumber(a) or 10)
    elseif verb == "lift" then
        local dz = tonumber(a) or 1500
        return StartTest("lift", tonumber(b) or 15, function(f) f.target = { f.x0, f.y0, f.z0 + dz } end)
    elseif verb == "coast" then
        local speed, heading = tonumber(a) or 2000, tonumber(b) or 90
        return StartTest("coast", tonumber(c) or 12, function(f, pawn, m)
            local h = math.rad(heading)
            SetVelocity(m, math.sin(h) * speed * S, -math.cos(h) * speed * S, 0)    -- heading: 0 = north (+y), 90 = east (+x)
        end)
    elseif verb == "line" then
        local pc, pawn = PC(), Pawn()
        if not (pc and pawn) then error("no player pawn") end
        local dist, speed = tonumber(a) or 20000, tonumber(b) or 2000
        local heading = tonumber(c) or ((pc:GetControlRotation().Yaw + 90) % 360)
        local pitch = tonumber(d) or -5
        local l = pawn:K2_GetActorLocation()
        local x, y, z = l.X / S, -l.Y / S, l.Z / S
        local h = math.rad(heading)
        return V.fly(string.format("default %s %s 0 lift=0 %.1f,%.1f,%.1f %.1f,%.1f,%.1f %.1f,%.1f,%.1f", speed, pitch,
            x, y, z, x + math.sin(h) * dist / 2, y + math.cos(h) * dist / 2, z, x + math.sin(h) * dist, y + math.cos(h) * dist, z))
    end
    error("usage: flytest probe|hover [secs]|lift [dz] [secs]|coast [speed] [heading] [secs]|line [dist] [speed] [heading] [pitch]|refs [radius]|doors [radius]|off|result")
end

-- "cam on [show]" | "cam off" | "cam snap" | "cam": the cinematic camera (see above). "show" keeps the player's body visible.
function V.cam(rest)
    local verb, arg = rest:match("^(%S*)%s*(%S*)")
    if verb == "on" then return CamOn(arg ~= "show") end
    if verb == "off" then return CamOff() end
    if verb == "snap" then cam.snap = true; return "snap" end
    local pc = PC()
    local vt = "?"
    pcall(function() vt = pc.PlayerCameraManager.ViewTarget.Target:GetClass():GetFName():ToString() end)
    return string.format("cam=%s viewtarget=%s spawns=%d retargets=%d snaps=%d hudhides=%d errors=%d eyeZ=%.0f pos=%.0f,%.0f,%.0f heading=%.1f pitch=%.1f",
        cam.on and "on" or "off", vt, cam.spawns or 0, cam.retargets or 0, cam.snaps or 0, cam.hudToggles or 0, cam.errors or 0, cam.eyeZ or 0,
        (cam.x or 0) / S, -(cam.y or 0) / S, (cam.z or 0) / S, ((cam.yaw or 0) + 90) % 360, cam.pitch or 0)
end

-- "spectate on [show]" | "spectate off": the benchmark's showcase mode. HUD hidden, subtitles off, head
-- bob off, cinematic camera on (player body hidden unless "show"). The game options it changes are
-- remembered and put back by "spectate off". (God mode and AI detection are the runner's: `god`, `obv tdetect`.)
local spectSaved = nil
local SPECT_CVARS = { ["Altar.Subtitle.Visibility"] = 0, ["Altar.Player.HeadBobbing"] = 0 }
function V.spectate(rest)
    local verb, arg = rest:match("^(%S*)%s*(%S*)")
    local out = {}
    if verb == "on" then
        if not spectSaved then
            spectSaved = {}
            for k in pairs(SPECT_CVARS) do spectSaved[k] = Cvar(k) end
        end
        for k, v in pairs(SPECT_CVARS) do pcall(Exec, k .. " " .. v) end
        out[#out + 1] = V.hud("0")
        out[#out + 1] = CamOn(arg ~= "show")
        cam.noHud = true                      -- the camera keeps the HUD hidden across level changes
    elseif verb == "off" then
        out[#out + 1] = CamOff()
        cam.noHud = false
        out[#out + 1] = V.hud("1")
        if spectSaved then
            for k in pairs(SPECT_CVARS) do if spectSaved[k] then pcall(Exec, string.format("%s %g", k, spectSaved[k])) end end
            spectSaved = nil
        end
    elseif verb == "" then
        return "spectate=" .. (spectSaved and "on" or "off") .. " " .. V.cam("")
    else
        error("usage: spectate on [show] | off")
    end
    return table.concat(out, " ")
end

-- "pin <cvar> <value>" | "pin clear" | "pin": keep console variables set. The game changes some settings by
-- itself (it turns FSR off whenever its window loses the focus, and once did so in the middle of a
-- flight). Pinned ones are checked every half second from the frame loop and set again when they
-- differ, each time counted and logged ("EV pin=<name> was=<v>"); "pin" lists them with their counts.
local pins, pinAt = {}, 0
local function PinTick(now)
    if now < pinAt or next(pins) == nil then return end
    pinAt = now + 0.5
    for name, p in pairs(pins) do
        local v = Cvar(name)
        if v and math.abs(v - p.value) > 0.01 then
            pcall(Exec, name .. " " .. p.text)
            p.fixes = p.fixes + 1
            log(string.format("EV pin=%s was=%s set=%s", name, tostring(v), p.text))
        end
    end
end
function V.pin(rest)
    if rest == "clear" then pins = {}; return "pins=none" end
    local name, value = rest:match("^(%S+)%s+(%S+)$")
    if name then
        if not tonumber(value) then error("pin: numeric value expected") end
        pins[name] = { value = tonumber(value), text = value, fixes = 0 }
        return "pinned " .. name .. "=" .. value
    end
    local out = {}
    for n, p in pairs(pins) do out[#out + 1] = string.format("%s=%s:%d", n, p.text, p.fixes) end
    table.sort(out)
    return "pins=" .. (#out > 0 and table.concat(out, ",") or "none")
end

-- "mainmenu": leave the game for the main menu (the pause menu's "Main Menu" without its question).
-- Flights, walks and the camera are stopped first. From the menu a new game (`coc`) or Continue works as after a start.
function V.mainmenu()
    -- a flight hovering at its end ("stay", view faded out) keeps hovering: dropping the player now would be a fall
    -- and a splash during the fade to the menu; the level change ends it
    if (fly and not (fly.kind == "path" and fly.phase == "stay")) or (saved and not fly) then FlyStop("mainmenu") end
    if walk then WalkStop("mainmenu") end
    CamOff()
    pins = {}                                 -- the menu has its own settings life; the next run pins again
    Obj("VLevelChangeData"):GoToMainMenuAfterFadeToBlack()
    return "sent"
end

-- "god": is the old engine's god mode on? "god toggle": `tgm` (no damage from lava, falls, creatures).
-- Not the cheat manager's ObvGodMode: that one also raises the character's skills.
function V.god(rest)
    if rest == "toggle" then Obv("tgm"); return "toggled" end
    return "god=" .. tostring(PC().CheatManager:IsInGodMode())
end

function V.quit()
    local pc = PC()
    Kismet():QuitGame(pc, pc, 0, false)
    return "quitting"
end

-- "cond [radius]": the conditions the scene actually has, for repeatable runs (2026-10-20, benchrepro):
-- hour = the time of day (ATMSubsystem), wx = the weather presets the atmosphere tool has active with their blend
-- alpha (ATMSubsystem.ActiveEvents: "Clear:1.00", during a change "Rain:1.00,Clear:0.30"; fw switches within a frame),
-- npcs = characters within radius (game units, default 3000) of the player, near = the three nearest as name@x,y
-- ("cond 0": no character scan, only hour and weather). One-off reads, no hooks.
function V.cond(rest)
    local radius = (tonumber(rest) or 3000) * S
    local out = {}
    local atm = Obj("ATMSubsystem")
    if atm then
        pcall(function() out[#out + 1] = string.format("hour=%.3f", atm:GetTime()) end)
        local wx = {}
        pcall(function()
            atm.ActiveEvents:ForEach(function(_, v)
                local inst = v:get()
                if inst and inst:IsValid() then
                    local n, a = "?", -1
                    pcall(function() n = inst.PresetBase:GetFName():ToString():gsub("^ATMP_Weather_", "") end)
                    pcall(function() a = inst.CachedAlpha end)
                    wx[#wx + 1] = string.format("%s:%.2f", n, a)
                end
            end)
        end)
        out[#out + 1] = "wx=" .. (#wx > 0 and table.concat(wx, ",") or "none")
    end
    local pawn = radius > 0 and Pawn() or nil
    if pawn then
        local p = pawn:K2_GetActorLocation()
        local near = {}
        for _, ch in ipairs(FindAllOf("Character") or {}) do
            if ch:IsValid() and ch:GetAddress() ~= pawn:GetAddress() then
                pcall(function()
                    local l = ch:K2_GetActorLocation()
                    local d = Dist3(l.X, l.Y, l.Z, p.X, p.Y, p.Z)
                    if d < radius then near[#near + 1] = { d = d, n = ch:GetFName():ToString():gsub("_C_%d+$", ""), x = l.X / S, y = -l.Y / S } end
                end)
            end
        end
        table.sort(near, function(a, b) return a.d < b.d end)
        out[#out + 1] = "npcs=" .. #near
        local t = {}
        for i = 1, math.min(3, #near) do t[#t + 1] = string.format("%s@%.0f,%.0f", near[i].n, near[i].x, near[i].y) end
        if #t > 0 then out[#out + 1] = "near=" .. table.concat(t, ";") end
    end
    return table.concat(out, " ")
end

local pingFrames, pingAt = 0, os.clock()
function V.ping()
    local now = os.clock()
    local fps = (driver.frames - pingFrames) / math.max(0.001, now - pingAt)
    pingFrames, pingAt = driver.frames, now
    return string.format("pong clock=%.3f driver=%s frames=%d loops_per_s=%.1f", now, tostring(driver.mode), driver.frames or 0, fps)
end
function V.state() return State(false) end
function V.where() return State(true) end
function V.exec(rest) Exec(rest); return "sent" end
function V.obv(rest) Obv(rest); return "sent" end

-- "cvar <name> <value>": set through the console, return the value read back
function V.cvar(rest)
    local name, value = rest:match("^(%S+)%s+(.+)$")
    if not name then error("usage: cvar <name> <value>") end
    Exec(name .. " " .. value)
    return name .. "=" .. tostring(Cvar(name))
end
function V.get(rest) return rest .. "=" .. tostring(Cvar(rest)) end

-- "tp <world|cell> <name> <x> <y> <z> <heading>": one-step teleport (PositionWorld / PositionCell)
-- "tp here <x> <y> <z>": setpos within the current cell
function V.tp(rest)
    local hx, hy, hz = rest:match("^here%s+(%S+)%s+(%S+)%s+(%S+)$")
    local kind, name, x, y, z, h = rest:match("^(%S+)%s+(%S+)%s+(%S+)%s+(%S+)%s+(%S+)%s*(%S*)$")
    if fly or saved then FlyStop("teleport") end              -- never teleport in flight mode
    if hx then
        Obv("player.setpos x " .. hx); Obv("player.setpos y " .. hy); Obv("player.setpos z " .. hz)
    elseif kind == "world" then
        Obv(string.format("player.positionworld %s %s %s %s %s", x, y, z, h ~= "" and h or "0", name))
    elseif kind == "cell" then
        Obv(string.format("player.positioncell %s %s %s %s %s", x, y, z, h ~= "" and h or "0", name))
    else
        error("usage: tp world|cell <name> <x> <y> <z> <heading> | tp here <x> <y> <z>")
    end
    lastPos = nil
    return "sent"
end

-- "airhold <x> <y> <z> <heading> [pitch]" before a teleport into the air: as soon as the player shows up within 3000
-- units (horizontally) of x,y, flight mode holds it at x,y,z facing `heading` (the player never falls into water,
-- no splash or underwater sound); a following "fly" or "tp" replaces the hold. "airhold off" cancels a pending one.
local airhold = nil
function V.airhold(rest)
    if rest == "off" then airhold = nil; return "airhold=off" end
    local x, y, z, h, p = rest:match("^(%S+)%s+(%S+)%s+(%S+)%s+(%S+)%s*(%S*)$")
    if not h then error("usage: airhold <x> <y> <z> <heading> [pitch] | airhold off") end
    airhold = { x = tonumber(x), y = tonumber(y), z = tonumber(z), h = tonumber(h), p = tonumber(p) or 0, t = os.clock() }
    return "airhold=pending"
end

local function AirholdFrame(now)
    local a = airhold
    if now - a.t > 120 then airhold = nil; log("EV airhold=expired"); return end
    local pc, pawn = PC(), Pawn()
    if not (pc and pawn) or fly then return end
    local lcd = Obj("VLevelChangeData")
    if lcd and (lcd.bIsFastTravelling or lcd.bIsSeamlessTravelling) then return end
    local l = pawn:K2_GetActorLocation()
    local x, y, z = l.X / S, -l.Y / S, l.Z / S
    if math.sqrt((x - a.x) ^ 2 + (y - a.y) ^ 2) > 3000 then return end
    airhold = nil
    local r = math.rad(a.h)
    StartFlight(FlyOptions("hold=100000 prog=0"), { { a.x, a.y, a.z }, { a.x + 1000 * math.sin(r), a.y + 1000 * math.cos(r), a.z } }, 600, a.p, 0)
    fly.airhold = true
    log(string.format("EV airhold=caught at=%.0f,%.0f,%.0f", x, y, z))
end

-- "look <heading> <pitch>"
function V.look(rest)
    local h, p = rest:match("^(%S+)%s*(%S*)$")
    PC():SetControlRotation({ Pitch = tonumber(p) or 0.0, Yaw = (tonumber(h) - 90) % 360, Roll = 0.0 })
    return "ok"
end

-- "aim [maxdist] [pattern] [front]" (2026-10-20, faces scenario): turn the camera to the head of the nearest other
-- character within maxdist game units (default 400) of the camera whose name contains `pattern` (e.g. Kewan for
-- BP_Kewan_C_...), e.g. one a scene spawned with placeatme (which places by the old engine's heading, not the
-- camera's). Head = the mesh's "head" socket when it is within 2 m above the actor, else the actor's location + 70 cm.
-- "front": instead move the player to 150 cm in front of that character (old engine setpos), so that a following
-- "aim" sees its face. One-off call, no hooks.
function V.aim(rest)
    local maxd, pattern, front = 400, nil, false
    for tok in rest:gmatch("%S+") do
        if tonumber(tok) then maxd = tonumber(tok) elseif tok == "front" then front = true else pattern = tok end
    end
    maxd = maxd * S
    local pc, pawn = PC(), Pawn()
    if not pc then error("no player controller") end
    local c = pc.PlayerCameraManager:GetCameraLocation()
    local best, bd = nil, maxd
    for _, ch in ipairs(FindAllOf("Character") or {}) do
        if ch:IsValid() and not (pawn and ch:GetAddress() == pawn:GetAddress()) then
            local ok, d = pcall(function()
                if pattern and not ch:GetFName():ToString():find(pattern, 1, true) then return nil end
                local l = ch:K2_GetActorLocation()
                return Dist3(l.X, l.Y, l.Z, c.X, c.Y, c.Z)
            end)
            if ok and d and d < bd then best, bd = ch, d end
        end
    end
    if not best then return "aim=none" end
    local l = best:K2_GetActorLocation()
    if front then
        local f = best:GetActorForwardVector()
        local x, y = l.X + 150 * f.X, l.Y + 150 * f.Y
        Obv("player.setpos x " .. string.format("%.1f", x / S)); Obv("player.setpos y " .. string.format("%.1f", -y / S))
        Obv("player.setpos z " .. string.format("%.1f", l.Z / S))
        lastPos = nil
        return string.format("aim=%s front at=%.0f,%.0f,%.0f", best:GetFName():ToString(), x / S, -y / S, l.Z / S)
    end
    local hx, hy, hz, src = l.X, l.Y, l.Z + 70, "actor"
    pcall(function()
        local m = best.Mesh
        if m and m:IsValid() then
            local s = m:GetSocketLocation(FName("head"))
            if s.Z > l.Z and s.Z < l.Z + 200 then hx, hy, hz, src = s.X, s.Y, s.Z, "head" end
        end
    end)
    local dx, dy, dz = hx - c.X, hy - c.Y, hz - c.Z
    local yaw = math.deg(math.atan(dy, dx))
    local pitch = math.deg(math.atan(dz, math.sqrt(dx * dx + dy * dy)))
    pc:SetControlRotation({ Pitch = pitch, Yaw = yaw % 360, Roll = 0.0 })
    return string.format("aim=%s dist=%.0f heading=%.1f pitch=%.1f at=%s", best:GetFName():ToString(), bd / S, (yaw + 90) % 360, pitch, src)
end

-- The game's gameplay input mapping (2026-10-06, drift): in a long hidden session the game was found with only
-- IMC_UI_Generic_BasicNavigation applied (every IMC_Game_* context removed) and no menu open, after ~15 min of door
-- and teleport scenes; injected game actions (activate, move) then do nothing. If no menu is visible and
-- IMC_Game_Actions is not applied, let the controller switch back to its game mappings (its own function).
local inputFixes = 0
local function EnsureGameInput()
    local sub = Obj("EnhancedInputLocalPlayerSubsystem")
    local pc = PC()
    if not (sub and pc) then return "" end
    local ui = Obj("VAltarUISubsystem")
    local menu = false
    if ui then pcall(function() menu = ui:IsMenuVisible() end) end
    if menu then return "" end
    local imc = cache.__imcGameActions
    if not (imc and imc:IsValid()) then
        imc = nil
        for _, m in ipairs(FindAllOf("InputMappingContext") or {}) do
            if m:IsValid() and m:GetFName():ToString() == "IMC_Game_Actions" then imc = m; break end
        end
        cache.__imcGameActions = imc
    end
    if not imc then return "" end
    local ok, has = pcall(function() return sub:HasMappingContext(imc, {}) end)
    local out = ""
    if ok and not has then
        pc:SwitchToGameInputMappings()
        inputFixes = inputFixes + 1
        out = " inputfix=" .. inputFixes
    end
    -- the controller was also left ignoring move input (IgnoreMoveInput counter), which stops injected walking
    local okm, ignored = pcall(function() return pc:IsMoveInputIgnored() end)
    if okm and ignored then pcall(function() pc:ResetIgnoreMoveInput() end); out = out .. " moveinputreset=1" end
    return out
end

-- "prep": first person, no idle (vanity) camera
function V.prep()
    local pc = PC()
    local out = {}
    local function try(k, fn) local ok, e = pcall(fn); out[#out + 1] = k .. (ok and "" or ("!" .. tostring(e):gsub("\n.*", ""))) end
    try("exitvanity", function() pc:ExitVanityCamera() end)
    try("stopvanitytimer", function() pc.PlayerCameraManager:StopVanityCameraTimer() end)
    try("pov", function() if pc.POV ~= 0 then pc:SwitchPOV(0, true) end end)
    -- every scene starts with the game input mapping (a game left in its UI mapping measured differently, 2026-10-06)
    local fix = ""
    try("input", function() fix = EnsureGameInput() end)
    return table.concat(out, " ") .. fix
end

-- "hud 0|1": the gameplay HUD and the cursor (see SetHud); fades, loading screens and menus stay
function V.hud(rest)
    if not SetHud(rest == "1") then return "hud=nohud" end
    return "hud=" .. tostring(HudShown())
end

-- "freeze 0|1": the game's own menu freeze (world and old engine stop, rendering continues)
function V.freeze(rest)
    local f = Obj("VFreezeInMenuSubsystem")
    if rest == "1" then f:Freeze(FName("OblvBench"), true) else f:UnFreeze(FName("OblvBench"), true) end
    -- after an unfreeze the game was found still in its UI input mapping (2026-10-06): put the game mapping back
    local fix = (rest ~= "1" and not f:IsFreezing()) and EnsureGameInput() or ""
    return "frozen=" .. tostring(f:IsFreezing()) .. fix
end

-- "pause 0|1": engine-level pause (fallback for freeze)
function V.pause(rest)
    local gs = StaticFindObject("/Script/Engine.Default__GameplayStatics")
    gs:SetGamePaused(PC(), rest == "1")
    return "paused=" .. tostring(gs:IsGamePaused(PC()))
end

-- "activate": press and release the activate input (whatever is under the crosshair)
-- Injects the game's own "Activate" input action for one frame, exactly what the key does, so it
-- works while the game window is in the background. (Calling the controller's
-- ActivateInput_Pressed/Released, posting key messages, or the console's prid + activate do nothing.)
function V.activate()
    local fix = EnsureGameInput()
    Inject(ACTIVATE_ACTION)
    return "ok" .. fix
end

-- "walk <mode> <timeoutSec> <x,y> <x,y> ...": mode = inject (default) | input | auto. "EV walk=done" when over.
function V.walk(rest)
    local mode, timeout, pts = rest:match("^(%S+)%s+(%S+)%s+(.+)$")
    local list = {}
    for px, py in (pts or ""):gmatch("([-%d.]+),([-%d.]+)") do list[#list + 1] = { tonumber(px), tonumber(py) } end
    if #list == 0 or not tonumber(timeout) then error("usage: walk inject|input|auto <timeoutSec> x,y x,y ...") end
    if mode == "default" then mode = "inject" end
    if mode ~= "inject" and mode ~= "input" and mode ~= "auto" then error("unknown walk mode " .. mode) end
    if fly or saved then FlyStop("walk") end
    if walk then WalkStop("replaced") end
    local fix = mode ~= "input" and EnsureGameInput() or ""
    local pc = PC()
    local now = os.clock()
    walk = { pts = list, i = 1, mode = mode, radius = 150, turnRate = 240, pitch = 0.0, t0 = now, last = now, lastProg = now,
             deadline = now + tonumber(timeout), dist = 0, progDist = 0, frames = 0 }
    if mode == "auto" then
        local ok = pcall(Inject, AUTOMOVE_ACTION)
        if not ok then pc:AutoMoveInput_Pressed(); pcall(function() pc:AutoMoveInput_Released() end) end
    end
    return string.format("walking %d waypoints mode=%s driver=%s clock=%.3f%s", #list, mode, tostring(driver.mode), now, fix)
end
function V.walkstop() WalkStop("stopped"); return "ok" end

function V.watch(rest) watching = rest == "1"; lastSig = nil; lastPos = nil; return "watch=" .. tostring(watching) end

-- main menu
function V.continue() Obj("VMainMenuViewModel"):RegisterSendClickedContinue(); return "sent" end

-- "menu": one step towards the main menu's buttons, skipping the start-up videos and the title screen.
-- Answers "ready", "skipped <page>", "waiting" (start-up still running), or "eula=0" / "setup=0" when
-- the first-start EULA or settings are pending: those are the player's to accept, so the caller stops.
-- Without this step a game started with `coc` keeps the title screen's state (the view turns by itself).
-- Videos: the page's own skip-button click handler skips at once (VModernOnboardingMenu:Skip() returns
-- false and lets the video play out). Title screen: its mouse click catcher's click handler.
local TITLE_CLICK = "BndEvt__WBP_Modern_Onboarding_TitleScreen_MouseClickCatcher_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature"
local skipFn = {}           -- page class -> name of its skip-button click handler (false: none)
function V.menu()
    local lib = StaticFindObject("/Script/Altar.Default__VModernOnboardingFunctionLibrary")
    if not lib:IsEULAAccepted() then return "eula=0" end
    if not lib:IsOnboardingSettingsDone() then return "setup=0" end
    local m = Obj("WBP_Modern_OnboardingMenu_C")
    if not m then return "waiting" end
    if m:IsOnboardingFinished() then return "ready" end
    local page = m:GetCurrentPage()
    if not (page and page:IsValid()) then return "waiting" end
    local name = page:GetClass():GetFName():ToString()
    if name:find("TitleScreen") then page[TITLE_CLICK](page); return "skipped " .. name .. " r=click" end
    if skipFn[name] == nil then
        skipFn[name] = false
        page:GetClass():ForEachFunction(function(f)
            local n = f:GetFName():ToString()
            if n:find("SkipButton") and n:find("OnButtonClicked") then skipFn[name] = n end
        end)
    end
    if skipFn[name] then page[skipFn[name]](page); return "skipped " .. name .. " r=button" end
    return "skipped " .. name .. " r=" .. tostring(m:Skip())
end

-- "closemenu": leave the main menu for the world loaded behind it (after `coc` from the menu)
function V.closemenu() Obj("VMainMenuViewModel"):SendCloseMainMenu(); return "sent" end

-- "msg": text of the message box view model; "msg <n>": click its button n (0 = first, e.g. "Yes")
function V.msg(rest)
    local vm = Obj("VMessageMenuViewModel")
    if not vm then error("no message menu") end
    if rest ~= "" then vm:RegisterSendClickedButton(tonumber(rest)); return "clicked " .. rest end
    local buttons = {}
    pcall(function() vm:GetButtonsText():ForEach(function(_, b) buttons[#buttons + 1] = b:get():ToString() end) end)
    return "text=" .. vm:GetMessage():ToString() .. " | buttons=" .. table.concat(buttons, ",")
end
function V.quickload() PC():RequestQuickLoad(); return "sent" end

-- "ft open" | "ft list [text]" | "ft go <map icon name>": fast travel the way the map does it (doors2, task D).
-- open = the controller's own map input handler (toggles the map menu); the world map page view model's icons are
-- LegacyMapMenuIconProperties {Position, Key, Name, UnderlyingType, Type, FastTravelEnabled, DepthAdjustment,
-- IsTargetable, IsThroughLoadingDoor, PlayerRotation, IconAddress}; go = VMapPageViewModel:
-- RegisterSendWorldMapIconClickedHandlerName(Key) = the click the map UI sends to the old engine (pairing message
-- "MapPage_World_Map_Icon_Clicked_Handler_Name"), which then asks "travel to ...?" (answer with "msg 0").
local function MapIcons()
    local out = {}
    -- only the world map page: a click sent through the other (local map) page's view model crashed the old engine's
    -- handler (OblivionThread AV exe+0x6561A86, twice on 2026-10-05, when both pages listed the icon)
    local pages = {}
    pcall(function()
        local menu = FindFirstOf("VMapMenuViewModel")
        local w = menu and menu.WorldMapMenuViewModel
        if w and w:IsValid() then pages[1] = w end
    end)
    for _, vm in ipairs(pages) do
        if vm:IsValid() then
            pcall(function()
                vm.MapIcons:ForEach(function(_, e)
                    local s = e:get()
                    local function str(v) local ok, r = pcall(function() return v:ToString() end); return ok and r or tostring(v) end
                    out[#out + 1] = { vm = vm, s = s, key = str(s.Key), name = str(s.Name), ft = s.FastTravelEnabled }
                end)
            end)
        end
    end
    return out
end
function V.ft(rest)
    local cmd, arg = rest:match("^(%S+)%s*(.-)$")
    if cmd == "open" then
        -- the game's own input action, injected (calling the controller's OpenMapMenuInput_Pressed does nothing)
        Inject(IA .. "Default/IA_Game_Default_OpenMapMenu.IA_Game_Default_OpenMapMenu")
        return "map toggled"
    elseif cmd == "close" then
        -- the player menu's own close action (the map is a page of the player menu)
        Inject("/Game/Dev/Input/UserInterface/InputActions/Specific/PlayerMenu/IA_UI_Specific_PlayerMenu_CloseMenu.IA_UI_Specific_PlayerMenu_CloseMenu")
        return "close sent"
    elseif cmd == "list" then
        local out, icons = {}, MapIcons()
        for _, ic in ipairs(icons) do
            if arg == "" or ic.name:find(arg, 1, true) or ic.key:find(arg, 1, true) then
                if #out < 40 then out[#out + 1] = ic.name .. "[" .. ic.key .. "]" .. (ic.ft and "" or "(noft)") end
            end
        end
        return "icons=" .. #icons .. " " .. table.concat(out, ", ")
    elseif cmd == "go" then
        for _, ic in ipairs(MapIcons()) do
            if ic.name == arg or ic.key == arg then
                ic.vm:RegisterSendWorldMapIconClickedHandlerName(ic.s.Key)
                return "clicked " .. ic.name .. " key=" .. ic.key .. " ft=" .. tostring(ic.ft)
            end
        end
        error("no map icon named " .. arg)
    end
    error("usage: ft open | ft close | ft list [text] | ft go <name>")
end

-- "lua <chunk>": ad-hoc exploration
function V.lua(rest)
    local fn, err = load(rest)
    if not fn then error(err) end
    return tostring(fn())
end

-- "luafile <path>": run a Lua file (probes too long for one line)
function V.luafile(rest)
    local fn, err = loadfile(rest)
    if not fn then error(err) end
    return tostring(fn())
end

-- ---------------------------------------------------------------- entry points

function M.Run(line)
    local id, verb, rest = line:match("^%s*(%d+)%s+(%S+)%s*(.-)%s*$")
    if not id then return end
    local fn = V[verb]
    if not fn then log("ACK " .. id .. " ERR unknown verb " .. verb); return end
    local ok, res = pcall(fn, rest)
    res = tostring(res):gsub("[\r\n]+", " | ")
    log("ACK " .. id .. (ok and " OK " or " ERR ") .. res)
end

-- Busy: the bootstrap polls the command file at full rate and calls Tick
function M.WantsTick() return watching or walk ~= nil or fly ~= nil end

-- At the poll rate (20 Hz) while busy
function M.Tick()
    if walk and driver.mode ~= "frame" then WalkTick(os.clock()) end
    if watching then WatchTick() end
end

-- Every frame (frame driver only), on the game thread before the world ticks
function M.Frame(now)
    -- probes can run code every frame: a Lua file run with `luafile` sets BenchFrameExtra = function(now) ... end
    -- (removed after its first error; nil it to stop).
    if BenchFrameExtra then
        local okX, errX = pcall(BenchFrameExtra, now)
        if not okX then log("ERROR frame extra: " .. (tostring(errX):gsub("[\r\n]+", " | "))); BenchFrameExtra = nil end
    end
    local okPin, errPin = pcall(PinTick, now)
    if not okPin then log("ERROR pin: " .. (tostring(errPin):gsub("[\r\n]+", " | "))); pins = {} end
    if airhold then
        local ok, err = pcall(AirholdFrame, now)
        if not ok then log("ERROR airhold: " .. (tostring(err):gsub("[\r\n]+", " | "))); airhold = nil end
    end
    if fly then
        local ok, err = pcall(fly.kind == "path" and FlyFrame or TestFrame, now)
        if not ok then
            log("ERROR fly: " .. (tostring(err):gsub("[\r\n]+", " | ")))
            FlyStop("error")
        end
    end
    if walk then
        local ok, err = pcall(WalkTick, now)
        if not ok then
            log("ERROR walk: " .. (tostring(err):gsub("[\r\n]+", " | ")))
            WalkStop("error")
        end
    end
    if fade then
        local ok, err = pcall(FadeFrame, now)
        if not ok then log("ERROR fade: " .. (tostring(err):gsub("[\r\n]+", " | "))); fade = nil end
    end
    if cam.on then
        local ok, err = pcall(CamFrame, now)
        if not ok then
            cam.errors = (cam.errors or 0) + 1
            if now - (cam.errAt or 0) >= 2 then cam.errAt = now; log("ERROR cam (x" .. cam.errors .. "): " .. (tostring(err):gsub("[\r\n]+", " | "))) end
        end
    end
end

-- Before a reload replaces this module: leave the pawn in normal movement
function M.Unload()
    if fly or saved then FlyStop("reload") end
    if walk then WalkStop("reload") end
    if fade then fade = nil; pcall(function() PC().PlayerCameraManager:StopCameraFade() end) end
    CamOff()
end

function M.Init(logFn, drv)
    log = logFn
    if drv then driver = drv end
    -- opt-in probe autoload: if bench-autoload.lua exists in the runtime folder it is run each time this module loads (every
    -- run), so probes (BenchWatchExtra / BenchFrameExtra) are in place even in a newly started game. Delete the file
    -- when done.
    local f = OBLV_RUNTIME and loadfile(OBLV_RUNTIME .. "bench-autoload.lua")
    if f then
        local ok, res = pcall(f)
        log("autoload: " .. (ok and tostring(res) or ("ERROR " .. tostring(res))):gsub("[\r\n]+", " | "))
    end
end

return M
