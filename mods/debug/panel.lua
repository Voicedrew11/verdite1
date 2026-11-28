-- The panel's open tab, written as RML each frame. A tab's draw(w) calls the calls below; a press
-- comes back through on_ui_action as the number of the control pressed, and runs what that
-- control was given. ui.set_rml rebuilds nothing while the RML is the same, so a field keeps its
-- typing until something it shows changes.
local panel = {}
panel.__index = panel

local function escaped(text)
  return (tostring(text):gsub("[&<>\"]", { ["&"] = "&amp;", ["<"] = "&lt;", [">"] = "&gt;", ['"'] = "&quot;" }))
end
panel.escaped = escaped

function panel.new()
  return setmetatable({ parts = {}, actions = {}, fields = {} }, panel)
end

function panel:add(rml)
  self.parts[#self.parts + 1] = rml
end

-- A control: pressing it runs fn.
function panel:action(fn)
  self.actions[#self.actions + 1] = fn
  return #self.actions
end

function panel:button(label, fn)
  self:add(string.format('<button data-action="press" data-value="%d">%s</button>', self:action(fn), escaped(label)))
end

function panel:heading(text)
  self:add("<h2>" .. escaped(text) .. "</h2>")
end

function panel:text(text, class)
  self:add(string.format('<p class="%s">%s</p>', class or "", escaped(text)))
end

-- What a row explains under it, dim.
function panel:help(text)
  self:text(text, "help")
end

-- Controls side by side until line_end.
function panel:line()
  self:add('<div class="line">')
end

function panel:line_end()
  self:add("</div>")
end

-- A switch: its label and On or Off; pressing it gives fn the other value.
function panel:toggle(label, on, fn)
  local id = self:action(function() fn(not on) end)
  self:add(string.format('<div class="row" data-action="press" data-value="%d"><span class="label">%s</span>'
    .. '<span class="value%s">%s</span></div>', id, escaped(label), on and " on" or "", on and "On" or "Off"))
end

-- A number with buttons that step it by each of steps, down and up; fn gets the new value, held
-- to min and max when given.
function panel:stepper(label, value, steps, fn, min, max)
  local function held(new)
    if min then new = math.max(min, new) end
    if max then new = math.min(max, new) end
    return new
  end
  self:add('<div class="row"><span class="label">' .. escaped(label) .. "</span>")
  for i = #steps, 1, -1 do
    local step = steps[i]
    self:add(string.format('<button class="step" data-action="press" data-value="%d">-%s</button>',
      self:action(function() fn(held(value - step)) end), escaped(step)))
  end
  self:add('<span class="value">' .. escaped(value) .. "</span>")
  for _, step in ipairs(steps) do
    self:add(string.format('<button class="step" data-action="press" data-value="%d">+%s</button>',
      self:action(function() fn(held(value + step)) end), escaped(step)))
  end
  self:add("</div>")
end

-- One of names, with arrows either side; fn gets the new index (1-based).
function panel:choice(label, names, index, fn)
  local before = self:action(function() if index > 1 then fn(index - 1) end end)
  local after = self:action(function() if index < #names then fn(index + 1) end end)
  self:add(string.format('<div class="row"><span class="label">%s</span>'
    .. '<button class="step" data-action="press" data-value="%d">&lt;</button><span class="value">%s</span>'
    .. '<button class="step" data-action="press" data-value="%d">&gt;</button></div>',
    escaped(label), before, escaped(names[index] or ""), after))
end

-- A text field holding text; Enter gives fn what it holds. name tells it from the tab's other fields.
function panel:field(name, label, text, fn)
  self.fields[name] = fn
  self:add(string.format('<div class="row"><span class="label">%s</span><input type="text" id="field-%s" '
    .. 'value="%s" data-enter="field" data-value="%s"/></div>', escaped(label), name, escaped(text), name))
end

function panel:rml()
  return table.concat(self.parts)
end

-- A press from on_ui_action; true when it was one of this panel's.
function panel:press(event, page)
  if event.name == "press" then
    local fn = self.actions[tonumber(event.value)]
    if fn then fn() return true end
  elseif event.name == "field" then
    local fn = self.fields[event.value]
    if fn then fn(ui.field(page, "field-" .. event.value)) return true end
  end
  return false
end

return panel
