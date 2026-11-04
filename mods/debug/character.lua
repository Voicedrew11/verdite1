-- Character tab: the player's numbers, conditions and combat ratings.
local M = { title = "Character" }

local status = ""
local queue = {}   -- kf.call work, run in between_updates
local held = nil   -- the combat ratings to keep, or nil

local function attempt(fn, ...)
  local ok, err = pcall(fn, ...)
  if not ok then status = tostring(err) end
  return ok
end

local function set_player(field, value)
  kf.player[field] = value
end

local CHARACTER = {
  { "level", "Level" },
  { "experience", "Experience" },
  { "next_level_experience", "Next level at" },
  { "hp", "HP" },
  { "max_hp", "Max HP" },
  { "mp", "MP" },
  { "max_mp", "Max MP" },
  { "gold", "Gold" },
  { "base_physical_power", "Base physical power" },
  { "base_magic", "Base magic" },
  { "physical_power", "Physical power" },
  { "magic", "Magic" },
  { "physical_power_training", "Physical training" },
  { "magic_training", "Magic training" },
  { "magic_charge", "Magic charge" },
}

local CONDITION = {
  { "curse_timer", "Curse timer" },
  { "darkness_timer", "Darkness timer" },
  { "poison_timer", "Poison timer" },
  { "slowed_timer", "Slowed timer" },
}

local FLAGS = {
  { 1, "Cursed" },
  { 2, "Darkness" },
  { 4, "Poisoned" },
  { 8, "Slowed" },
  { 16, "Fire defense boost" },
}

local RATINGS = {
  { "cutting_attack", "Cutting attack" },
  { "striking_attack", "Striking attack" },
  { "piercing_attack", "Piercing attack" },
  { "holy_attack", "Holy attack" },
  { "fire_attack", "Fire attack" },
  { "cutting_defense", "Cutting defense" },
  { "striking_defense", "Striking defense" },
  { "piercing_defense", "Piercing defense" },
  { "poison_resistance", "Poison resistance" },
  { "magic_defense", "Magic defense" },
  { "fire_defense", "Fire defense" },
}

local function full_heal()
  kf.player.hp = kf.player.max_hp
  kf.player.mp = kf.player.max_mp
end

local function cure()
  kf.player.status_flags = 0
  kf.player.curse_timer = 0
  kf.player.darkness_timer = 0
  kf.player.poison_timer = 0
  kf.player.slowed_timer = 0
end

-- Queued: the game's own level-up, which grants the level's gains.
local function level_up()
  local need = kf.player.next_level_experience - kf.player.experience
  kf.call.player_add_experience(math.max(need, 0))
end

local function recalculate()
  kf.call.player_recalculate_combat_stats()
end

local function hold_ratings()
  local values = {}
  for _, row in ipairs(RATINGS) do
    values[row[1]] = kf.player[row[1]]
  end
  return values
end

local function draw_buttons()
  if ui.button("Full heal") then attempt(full_heal) end
  ui.tooltip("Sets HP and MP to their maximums.")
  if ui.button("Cure") then attempt(cure) end
  ui.tooltip("Clears the status conditions and the curse, darkness, poison and slow timers.")
  if ui.button("Level up") then queue[#queue + 1] = level_up end
  ui.tooltip("Gives the experience to reach the next level, so the game levels you up and grants the gains.")
  if ui.button("Recalculate") then queue[#queue + 1] = recalculate end
  ui.tooltip("Has the game work out the combat ratings again from your gear and level.")
end

-- Writes one row's value, and keeps it while the ratings are held.
local function draw_rows(rows)
  for _, row in ipairs(rows) do
    local field = row[1]
    local value = kf.player[field]
    local new = ui.input_int(row[2], value)
    if new ~= value and attempt(set_player, field, new) then
      if held and held[field] ~= nil then held[field] = new end
    end
  end
end

local function draw_flags()
  local flags = kf.player.status_flags
  for _, row in ipairs(FLAGS) do
    local bit = row[1]
    local on = (flags & bit) ~= 0
    local now = ui.checkbox(row[2], on)
    if now ~= on then
      local new = now and (flags | bit) or (flags & ~bit)
      attempt(set_player, "status_flags", new)
      flags = new
    end
  end
end

local function draw_hold()
  local on = held ~= nil
  local now = ui.checkbox("Hold these values", on)
  if now ~= on then held = now and hold_ratings() or nil end
  ui.tooltip("The game recalculates the combat ratings. While this is on, they are kept at the values you set.")
end

function M.draw()
  if kf.context() == "opening" then
    ui.text("Load a save or start a game.")
    return
  end
  draw_buttons()
  if ui.collapsing_header("Character", true) then draw_rows(CHARACTER) end
  if ui.collapsing_header("Condition") then
    draw_rows(CONDITION)
    draw_flags()
  end
  if ui.collapsing_header("Combat ratings") then
    draw_hold()
    draw_rows(RATINGS)
  end
  if status ~= "" then ui.text_wrapped(status) end
end

-- Keeps the held ratings where the game would move them.
function M.update()
  if held == nil then return end
  for field, value in pairs(held) do
    if kf.player[field] ~= value then attempt(set_player, field, value) end
  end
end

function M.between_updates()
  local jobs = queue
  queue = {}
  for _, job in ipairs(jobs) do attempt(job) end
end

return M
