-- Items tab: the pack by name, and what is worn in each slot.
local M = { title = "Items" }

local GOLD_COIN = 39
local MAX_STACK = 99
local LAST_ID = 254

local SLOT_ROWS = {
  { "weapon", "Weapon" },
  { "head", "Head" },
  { "body", "Body" },
  { "shield", "Shield" },
  { "arm", "Arm" },
  { "leg", "Leg" },
  { "accessory", "Accessory" },
}

local status = ""
local catalog = nil   -- { id, name, slot, description, lower }, built on first draw
local search = ""
local held_only = false

local function attempt(fn, ...)
  local ok, err = pcall(fn, ...)
  if not ok then status = tostring(err) end
  return ok
end

local function build_catalog()
  local list = {}
  for id = 0, LAST_ID do
    local name = kf.items.name(id)
    if name then
      list[#list + 1] = {
        id = id,
        name = name,
        slot = kf.items.slot(id),
        description = kf.items.description(id),
        lower = name:lower(),
      }
    end
  end
  return list
end

local function clamp(value, low, high)
  return math.max(low, math.min(high, value))
end

-- Moves the pack from `from` to `to` by the difference.
local function set_count(id, from, to)
  if to > from then
    kf.items.give(id, to - from)
  else
    kf.items.take(id, from - to)
  end
end

local function give_one_of_each()
  for _, entry in ipairs(catalog) do
    if entry.id ~= GOLD_COIN and kf.items.count(entry.id) == 0 then
      kf.items.give(entry.id, 1)
    end
  end
end

local function clear_all()
  for _, entry in ipairs(catalog) do
    local count = kf.items.count(entry.id)
    if count > 0 then kf.items.take(entry.id, count) end
  end
end

local function visible_items()
  local needle = search:lower()
  local list = {}
  for _, entry in ipairs(catalog) do
    local shown = needle == "" or entry.lower:find(needle, 1, true) ~= nil
    if held_only and kf.items.count(entry.id) == 0 then shown = false end
    if shown then list[#list + 1] = entry end
  end
  return list
end

local function draw_rows(w, list)
  for _, entry in ipairs(list) do
    local count = kf.items.count(entry.id)
    w:stepper(entry.name, count, {1, 10}, function(new) attempt(set_count, entry.id, count, new) end, 0, MAX_STACK)
  end
end

-- The choices for a slot: what is carried, plus what is worn now.
local function slot_choices(slot)
  local worn = kf.equipped(slot)
  local choices = {}
  for _, entry in ipairs(catalog) do
    if entry.slot == slot and (kf.items.count(entry.id) > 0 or entry.id == worn) then
      choices[#choices + 1] = entry
    end
  end
  return choices
end

local function draw_slot(w, slot, label)
  local choices = slot_choices(slot)
  local worn = kf.equipped(slot)
  local names = { "(none)" }
  local index = 1
  for i, entry in ipairs(choices) do
    names[i + 1] = entry.name
    if entry.id == worn then index = i + 1 end
  end
  w:choice(label, names, index, function(new)
    local item = nil
    if new > 1 then item = choices[new - 1].id end
    attempt(kf.equip, slot, item)
  end)
end

function M.draw(w)
  if kf.context() == "opening" then
    w:text("Load a save or start a game.")
    return
  end
  catalog = catalog or build_catalog()

  w:heading("Equipment")
  for _, row in ipairs(SLOT_ROWS) do
    draw_slot(w, row[1], row[2])
  end
  w:heading("Pack")
  w:field("search", "Search", search, function(text) search = text end)
  w:toggle("Held only", held_only, function(on) held_only = on end)
  w:line()
  w:button("Give one of each", function() attempt(give_one_of_each) end)
  w:button("Clear all", function() attempt(clear_all) end)
  w:line_end()
  w:help("Give one of each: every item you do not hold, but the gold coin. Clear all leaves what is worn.")
  if status ~= "" then w:text(status, "warning") end
  draw_rows(w, visible_items())
end

return M
