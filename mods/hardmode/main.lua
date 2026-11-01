-- Hard Mode: every floor's monsters have more HP and the shops charge more, by the factors in
-- Settings. kf.rules lays the values over the game's own; they come off when the mod is disabled.
-- The game's own numbers come from kf.rules.original_*, so scaling twice never compounds.

config.monster_hp = config.monster_hp or 2.0
config.prices = config.prices or 1.5

local KINDS = 12        -- monster kinds on a floor (definition ids 0 to 11)
local ITEMS = 80        -- item ids 0 to 79
local SHOPS = { 1, 2 }  -- the shops' stock banks

local floors_done = {}  -- the floors whose monsters are set
local prices_done = false

local function scaled(value, factor)
  return math.min(65535, math.floor(value * factor + 0.5))
end

-- A floor's own numbers are known only while it is loaded, so each floor is set as it loads.
local function set_floor(floor)
  if floor < 1 or floor > 5 or floors_done[floor] or not kf.rules.original_monster(floor, 0) then return end
  for kind = 0, KINDS - 1 do
    local original = kf.rules.original_monster(floor, kind)
    if original.hp > 0 then
      kf.rules.set_monster(floor, kind, { hp = scaled(original.hp, config.monster_hp) })
    end
  end
  floors_done[floor] = true
end

-- The prices load with the game; before that original_price is nil. A price of 0 is left alone.
local function set_prices()
  if prices_done or not kf.rules.original_price(0, SHOPS[1]) then return end
  for item = 0, ITEMS - 1 do
    for _, shop in ipairs(SHOPS) do
      local original = kf.rules.original_price(item, shop)
      if original.buy > 0 then
        kf.rules.set_price(item, shop, { buy = scaled(original.buy, config.prices) })
      end
    end
  end
  prices_done = true
end

local function set_all()
  set_floor(kf.player.floor)
  set_prices()
end

function on_map_load(map) set_floor(map.floor) end
function on_update() set_all() end

function on_settings()
  local hp = ui.slider_float("Monster HP", config.monster_hp, 1, 4, "x%.2f")
  local prices = ui.slider_float("Shop prices", config.prices, 1, 4, "x%.2f")
  if hp == config.monster_hp and prices == config.prices then return end
  config.monster_hp, config.prices = hp, prices
  kf.rules.clear()
  floors_done, prices_done = {}, false
  set_all()
end
