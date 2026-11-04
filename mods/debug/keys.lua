-- Hotkeys: one row per action, each an SDL key name ("F3", "V", "Left Shift").
-- Names live in config.keys, not the old config.hotkeys, so the defaults show.

local keys = {title = "Keys"}

-- A row is one hotkey. Adding one is a new row here.
local ROWS = {
  {action = "panel", label = "Panel", default = "F3"},
  {action = "fly", label = "Fly", default = "F6"},
  {action = "invincible", label = "God mode", default = "F7"},
  {action = "bookmark_save", label = "Save warp 1", default = "F8"},
  {action = "bookmark_go", label = "Go to warp 1", default = "F9"},
  {action = "snap", label = "Snap to floor", default = "F10"},
  {action = "return_to_start", label = "Back to start", default = "F12"},
  {action = "film", label = "Film mode", default = "V"},
  {action = "fly_up", label = "Fly up (held)", default = "E"},
  {action = "fly_down", label = "Fly down (held)", default = "C"},
}

if config.keys_enabled == nil then config.keys_enabled = true end
config.keys = config.keys or {}
for _, row in ipairs(ROWS) do
  if type(config.keys[row.action]) ~= "string" then
    config.keys[row.action] = row.default
  end
end

-- kf.key_down raises on a name SDL does not know; pcall tells the two apart.
local valid = {}
local function is_valid(name)
  if valid[name] == nil then
    valid[name] = pcall(kf.key_down, name)
  end
  return valid[name]
end

-- The key name for an action, or nil when it does nothing.
local function bound(action)
  if not config.keys_enabled then return nil end
  local name = config.keys[action]
  if name == nil or name == "" or not is_valid(name) then return nil end
  return name
end

function keys.pressed(action)
  local name = bound(action)
  return name ~= nil and kf.key_pressed(name)
end

function keys.down(action)
  local name = bound(action)
  return name ~= nil and kf.key_down(name)
end

local function draw_enabled()
  local on = ui.checkbox("Hotkeys on", config.keys_enabled)
  if on ~= config.keys_enabled then
    config.keys_enabled = on
    mod.save_config()
  end
end

local function draw_row(row)
  local text = config.keys[row.action]
  local new = ui.input_text(row.label, text)
  if new ~= text then
    config.keys[row.action] = new
    mod.save_config()
  end
  if new ~= "" and not is_valid(new) then
    ui.text_colored(1, 0.35, 0.35, "Not a key name: " .. new)
  end
end

local function draw_held()
  ui.separator()
  ui.text_wrapped("In flight the walk keys (forward, back, strafe) fly you, "
    .. "and the run key makes it fast. Fly up and fly down are held.")
end

function keys.draw()
  draw_enabled()
  for _, row in ipairs(ROWS) do
    draw_row(row)
  end
  draw_held()
end

return keys
