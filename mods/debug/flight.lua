-- Flight: move through the world by hand, height included.
-- World Y grows down, so up is a smaller y. Positions are floats here, since a
-- whole-unit step at low speed would truncate to nothing.

local keys = require("keys")

local flight = {}

local CELL = 2000
local YAW_FULL = 4096
local PITCH_LIMIT = 191
local Y_LIMIT = 200000
local MAX_STEP = 0.1   -- seconds; a gap across a load must not fling the player
local MOVES = {"forward", "backward", "strafe_left", "strafe_right"}

local was_on = false
local need_start = false
local last_floor = nil
local last_cinematic = false
local last_time = 0
local last_look_time = 0
local fx, fy, fz = 0, 0, 0
local wx, wy, wz = 0, 0, 0   -- what we last wrote, to see a move from outside
local vx, vy, vz = 0, 0, 0
local start = nil
local open_spot = nil
-- Smoothed view for the cinematic camera. "written" is what we last wrote, so the
-- game's own turn is the difference from it.
local look = {yaw_target = 0, yaw_smooth = 0, yaw_written = 0,
  pitch_target = 0, pitch_smooth = 0, pitch_written = 0}

local function round(v)
  return math.floor(v + 0.5)
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- Share of the way to a target after dt seconds, for time constant tau.
local function share(dt, tau)
  if tau <= 0 then return 1 end
  return 1 - math.exp(-dt / tau)
end

-- Shortest signed turn from a to b, in 12-bit angle units.
local function turn_between(a, b)
  local d = (b - a) % YAW_FULL
  if d >= YAW_FULL / 2 then d = d - YAW_FULL end
  return d
end

local function axis(plus, minus)
  return (plus and 1 or 0) - (minus and 1 or 0)
end

-- 12-bit angle units to radians.
local function radians(units)
  return units * 2 * math.pi / YAW_FULL
end

local function reseed()
  fx, fy, fz = kf.player.x, kf.player.y, kf.player.z
  wx, wy, wz = fx, fy, fz
  vx, vy, vz = 0, 0, 0
  last_floor = kf.player.floor
  last_cinematic = config.cinematic
  last_time = kf.time()
  last_look_time = last_time
  local yaw, pitch = kf.player.yaw, kf.player.pitch
  look.yaw_target, look.yaw_smooth, look.yaw_written = yaw, yaw, yaw
  look.pitch_target, look.pitch_smooth, look.pitch_written = pitch, pitch, pitch
end

local function remember_start()
  local p = kf.player
  start = {floor = p.floor, x = p.x, y = p.y, z = p.z, yaw = p.yaw, pitch = p.pitch}
  need_start = false
end

local function block(on)
  for _, action in ipairs(MOVES) do
    kf.action_block(action, on)
  end
end

local function begin_flight()
  block(true)
  need_start = true
  last_floor = nil
  open_spot = nil
end

local function end_flight()
  block(false)
  -- A landing in rock would kill or trap; go back to the last open ground first.
  if open_spot and kf.world.cell_blocked(kf.player.cell_x, kf.player.cell_z) then
    pcall(kf.move_player, open_spot.x, nil, open_spot.z)
  end
  flight.snap_to_floor()
end

local function sync_switch()
  local on = config.fly == true
  if on == was_on then return end
  -- Switched off in a menu: the landing waits for play, and holds_height keeps the player up till then.
  if not on and kf.context() ~= "gameplay" then return end
  was_on = on
  if on then begin_flight() else end_flight() end
end

-- Cinematic only: eases the view toward where the game turned it.
local function turn_view(dt)
  local yaw_seen = kf.player.yaw
  look.yaw_target = (look.yaw_target + turn_between(look.yaw_written, yaw_seen)) % YAW_FULL
  look.yaw_smooth = (look.yaw_smooth
    + turn_between(look.yaw_smooth, look.yaw_target) * share(dt, config.look_smoothing)) % YAW_FULL
  look.yaw_written = round(look.yaw_smooth) % YAW_FULL
  kf.player.yaw = look.yaw_written

  local pitch_seen = kf.player.pitch
  look.pitch_target = clamp(look.pitch_target + pitch_seen - look.pitch_written,
    -PITCH_LIMIT, PITCH_LIMIT)
  look.pitch_smooth = look.pitch_smooth
    + (look.pitch_target - look.pitch_smooth) * share(dt, config.look_smoothing)
  look.pitch_written = clamp(round(look.pitch_smooth), -PITCH_LIMIT, PITCH_LIMIT)
  kf.player.pitch = look.pitch_written
end

-- Units a second wanted from the input, along the view and across it.
local function wanted_velocity(input)
  local speed = config.fly_speed
  if input.fast then speed = speed * config.fly_fast end
  local h = radians(kf.player.yaw)
  local v = radians(kf.player.pitch)
  local along = input.forward * speed
  local across = input.strafe * speed
  local dx = -math.sin(h) * along * math.cos(v) + math.cos(h) * across
  local dz = math.cos(h) * along * math.cos(v) + math.sin(h) * across
  -- Pitch is negative when looking up (look_up lowers it), and up is a smaller y.
  local dy = math.sin(v) * along - input.up * speed
  return dx, dy, dz
end

local function ease_velocity(dt, want_x, want_y, want_z)
  if not config.cinematic then
    vx, vy, vz = want_x, want_y, want_z
    return
  end
  local k = share(dt, config.move_smoothing)
  vx = vx + (want_x - vx) * k
  vy = vy + (want_y - vy) * k
  vz = vz + (want_z - vz) * k
end

local function remember_open_spot()
  local cx = math.floor(fx / CELL)
  local cz = math.floor(fz / CELL)
  if not kf.world.cell_blocked(cx, cz) then
    open_spot = {x = round(fx), z = round(fz)}
  end
end

-- Off the map the move fails and the player stays put; the floats follow them back.
local function write_position()
  wx, wy, wz = round(fx), round(fy), round(fz)
  if pcall(kf.move_player, wx, wy, wz) then
    remember_open_spot()
    return
  end
  fx, fy, fz = kf.player.x, kf.player.y, kf.player.z
  wx, wy, wz = fx, fy, fz
end

-- A warp or another mod moved the player since our last write.
local function moved_elsewhere()
  return kf.player.x ~= wx or kf.player.y ~= wy or kf.player.z ~= wz
end

local function disturbed()
  return kf.player.floor ~= last_floor or config.cinematic ~= last_cinematic or moved_elsewhere()
end

-- After the game's look and before the update's camera, so the camera has only the eased view:
-- from on_update the camera would carry each update's raw mouse turn on top of it.
local function ease_view_after_player_update()
  if not flight.eases_view() or not was_on or need_start or kf.context() ~= "gameplay" then return end
  if disturbed() then reseed() end
  local now = kf.time()
  local dt = clamp(now - last_look_time, 0, MAX_STEP)
  last_look_time = now
  turn_view(dt)
end

local function fly_step(dt)
  local input = flight._input()
  local want_x, want_y, want_z = wanted_velocity(input)
  ease_velocity(dt, want_x, want_y, want_z)
  fx = fx + vx * dt
  fy = clamp(fy + vy * dt, -Y_LIMIT, Y_LIMIT)
  fz = fz + vz * dt
  write_position()
end

-- Reads the held keys. Tests replace flight._input.
local function read_input()
  return {
    forward = axis(kf.action_down("forward"), kf.action_down("backward")),
    strafe = axis(kf.action_down("strafe_right"), kf.action_down("strafe_left")),
    up = axis(keys.down("fly_up"), keys.down("fly_down")),
    fast = kf.action_down("run"),
  }
end
flight._input = read_input

-- Called from on_update every update.
function flight.update()
  sync_switch()
  if not config.fly then return end
  if kf.context() ~= "gameplay" then
    reseed()
    return
  end
  if need_start then remember_start() end
  if disturbed() then reseed() end
  local now = kf.time()
  local dt = clamp(now - last_time, 0, MAX_STEP)
  last_time = now
  fly_step(dt)
end

function flight.snap_to_floor()
  if kf.context() ~= "gameplay" then return false end
  local standing_y = kf.world.standing_y(kf.player.cell_x, kf.player.cell_z)
  if not pcall(kf.move_player, kf.player.x, standing_y, kf.player.z) then return false end
  reseed()
  return true
end

function flight.return_to_start()
  if kf.context() ~= "gameplay" or start == nil then return false end
  if start.floor ~= kf.player.floor then return false end
  if not pcall(kf.move_player, start.x, start.y, start.z) then return false end
  kf.player.yaw = start.yaw
  kf.player.pitch = start.pitch
  reseed()
  return true
end

-- Film mode: the three switches together.
function flight.toggle_film()
  local on = config.cinematic and config.peaceful and config.fly
  config.cinematic = not on
  config.peaceful = not on
  config.fly = not on
  mod.save_config()
end

-- The cinematic camera eases only in flight.
function flight.eases_view()
  return config.fly == true and config.cinematic == true
end

function flight.is_flying()
  return config.fly == true
end

-- True until the landing has run: the game updates before on_update sees a switch turned off, so
-- letting go at once would drop the player from the air, which can kill.
function flight.holds_height()
  return config.fly == true or was_on
end

-- Lands the player and unblocks the walk keys when the mod goes.
function flight.release()
  if was_on and kf.context() == "gameplay" then end_flight() end
  block(false)
end

local function number(key, value)
  if value ~= config[key] then
    config[key] = value
    mod.save_config()
  end
end

local function flight_buttons()
  if ui.button("Go back") then flight.return_to_start() end
  ui.tooltip("Back to where flight began, on this floor: the undo for a flight that went too far.")
  ui.same_line()
  if ui.button("Snap to floor") then flight.snap_to_floor() end
  ui.tooltip("Drop to the ground under you.")
end

local function cinematic_controls()
  local on = ui.checkbox("Cinematic camera", config.cinematic)
  if on ~= config.cinematic then
    config.cinematic = on
    mod.save_config()
  end
  ui.tooltip("Ease the flight and the turn instead of landing on the input, for filming. "
    .. "The velocity builds up and coasts down, and the view trails where you look.")
  if not config.cinematic then return end
  number("move_smoothing", ui.slider_float("Move smoothing", config.move_smoothing,
    0.05, 2, "%.2f s"))
  ui.tooltip("How long the flight takes to reach full speed, in seconds.")
  number("look_smoothing", ui.slider_float("Look smoothing", config.look_smoothing,
    0.05, 2, "%.2f s"))
  ui.tooltip("How long the view takes to catch up with a turn, in seconds.")
end

-- Drawn in the Cheats tab, under the switches.
function flight.draw()
  if not config.fly then return end
  number("fly_speed", ui.slider_float("Fly speed", config.fly_speed, 20, 8000))
  ui.tooltip("World units a second. Hold Run to multiply it by Fast.")
  number("fly_fast", ui.slider_float("Fast", config.fly_fast, 1, 10))
  ui.tooltip("How many times faster Run makes flight.")
  cinematic_controls()
  flight_buttons()
end

kf.hook.after.player_update(ease_view_after_player_update)

return flight
