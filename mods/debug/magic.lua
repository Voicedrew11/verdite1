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

local function draw_spell(w, spell, selected)
  local learned = kf.magic.learned(spell)
  w:toggle(SPELLS[spell + 1], learned, function(on) attempt(kf.magic.set_learned, spell, on) end)
  if not learned then return end
  if selected == spell then
    w:text("selected", "dim")
    return
  end
  w:button("Select " .. SPELLS[spell + 1], function() attempt(kf.magic.select, spell) end)
end

function M.draw(w)
  if kf.context() == "opening" then
    w:text("Load a save or start a game.")
    return
  end
  w:help("The game's spell menu lists only spells 4 to 8, Lightning Bolt to Light Needle.")
  w:line()
  w:button("Learn all", function() attempt(set_all, true) end)
  w:button("Forget all", function() attempt(set_all, false) end)
  w:button("Deselect", function() attempt(kf.magic.select, nil) end)
  w:line_end()

  local selected = kf.magic.selected()
  for spell = 0, #SPELLS - 1 do
    draw_spell(w, spell, selected)
  end
  if status ~= "" then w:text(status, "warning") end
end

return M
