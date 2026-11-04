-- State tab: a live readout. Reads only; it changes nothing.
local magic = require("magic")

local M = { title = "State" }

local SLOTS = { "weapon", "head", "body", "shield", "arm", "leg", "accessory" }

local function field(name)
  return function() return kf.player[name] end
end

local function equipped(slot)
  return function()
    local id = kf.equipped(slot)
    if id == nil then return "(none)" end
    return kf.items.name(id)
  end
end

local function selected_spell()
  local spell = kf.magic.selected()
  if spell == nil then return "none" end
  return spell .. " " .. magic.spell_names[spell + 1]
end

-- Each row: { label, getter }. A getter that raises shows "unavailable".
local WORLD = {
  { "Context", function() return kf.context() end },
  { "Frame", function() return kf.frame() end },
  { "Floor", field("floor") },
  { "Map variant", field("map_variant") },
  { "Highest floor", field("highest_floor") },
}

local POSITION = {
  { "X", field("x") },
  { "Y", field("y") },
  { "Z", field("z") },
  { "Cell X", field("cell_x") },
  { "Cell Z", field("cell_z") },
  { "Yaw", field("yaw") },
  { "Pitch", field("pitch") },
}

local PLAYER = {
  { "HP", field("hp") },
  { "Max HP", field("max_hp") },
  { "MP", field("mp") },
  { "Max MP", field("max_mp") },
  { "Level", field("level") },
  { "Gold", field("gold") },
  { "Experience", field("experience") },
  { "Status flags", field("status_flags") },
  { "Selected spell", selected_spell },
}

local WORN = {}
for _, slot in ipairs(SLOTS) do
  WORN[#WORN + 1] = { slot, equipped(slot) }
end

-- kf.raw paths, read through the raw layer; its names can change.
local function raw(path)
  return function()
    local value = kf.raw
    for part in path:gmatch("[^.]+") do
      value = value[part]
    end
    return value
  end
end

local RAW = {
  { "Foot height", raw("player_state.foot_height") },
  { "Vertical state", raw("player_state.vertical_state") },
  { "Vertical velocity", raw("player_state.vertical_velocity") },
  { "Update state", raw("player_state.update_state") },
  { "View bob offset", raw("player_state.view_bob_offset") },
  { "Movement speed", raw("player_state.motion_state.movement_speed") },
  { "Forward velocity", raw("player_state.motion_state.forward_velocity") },
  { "Strafe velocity", raw("player_state.motion_state.strafe_velocity") },
  { "Yaw step", raw("player_state.motion_state.yaw_step") },
  { "Pitch step", raw("player_state.motion_state.pitch_step") },
}

-- A row whose getter raises shows "unavailable" rather than stopping the mod.
local function show_rows(rows)
  for _, row in ipairs(rows) do
    local ok, value = pcall(row[2])
    if not ok then value = "unavailable" end
    ui.text(row[1] .. ": " .. tostring(value))
  end
end

function M.draw()
  if kf.context() == "opening" then
    ui.text("Load a save or start a game.")
    return
  end
  if ui.collapsing_header("World", true) then show_rows(WORLD) end
  if ui.collapsing_header("Position", true) then show_rows(POSITION) end
  if ui.collapsing_header("Player", true) then show_rows(PLAYER) end
  if ui.collapsing_header("Equipment") then show_rows(WORN) end
  if ui.collapsing_header("Raw layer") then show_rows(RAW) end
end

return M
