-- Debug: cheats, flight, editors and warps for testing. Switches are off until turned
-- on here, in the menu, or with their hotkey (keys.lua). The panel opens with F3.

local keys = require("keys")
local flight = require("flight")
local character = require("character")
local items = require("items")
local magic = require("magic")
local warp = require("warp")
local state = require("state")

local function default(key, value)
  if config[key] == nil then config[key] = value end
end
for _, key in ipairs({"noclip", "invincible", "infinite_mp", "peaceful", "fly", "cinematic"}) do
  default(key, false)
end
default("move_speed", 1.0)
default("turn_speed", 1.0)
default("game_speed", 1.0)
default("fly_speed", 4000)
default("fly_fast", 4.0)
default("move_smoothing", 0.4)
default("look_smoothing", 0.4)

-- Each editor is a tab, with .title, .draw(), and optional .update() and .between_updates().
local editors = {character, items, magic, warp, state}

local panel_open = false   -- not saved: closed at start

local function apply()
  kf.play.noclip = config.noclip or flight.holds_height()
  kf.play.fly = flight.holds_height()
  kf.play.invincible = config.invincible
  kf.play.peaceful = config.peaceful
  kf.play.move_speed = config.move_speed
  kf.play.turn_speed = config.turn_speed
  kf.game.speed = config.game_speed
end

-- Stores a value, saves it and applies it, if it changed.
local function set(key, value)
  if value == config[key] then return end
  config[key] = value
  mod.save_config()
  apply()
end

local function toggle(key)
  set(key, not config[key])
end

local function toggle_panel()
  panel_open = not panel_open
  kf.game.mouse = panel_open
end

local function in_play()
  return kf.context() == "gameplay"
end

local function refill_mp()
  if config.infinite_mp and in_play() and kf.player.mp < kf.player.max_mp then
    kf.player.mp = kf.player.max_mp
  end
end

-- Hotkey actions, in order. The flag says the action needs a running game.
local HOTKEYS = {
  {action = "panel", run = toggle_panel, in_play = false},
  {action = "fly", run = function() toggle("fly") end, in_play = false},
  {action = "invincible", run = function() toggle("invincible") end, in_play = false},
  {action = "bookmark_save", run = function() warp.save(1) end, in_play = true},
  {action = "bookmark_go", run = function() warp.go(1) end, in_play = true},
  {action = "snap", run = flight.snap_to_floor, in_play = true},
  {action = "return_to_start", run = flight.return_to_start, in_play = true},
  {action = "film", run = flight.toggle_film, in_play = false},
}

-- Hotkeys are read here only: a key's press is seen once per update.
local function read_hotkeys()
  for _, hotkey in ipairs(HOTKEYS) do
    if keys.pressed(hotkey.action) and (in_play() or not hotkey.in_play) then
      hotkey.run()
      apply()
    end
  end
end

local function draw_cheats()
  set("noclip", ui.checkbox("Noclip", config.noclip))
  ui.tooltip("Walk through walls. Your height still follows the floor.")
  set("fly", ui.checkbox("Fly", config.fly))
  ui.tooltip("Float through the world with your walk keys. E and C go up and down. "
    .. "Walls do not stop you, and falls cannot kill you. F6 toggles it.")
  set("invincible", ui.checkbox("God mode", config.invincible))
  ui.tooltip("Blocks HP loss and fatal falls. Poison or curse from a blow still land.")
  set("infinite_mp", ui.checkbox("Infinite MP", config.infinite_mp))
  ui.tooltip("Refills MP every update, in the game world only.")
  set("peaceful", ui.checkbox("Peaceful", config.peaceful))
  ui.tooltip("Enemies never attack you.")
  ui.separator()
  set("move_speed", ui.slider_float("Move speed", config.move_speed, 0.25, 8))
  ui.tooltip("How fast you walk, from a quarter to eight times normal.")
  set("turn_speed", ui.slider_float("Turn speed", config.turn_speed, 0.25, 8))
  ui.tooltip("How fast the stick turns you, from a quarter to eight times normal.")
  set("game_speed", ui.slider_float("Game speed", config.game_speed, 0.25, 8))
  ui.tooltip("How fast the whole game runs: 0.25 is slow motion, 8 is fast forward.")
  flight.draw()
end

local cheats_tab = {title = "Cheats", draw = draw_cheats}
local tabs = {cheats_tab, character, items, magic, warp, state, keys}

local function draw_tabs()
  if not ui.begin_tab_bar("debug") then return end
  for _, tab in ipairs(tabs) do
    if ui.begin_tab(tab.title) then
      tab.draw()
      ui.end_tab()
    end
  end
  ui.end_tab_bar()
end

function on_load()
  apply()
end

function on_update()
  apply()
  read_hotkeys()
  refill_mp()
  flight.update()
  if in_play() then
    for _, editor in ipairs(editors) do
      if editor.update then editor.update() end
    end
  end
end

function on_between_updates()
  for _, editor in ipairs(editors) do
    if editor.between_updates then editor.between_updates() end
  end
end

function on_draw()
  if not panel_open then return end
  if ui.begin_window("Debug") then draw_tabs() end
  ui.end_window()
end

function on_settings()
  draw_tabs()
end

function on_unload()
  flight.release()
end
