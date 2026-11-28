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

local function draw_buttons(w)
  w:line()
  w:button("Full heal", function() attempt(full_heal) end)
  w:button("Cure", function() attempt(cure) end)
  w:button("Level up", function() queue[#queue + 1] = level_up end)
  w:button("Recalculate", function() queue[#queue + 1] = recalculate end)
  w:line_end()
  w:help("Cure clears the conditions and their timers. Level up gives the experience for the next level, "
    .. "so the game grants its gains. Recalculate has the game work out the combat ratings again.")
end

-- Steps one row's value, and keeps it while the ratings are held.
local function draw_rows(w, rows)
  for _, row in ipairs(rows) do
    local field = row[1]
    w:stepper(row[2], kf.player[field], {1, 10, 100}, function(new)
      if attempt(set_player, field, new) and held and held[field] ~= nil then held[field] = new end
    end, 0)
  end
end

local function draw_flags(w)
  local flags = kf.player.status_flags
  for _, row in ipairs(FLAGS) do
    local bit = row[1]
    w:toggle(row[2], (flags & bit) ~= 0, function(on)
      attempt(set_player, "status_flags", on and (flags | bit) or (flags & ~bit))
    end)
  end
end

local function draw_hold(w)
  w:toggle("Hold these values", held ~= nil, function(on) held = on and hold_ratings() or nil end)
  w:help("The game recalculates the combat ratings. While this is on, they are kept at the values you set.")
end

function M.draw(w)
  if kf.context() == "opening" then
    w:text("Load a save or start a game.")
    return
  end
  draw_buttons(w)
  w:heading("Character")
  draw_rows(w, CHARACTER)
  w:heading("Condition")
  draw_rows(w, CONDITION)
  draw_flags(w)
  w:heading("Combat ratings")
  draw_hold(w)
  draw_rows(w, RATINGS)
  if status ~= "" then w:text(status, "warning") end
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
