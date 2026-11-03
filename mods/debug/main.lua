-- Debug: cheats and warps for testing. Switches are off until turned on here
-- or with their hotkey: config.hotkeys holds an SDL key name ("F9"), "" for none.

if config.noclip == nil then config.noclip = false end
if config.invincible == nil then config.invincible = false end
if config.infinite_mp == nil then config.infinite_mp = false end
if config.peaceful == nil then config.peaceful = false end
config.move_speed = config.move_speed or 1.0
config.game_speed = config.game_speed or 1.0
config.bookmarks = config.bookmarks or {}   -- slots "1" to "4"
config.hotkeys = config.hotkeys or {}

local switches = {
  {key = "noclip", label = "Noclip"},
  {key = "invincible", label = "God mode"},
  {key = "infinite_mp", label = "Infinite MP"},
  {key = "peaceful", label = "Peaceful"},
}
for _, s in ipairs(switches) do
  config.hotkeys[s.key] = config.hotkeys[s.key] or ""
end

-- Hands the values to the game.
local function apply()
  kf.play.noclip = config.noclip
  kf.play.invincible = config.invincible
  kf.play.peaceful = config.peaceful
  kf.play.move_speed = config.move_speed
  kf.game.speed = config.game_speed
end

-- Stores a new value and applies it, if it changed.
local function set(key, value)
  if value ~= config[key] then
    config[key] = value
    apply()
  end
end

local function warp(...)
  local ok, err = pcall(kf.warp, ...)
  if not ok then mod.log(err) end
end

function on_load()
  apply()
end

function on_update()
  apply()   -- the game's menus change config directly
  if config.infinite_mp and kf.context() == "gameplay" and kf.player.mp < kf.player.max_mp then
    kf.player.mp = kf.player.max_mp
  end
  local changed = false
  for _, s in ipairs(switches) do
    local key = config.hotkeys[s.key]
    if key and key ~= "" and kf.key_pressed(key) then
      config[s.key] = not config[s.key]
      changed = true
    end
  end
  if changed then
    mod.save_config()
    apply()
  end
end

function on_settings()
  for _, s in ipairs(switches) do
    set(s.key, ui.checkbox(s.label, config[s.key]))
  end
  set("move_speed", ui.slider_float("Move speed", config.move_speed, 0.25, 8))
  set("game_speed", ui.slider_float("Game speed", config.game_speed, 0.25, 8))

  ui.separator()
  local p = kf.player
  ui.text(string.format("Floor %d, cell %d, %d, yaw %d",
    p.floor, p.cell_x, p.cell_z, p.yaw))
  for n = 1, 5 do
    if ui.button("Warp to floor " .. n) then warp(n) end
  end

  ui.separator()
  for i = 1, 4 do
    local slot = tostring(i)
    if ui.button("Save " .. i) then
      config.bookmarks[slot] = {floor = p.floor, cell_x = p.cell_x,
        cell_z = p.cell_z, yaw = p.yaw}
      mod.save_config()
    end
    local b = config.bookmarks[slot]
    if b then
      ui.same_line()
      if ui.button("Go " .. i) then warp(b.floor, b.cell_x, b.cell_z, b.yaw) end
    end
  end
end
