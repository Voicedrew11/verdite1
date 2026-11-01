-- Bestiary: the nearest monsters on the floor in a window, and two buttons in Settings: one spawns
-- the floor's first kind two cells in front of the player, one gives a Medicinal Herb. The
-- kf.actors calls need gameplay or a menu with a floor loaded, so both places check first.

local MEDICINAL_HERB = 43   -- the item id of the Medicinal Herb
local NEAREST = 20          -- monsters listed in the window

local function in_play()
  local context = kf.context()
  return context == "gameplay" or context == "menu"
end

-- Distance from the player to an actor, in cells (2000 world units to a cell).
local function cells_from_player(actor)
  local dx = (actor.x - kf.player.x) / 2000
  local dz = (actor.z - kf.player.z) / 2000
  return math.sqrt(dx * dx + dz * dz)
end

-- A world position two cells ahead of the player, along the facing (yaw 0 is +z).
local function two_cells_ahead()
  local a = kf.player.yaw * 2 * math.pi / 4096
  -- Positions are integers: a fraction is refused.
  return math.floor(kf.player.x - math.sin(a) * 4000), math.floor(kf.player.z + math.cos(a) * 4000)
end

local function spawn_in_front()
  local kinds = kf.actors.kinds()
  if #kinds == 0 then return end
  local x, z = two_cells_ahead()
  local ok, result = pcall(kf.actors.spawn, kinds[1].kind, x, z)
  if not ok then
    mod.log(result)
  elseif result then
    mod.log("spawned kind", kinds[1].kind, "in slot", result)
  else
    mod.log("no free slot for a monster")
  end
end

function on_draw()
  if not in_play() then return end
  if ui.begin_window("Bestiary") then
    local list = kf.actors.list()
    table.sort(list, function(a, b) return cells_from_player(a) < cells_from_player(b) end)
    ui.text(string.format("%d monsters listed on this floor", #list))
    for i = 1, math.min(#list, NEAREST) do
      local a = list[i]
      ui.text(string.format("kind %d  hp %d  %s  %.1f cells", a.kind, a.hp, a.action, cells_from_player(a)))
    end
  end
  ui.end_window()
end

function on_settings()
  if not in_play() then
    ui.text("Load a game to use these.")
    return
  end
  if ui.button("Spawn one") then spawn_in_front() end
  if ui.button("Give a Medicinal Herb") then kf.items.give(MEDICINAL_HERB, 1) end
end
