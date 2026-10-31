-- Quick Heal: example of hotkeys and writing player state.
local keys = {"F5", "F6", "F7", "F8", "H"}
config.key = config.key or 1

function on_load()
  kf.log("press " .. keys[config.key] .. " to heal")
end

function on_update()
  if kf.context() ~= "gameplay" then return end
  if kf.key_pressed(keys[config.key]) then
    local p = kf.player
    p.hp = p.max_hp
    p.mp = p.max_mp
    kf.log("healed to " .. p.hp .. " HP, " .. p.mp .. " MP")
  end
end

function on_settings()
  config.key = ui.combo("Key", config.key, keys)
end
