-- Warp tab: marks you can return to, floor jumps and exact coordinates.
local flight = require("flight")

local M = { title = "Warp" }

local CELL = 2000
local FLOORS = 5
local SLOTS = { "1", "2", "3", "4" }

local status = ""
local pending = nil   -- an exact mark waiting for its floor to load
local coords = nil    -- { x, y, z } in the coordinate boxes

local function attempt(fn, ...)
  local ok, err = pcall(fn, ...)
  if not ok then status = tostring(err) end
  return ok
end

local function cell_of(world)
  return math.floor(world / CELL)
end

-- Marks saved before exact positions held only a cell and a yaw.
local function migrate()
  config.marks = config.marks or {}
  if config.bookmarks == nil then return end
  for _, slot in ipairs(SLOTS) do
    local old = config.bookmarks[slot]
    if old and config.marks[slot] == nil then
      config.marks[slot] = { floor = old.floor, cell_x = old.cell_x, cell_z = old.cell_z, yaw = old.yaw }
    end
  end
  config.bookmarks = nil
  mod.save_config()
end
migrate()

local function describe(mark)
  local cx = mark.cell_x or cell_of(mark.x)
  local cz = mark.cell_z or cell_of(mark.z)
  return string.format("floor %d, cell %d, %d", mark.floor, cx, cz)
end

-- Returns the warning for a cell the player would stand in a wall.
local function blocked_note(x, z)
  if not kf.world.cell_blocked(cell_of(x), cell_of(z)) then return nil end
  if kf.play.noclip or kf.play.fly then return nil end
  return "That cell is a wall. Turn on noclip or fly to stay in it."
end

local function move_to(x, y, z)
  kf.move_player(x, y, z)
  status = blocked_note(x, z) or "Moved."
end

local function move_exact(mark)
  kf.move_player(mark.x, mark.y, mark.z)
  kf.player.yaw = mark.yaw
  kf.player.pitch = mark.pitch
  status = blocked_note(mark.x, mark.z) or "Went to the mark."
end

function M.save(slot)
  local p = kf.player
  config.marks[slot] = { floor = p.floor, x = p.x, y = p.y, z = p.z, yaw = p.yaw, pitch = p.pitch }
  mod.save_config()
  status = "Saved mark " .. slot .. "."
end

function M.go(slot)
  local mark = config.marks[slot]
  if not mark then
    status = "Mark " .. slot .. " is empty."
    return
  end
  if mark.x == nil then
    attempt(kf.warp, mark.floor, mark.cell_x, mark.cell_z, mark.yaw)
    return
  end
  if mark.floor == kf.player.floor then
    pending = nil
    attempt(move_exact, mark)
    return
  end
  pending = attempt(kf.warp, mark.floor, cell_of(mark.x), cell_of(mark.z), mark.yaw) and mark or nil
end

local function clear(slot)
  config.marks[slot] = nil
  mod.save_config()
  status = "Cleared mark " .. slot .. "."
end

local function read_coords()
  coords = { x = kf.player.x, y = kf.player.y, z = kf.player.z }
end

local function go_to_coords()
  pending = nil
  attempt(move_to, coords.x, coords.y, coords.z)
end

local function warp_to_floor(floor)
  pending = nil
  if attempt(kf.warp, floor) then status = "Going to floor " .. floor .. "." end
end

local function snap_to_floor()
  pending = nil
  attempt(flight.snap_to_floor)
  read_coords()
end

local function draw_marks(w)
  w:heading("Marks")
  for _, slot in ipairs(SLOTS) do
    local mark = config.marks[slot]
    w:line()
    w:button("Save " .. slot, function() M.save(slot) end)
    if mark then
      w:button("Go " .. slot, function() M.go(slot) end)
      w:button("Clear " .. slot, function() clear(slot) end)
      w:add('<span class="dim">' .. w.escaped(describe(mark)) .. "</span>")
    end
    w:line_end()
  end
end

local function draw_coords(w)
  coords = coords or { x = kf.player.x, y = kf.player.y, z = kf.player.z }
  w:heading("Coordinates")
  for _, axis in ipairs({ "x", "y", "z" }) do
    w:stepper(axis:upper(), coords[axis], {100, 2000}, function(v) coords[axis] = v end)
  end
  w:help("World Y grows down: a smaller Y is higher up. Y is your eye height. One cell is 2000.")
  w:line()
  w:button("Go", go_to_coords)
  w:button("Read current", read_coords)
  w:button("Snap to floor", snap_to_floor)
  w:line_end()
end

local function draw_floors(w)
  w:heading("Floors")
  w:text("Current floor: " .. tostring(kf.player.floor))
  w:line()
  for floor = 1, FLOORS do
    w:button("Floor " .. floor, function() warp_to_floor(floor) end)
  end
  w:line_end()
end

function M.draw(w)
  if kf.context() == "opening" then
    w:text("Load a save or start a game.")
    return
  end
  if status ~= "" then w:text(status, "warning") end
  draw_marks(w)
  draw_coords(w)
  draw_floors(w)
end

-- Finishes an exact mark once its floor has loaded.
function M.update()
  if pending == nil or kf.player.floor ~= pending.floor then return end
  local mark = pending
  pending = nil
  attempt(move_exact, mark)
end

return M
