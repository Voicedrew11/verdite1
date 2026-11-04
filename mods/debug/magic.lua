-- Magic tab: which spells are learned, and which one is selected.
local M = { title = "Magic" }

local SPELLS = {
  "Healing", "Dispoison", "Resist Fire", "Bless", "Lightning Bolt",
  "Fire Ball", "Fire Wall", "Wind Cutter", "Light Needle",
}
M.spell_names = SPELLS

local status = ""

local function attempt(fn, ...)
  local ok, err = pcall(fn, ...)
  if not ok then status = tostring(err) end
  return ok
end

local function set_all(learned)
  for spell = 0, #SPELLS - 1 do
    kf.magic.set_learned(spell, learned)
  end
end

local function draw_spell(spell, selected)
  local learned = kf.magic.learned(spell)
  local now = ui.checkbox(SPELLS[spell + 1], learned)
  if now ~= learned then attempt(kf.magic.set_learned, spell, now) end
  ui.same_line()
  if not learned then
    ui.text_colored(0.5, 0.5, 0.5, "not learned")
    return
  end
  if selected == spell then
    ui.text("selected")
    return
  end
  if ui.button("Select##" .. spell) then attempt(kf.magic.select, spell) end
end

function M.draw()
  if kf.context() == "opening" then
    ui.text("Load a save or start a game.")
    return
  end
  ui.text_wrapped("The game's spell menu lists only spells 4 to 8, Lightning Bolt to Light Needle.")
  if ui.button("Learn all") then attempt(set_all, true) end
  ui.same_line()
  if ui.button("Forget all") then attempt(set_all, false) end
  ui.same_line()
  if ui.button("Deselect") then attempt(kf.magic.select, nil) end

  local selected = kf.magic.selected()
  for spell = 0, #SPELLS - 1 do
    draw_spell(spell, selected)
  end
  if status ~= "" then ui.text_wrapped(status) end
end

return M
