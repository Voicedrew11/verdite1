-- Kill count: example of on_kill and the save table. save is kept with each save slot and replaced
-- when a save loads, so it is read each time rather than kept in a local.

local function kills()
  save.kills = save.kills or {}
  return save.kills
end

-- Only the player's own kills count; by_player is false for a hazard or another actor's blow.
function on_kill(kill)
  if not kill.by_player then return end
  local kind = tostring(kill.kind)
  kills()[kind] = (kills()[kind] or 0) + 1
end

function on_draw()
  if kf.context() == "opening" then return end
  local total = 0
  for _, count in pairs(save.kills or {}) do total = total + count end
  local vx, vy, _, vh = kf.view()
  ui.draw_text(vx + 16, vy + vh - 40, string.format("Kills %d", total), 1, 0.9, 0.6)
end
