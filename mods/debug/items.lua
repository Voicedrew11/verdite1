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

local function draw_rows(list)
  for _, entry in ipairs(list) do
    local count = kf.items.count(entry.id)
    local new = clamp(ui.input_int(entry.name .. "##" .. entry.id, count), 0, MAX_STACK)
    if new ~= count then attempt(set_count, entry.id, count, new) end
    if entry.description then ui.tooltip(entry.description) end
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

local function draw_slot(slot, label)
  local choices = slot_choices(slot)
  local worn = kf.equipped(slot)
  local names = { "(none)" }
  local index = 1
  for i, entry in ipairs(choices) do
    names[i + 1] = entry.name
    if entry.id == worn then index = i + 1 end
  end
  local new = ui.combo(label, index, names)
  if new == index then return end
  local item = nil
  if new > 1 then item = choices[new - 1].id end
  attempt(kf.equip, slot, item)
end

local function draw_equipment()
  if not ui.collapsing_header("Equipment", true) then return end
  for _, row in ipairs(SLOT_ROWS) do
    draw_slot(row[1], row[2])
  end
end

function M.draw()
  if kf.context() == "opening" then
    ui.text("Load a save or start a game.")
    return
  end
  catalog = catalog or build_catalog()

  search = ui.input_text("Search", search)
  held_only = ui.checkbox("Held only", held_only)
  if ui.button("Give one of each") then attempt(give_one_of_each) end
  ui.tooltip("Gives one of every item you do not already hold, except the gold coin.")
  ui.same_line()
  if ui.button("Clear all") then attempt(clear_all) end
  ui.tooltip("Takes every item out of the pack. It does not take off what is worn.")

  if ui.begin_scroll("items", 320) then draw_rows(visible_items()) end
  ui.end_scroll()

  draw_equipment()
  if status ~= "" then ui.text_wrapped(status) end
end

return M
