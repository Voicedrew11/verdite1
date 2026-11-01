-- Map: a north-up minimap in a corner and the full floor map on the full_map action (M), drawn from
-- kf.automap. The cells are read into a cache a few rows each frame, so a door that opens shows a
-- few frames later, and the floor's shapes are rebuilt only when the cache changes.

local columns, rows = 100, 100
local band_rows = 10   -- the floor's shapes are kept in bands of rows, rebuilt one band at a time
local band_count = rows // band_rows

local defaults = {
  minimap = true,
  corner = 1,          -- 0 top right, 1 bottom right, 2 top left, 3 bottom left
  size = 28,           -- percent of the picture's height
  zoom = 8,            -- cells from the centre to the edge
  opacity = 85,        -- percent
  fog = true,          -- only what has been seen
  pause = true,        -- the full map pauses the game
  keep_music = true,   -- ...and the music plays on
  show_items = false,  -- cheats
  show_chests = false,
  show_enemies = false,
  key_hints = false,
}
for key, value in pairs(defaults) do
  if type(config[key]) ~= type(value) then config[key] = value end
end

-- ---------- the floor, cached ----------
-- cell, hazard and seen by z * columns + x. A change marks the bands whose shapes it touches: its
-- own row's, and its neighbours' (their walls face it).
local cache = { floor = 0, next_row = 0, cell = {}, hazard = {}, seen = {}, dirty = {} }

local function mark_rows(z)
  for row = math.max(z - 1, 0), math.min(z + 1, rows - 1) do cache.dirty[row // band_rows] = true end
end

local function read_row(z)
  local cell, hazard, seen = cache.cell, cache.hazard, cache.seen
  local automap = kf.automap
  local base = z * columns
  local changed = false
  for x = 0, columns - 1 do
    local i = base + x
    local c, h, s = automap.cell(x, z), automap.hazard(x, z), automap.seen(x, z)
    if c ~= cell[i] or h ~= hazard[i] or s ~= seen[i] then
      cell[i], hazard[i], seen[i] = c, h, s
      changed = true
    end
  end
  if changed then mark_rows(z) end
end

-- A new floor is read whole; otherwise a few rows a frame, round and round.
local function refresh_cache()
  local floor = kf.automap.floor
  if floor ~= cache.floor then
    cache.floor = floor
    for z = 0, rows - 1 do read_row(z) end
    cache.next_row = 0
    return
  end
  for _ = 1, band_rows do
    read_row(cache.next_row)
    cache.next_row = (cache.next_row + 1) % rows
  end
end

local function cell_shown(x, z)
  if x < 0 or z < 0 or x >= columns or z >= rows then return false end
  return not config.fog or kf.automap.owns_map or cache.seen[z * columns + x]
end


-- ---------- the floor's shapes, in cells ----------
-- Colours by name, 0..255 and alpha.
local palette = {
  floor = { 78, 70, 56, 0.9 }, pit = { 120, 60, 40, 0.9 }, poison = { 60, 110, 50, 0.9 },
  bottomless = { 20, 16, 14, 0.95 }, wall = { 215, 200, 160, 1 }, door = { 190, 120, 50, 1 },
  chasm = { 46, 46, 50, 0.92 }, hatch = { 78, 78, 84, 0.9 }, harp = { 70, 170, 190, 0.85 },
  raised = { 60, 140, 160, 0.95 },
}

-- A cell's fill by its kind; the rest are their hazard's, or floor.
local cell_fill = { door = "door", chasm = "chasm", harp_bridge = "chasm", harp_bridge_raised = "raised" }

-- A half cell's walkable triangle, as corners of the cell (0..1), and the diagonal that walls it.
local halves = {
  half_x_ge_z = { { 0, 0, 1, 0, 1, 1 }, { 0, 0, 1, 1 } },
  half_sum_le = { { 0, 0, 1, 0, 0, 1 }, { 1, 0, 0, 1 } },
  half_z_ge_x = { { 0, 0, 1, 1, 0, 1 }, { 0, 0, 1, 1 } },
  half_sum_ge = { { 1, 0, 1, 1, 0, 1 }, { 1, 0, 0, 1 } },
}

-- The sides a half cell owns: the two its triangle touches.
local not_south = { half_z_ge_x = true, half_sum_ge = true }
local not_north = { half_x_ge_z = true, half_sum_le = true }
local not_west = { half_x_ge_z = true, half_sum_ge = true }
local not_east = { half_z_ge_x = true, half_sum_le = true }

-- Each band's shapes: rebuilt when its cells change, and all of them when the fog or the map in the
-- pack does. Rows of one fill are one rectangle and runs of wall one line, so a floor is a few
-- thousand shapes at most, not ten thousand cells.
local geo = { fog = nil, owns_map = nil, bands = {} }

local function add_cell_shapes(x, z, cell, fill, polys, lines)
  local half = halves[cell]
  if half then
    local t = half[1]
    polys[#polys + 1] = { { x + t[1], z + t[2], x + t[3], z + t[4], x + t[5], z + t[6] }, fill, true }
    local d = half[2]
    lines[#lines + 1] = { x + d[1], z + d[2], x + d[3], z + d[4], "wall", 0.14 }
  elseif cell == "door_open" then
    polys[#polys + 1] = { { x, z, x + 1, z, x + 1, z + 1, x, z + 1 }, "door", false, 0.1 }
  elseif cell == "chasm" or cell == "harp_bridge" then
    lines[#lines + 1] = { x, z, x + 1, z + 1, "hatch", 0.08, true }
    if cell == "harp_bridge" then
      local a, b = 0.12, 0.88
      polys[#polys + 1] = { { x + a, z + a, x + b, z + a, x + b, z + b, x + a, z + b }, "harp", false, 0.1 }
    end
  end
end

-- The cells drawn (walkable and shown) in rows z0..z1, by index, and their bounds (max_x -1 when none).
local function shown_cells(z0, z1)
  local cells, seen = cache.cell, cache.seen
  local all = not config.fog or kf.automap.owns_map
  local shown = {}
  local min_x, min_z, max_x, max_z = columns, rows, -1, -1
  for z = z0, z1 do
    local base = z * columns
    for x = 0, columns - 1 do
      local i = base + x
      local cell = cells[i]
      if cell ~= "void" and cell ~= "wall" and (all or seen[i]) then
        shown[i] = true
        if x < min_x then min_x = x end
        if x > max_x then max_x = x end
        if z < min_z then min_z = z end
        if z > max_z then max_z = z end
      end
    end
  end
  return shown, { min_x, min_z, max_x, max_z }
end

local function build_fills(shown, bounds, rects, polys, lines)
  local cells, hazards = cache.cell, cache.hazard
  for z = bounds[2], bounds[4] do
    local base = z * columns
    local run_start, run_fill = 0, nil
    for x = bounds[1], bounds[3] + 1 do   -- one past the bounds, to end the last run
      local fill = nil
      local i = base + x
      if x <= bounds[3] and shown[i] then
        local cell = cells[i]
        fill = cell_fill[cell] or (hazards[i] ~= "none" and hazards[i]) or "floor"
        if cell ~= "floor" then add_cell_shapes(x, z, cell, fill, polys, lines) end
        if halves[cell] then fill = nil end   -- its triangle is its fill
      end
      if fill ~= run_fill then
        if run_fill then rects[#rects + 1] = { run_start, z, x, z + 1, run_fill } end
        run_start, run_fill = x, fill
      end
    end
  end
end

-- Whether a shown cell is walled on a side: the cell owns the side and its neighbour is solid.
local sides = {
  south = { not_south, 0, -1 }, north = { not_north, 0, 1 }, west = { not_west, -1, 0 }, east = { not_east, 1, 0 },
}

local function walled(shown, side, x, z)
  local i = z * columns + x
  if not shown[i] then return false end
  local cell = cache.cell[i]
  local rule = sides[side]
  if rule[1][cell] then return false end
  local nx, nz = x + rule[2], z + rule[3]
  if nx < 0 or nz < 0 or nx >= columns or nz >= rows then return true end
  local neighbour = cache.cell[nz * columns + nx]
  return neighbour == "void" or neighbour == "wall"
end

-- Runs of one side's walls along a row (south, north) or a column (west, east).
local function build_walls(shown, bounds, lines)
  for z = bounds[2], bounds[4] do
    for _, side in ipairs({ "south", "north" }) do
      local edge = side == "south" and z or z + 1
      local start = nil
      for x = bounds[1], bounds[3] + 1 do
        local on = x <= bounds[3] and walled(shown, side, x, z)
        if on and not start then start = x end
        if not on and start then
          lines[#lines + 1] = { start, edge, x, edge, "wall", 0.14 }
          start = nil
        end
      end
    end
  end
  for x = bounds[1], bounds[3] do
    for _, side in ipairs({ "west", "east" }) do
      local edge = side == "west" and x or x + 1
      local start = nil
      for z = bounds[2], bounds[4] + 1 do
        local on = z <= bounds[4] and walled(shown, side, x, z)
        if on and not start then start = z end
        if not on and start then
          lines[#lines + 1] = { edge, start, edge, z, "wall", 0.14 }
          start = nil
        end
      end
    end
  end
end

local function build_band(b)
  local shown, bounds = shown_cells(b * band_rows, (b + 1) * band_rows - 1)
  local band = { rects = {}, polys = {}, lines = {}, bounds = bounds }
  if bounds[3] >= 0 then
    build_fills(shown, bounds, band.rects, band.polys, band.lines)
    build_walls(shown, bounds, band.lines)
  end
  geo.bands[b] = band
end

local function refresh_geometry()
  local owns_map = kf.automap.owns_map
  if geo.fog ~= config.fog or geo.owns_map ~= owns_map then
    geo.fog, geo.owns_map = config.fog, owns_map
    for b = 0, band_count - 1 do cache.dirty[b] = true end
  end
  for b in pairs(cache.dirty) do build_band(b) end
  cache.dirty = {}
end

-- What is shown of the floor: {min_x, min_z, max_x, max_z}, max_x -1 when nothing is.
local function shown_bounds()
  local min_x, min_z, max_x, max_z = columns, rows, -1, -1
  for b = 0, band_count - 1 do
    local band = geo.bands[b]
    if band and band.bounds[3] >= 0 then
      local o = band.bounds
      min_x, min_z, max_x, max_z = math.min(min_x, o[1]), math.min(min_z, o[2]), math.max(max_x, o[3]), math.max(max_z, o[4])
    end
  end
  return { min_x, min_z, max_x, max_z }
end

-- ---------- a view: map cells to window pixels ----------
-- origin_x, origin_y: where (centre_x, centre_z) is drawn; scale: pixels per cell; north is up.
local function at(view, x, z)
  return view.origin_x + (x - view.centre_x) * view.scale, view.origin_y - (z - view.centre_z) * view.scale
end

local function colour(view, r, g, b, a)
  return r / 255, g / 255, b / 255, (a or 1) * view.alpha
end

-- The palette at the view's opacity.
local function view_colours(view)
  local colours = {}
  for name, c in pairs(palette) do colours[name] = { c[1] / 255, c[2] / 255, c[3] / 255, c[4] * view.alpha } end
  return colours
end

local function overlaps(x0, z0, x1, z1, area)
  return x1 >= area[1] and x0 <= area[3] and z1 >= area[2] and z0 <= area[4]
end

local function draw_band(view, band, area, colours)
  local scale = view.scale
  for _, r in ipairs(band.rects) do
    if overlaps(r[1], r[2], r[3], r[4], area) then
      local x, y = at(view, r[1], r[4])
      local c = colours[r[5]]
      ui.draw_rect(x, y, (r[3] - r[1]) * scale, (r[4] - r[2]) * scale, c[1], c[2], c[3], c[4])
    end
  end
  for _, p in ipairs(band.polys) do
    local q = p[1]
    if overlaps(q[1], q[2], q[1] + 1, q[2] + 1, area) then
      local points = {}
      for i = 1, #q, 2 do points[i], points[i + 1] = at(view, q[i], q[i + 1]) end
      local c = colours[p[2]]
      ui.draw_polygon(points, c[1], c[2], c[3], c[4], p[3], math.max(1, scale * (p[4] or 0)))
    end
  end
  for _, l in ipairs(band.lines) do
    if (not l[7] or scale >= 5) and overlaps(math.min(l[1], l[3]), math.min(l[2], l[4]), math.max(l[1], l[3]), math.max(l[2], l[4]), area) then
      local x0, y0 = at(view, l[1], l[2])
      local x1, y1 = at(view, l[3], l[4])
      local c = colours[l[5]]
      ui.draw_line(x0, y0, x1, y1, c[1], c[2], c[3], c[4], math.max(1, scale * l[6]))
    end
  end
end

-- The floor's shapes that overlap area {min_x, min_z, max_x, max_z} (cells).
local function draw_cells(view, area)
  local colours = view_colours(view)
  for b = 0, band_count - 1 do
    local band = geo.bands[b]
    if band and band.bounds[3] >= 0 then
      local o = band.bounds
      if overlaps(o[1], o[2], o[3] + 1, o[4] + 1, area) then draw_band(view, band, area, colours) end
    end
  end
end

-- ---------- markers ----------
local cheat_setting = { item = "show_items", chest = "show_chests", enemy = "show_enemies" }

-- Visible, or for plain doors and Harp bridges hoverable: their cells are drawn instead of a symbol.
local function marker_known(marker)
  local cheat = cheat_setting[marker.kind]
  if cheat then return config[cheat] end
  return cell_shown(math.floor(marker.x), math.floor(marker.z))
end

local function marker_shown(marker)
  return marker.kind ~= "door" and marker.kind ~= "harp_bridge" and marker_known(marker)
end

local warp_teleport, warp_exit, unlocked = 10, 11, 255

local function draw_label(x, y, size, text, r, g, b, a)
  local w, h = ui.text_size(text, size)
  ui.draw_text(x - w / 2, y - h / 2, text, r, g, b, a, size)
end

local function square(x, y, half_w, half_h)
  return { x - half_w, y - half_h, x + half_w, y - half_h, x + half_w, y + half_h, x - half_w, y + half_h }
end

local marker_draw = {
  locked_door = function(view, x, y, r)
    ui.draw_rect(x - r * 0.6, y - r * 0.5, r * 1.2, r * 1.1, colour(view, 210, 60, 50))
    local cr, cg, cb, ca = colour(view, 210, 60, 50)
    ui.draw_circle(x, y - r * 0.5, r * 0.4, cr, cg, cb, ca, false, math.max(1, r * 0.2))
  end,
  warp = function(view, x, y, r, marker, labels)
    ui.draw_circle(x, y, r * 1.1, colour(view, 150, 90, 220))
    local or_, og, ob, oa = colour(view, 0, 0, 0, 0.8)
    ui.draw_circle(x, y, r * 1.1, or_, og, ob, oa, false, 1)
    if labels or r >= 4 then
      local text = marker.label == warp_teleport and "T" or marker.label == warp_exit and "X" or tostring(marker.label)
      draw_label(x, y, r * 1.7, text, colour(view, 255, 255, 255))
    end
  end,
  save_point = function(view, x, y, r)
    local diamond = { x, y - r, x + r, y, x, y + r, x - r, y }
    local fr, fg, fb, fa = colour(view, 80, 220, 230)
    ui.draw_polygon(diamond, fr, fg, fb, fa)
    local or_, og, ob, oa = colour(view, 0, 0, 0, 0.8)
    ui.draw_polygon(diamond, or_, og, ob, oa, false, 1)
  end,
  fountain = function(view, x, y, r, marker)
    local fr, fg, fb, fa = colour(view, 70, 140, 255)
    if marker.label ~= 0 then ui.draw_circle(x, y, r, fr, fg, fb, fa) end
    ui.draw_circle(x, y, r, fr, fg, fb, fa, false, math.max(1, r * 0.3))
  end,
  npc = function(view, x, y, r)
    ui.draw_circle(x, y, r * 0.8, colour(view, 120, 230, 120))
    local or_, og, ob, oa = colour(view, 0, 0, 0, 0.8)
    ui.draw_circle(x, y, r * 0.8, or_, og, ob, oa, false, 1)
  end,
  item = function(view, x, y, r)
    ui.draw_circle(x, y, r * 0.55, colour(view, 250, 220, 90))
  end,
  chest = function(view, x, y, r)
    local box = square(x, y, r * 0.75, r * 0.55)
    local fr, fg, fb, fa = colour(view, 230, 170, 60)
    ui.draw_polygon(box, fr, fg, fb, fa)
    local or_, og, ob, oa = colour(view, 0, 0, 0, 0.8)
    ui.draw_polygon(box, or_, og, ob, oa, false, 1)
  end,
  enemy = function(view, x, y, r, marker)
    ui.draw_circle(x, y, r * 0.7, colour(view, 235, 50, 50))
    if #marker.items > 0 then
      -- A gold ring: this enemy drops an item when it dies.
      local ring = math.max(1.5, r * 0.3)
      local gr, gg, gb, ga = colour(view, 255, 205, 60)
      ui.draw_circle(x, y, r * 0.7 + ring * 0.5, gr, gg, gb, ga, false, ring)
    else
      local or_, og, ob, oa = colour(view, 0, 0, 0, 0.8)
      ui.draw_circle(x, y, r * 0.7, or_, og, ob, oa, false, 1)
    end
  end,
}

local function draw_markers(view, markers, labels)
  -- The full map keeps markers legible even when a cell is only a few pixels.
  local r = math.max(labels and 5 or 2.5, view.scale * 0.42)
  for _, marker in ipairs(markers) do
    local draw = marker_draw[marker.kind]
    if draw and marker_shown(marker) then
      local x, y = at(view, marker.x, marker.z)
      draw(view, x, y, r, marker, labels)
    end
  end
end

local function draw_arrow(view, x, z, yaw, r, g, b)
  local px, py = at(view, x, z)
  -- Facing (-sin yaw, +cos yaw) in map x/z; screen y points south.
  local fx, fy = -math.sin(yaw), -math.cos(yaw)
  local sx, sy = math.cos(yaw), -math.sin(yaw)
  local size = math.max(5, view.scale * 0.9)
  local arrow = {
    px + fx * size, py + fy * size,
    px - fx * size * 0.7 + sx * size * 0.65, py - fy * size * 0.7 + sy * size * 0.65,
    px - fx * size * 0.45, py - fy * size * 0.45,
    px - fx * size * 0.7 - sx * size * 0.65, py - fy * size * 0.7 - sy * size * 0.65,
  }
  -- Concave at the back: drawn as two triangles.
  local fr, fg, fb, fa = colour(view, r, g, b)
  ui.draw_polygon({ arrow[1], arrow[2], arrow[3], arrow[4], arrow[5], arrow[6] }, fr, fg, fb, fa)
  ui.draw_polygon({ arrow[1], arrow[2], arrow[5], arrow[6], arrow[7], arrow[8] }, fr, fg, fb, fa)
  local or_, og, ob, oa = colour(view, 0, 0, 0)
  ui.draw_polygon(arrow, or_, og, ob, oa, false, 1.5)
end

local function draw_players(view)
  local floor = kf.automap.floor
  for _, other in ipairs(kf.online.players()) do
    if other.floor == floor then draw_arrow(view, other.x, other.z, other.yaw, 90, 200, 255) end
  end
  draw_arrow(view, kf.automap.player_x, kf.automap.player_z, kf.automap.player_yaw, 255, 250, 220)
end

-- ---------- what a marker says on hover ----------
-- People, by character id, as the Mirror of Truth's profiles name them in the English translation.
-- Those who never give a name stay "Person".
local people = {
  [1] = "Wilfred Light (merchant)", [2] = "Gill Baccas (merchant)", [3] = "Juedds Kross (priest)",
  [4] = "Basque Crize (soldier)", [5] = "Watts Virgil (soldier)", [6] = "Erik Blair (soldier)",
  [7] = "Heinz Ruhart (minstrel)", [8] = "Rally Kelvim (sage)", [9] = "Nicholas Oarman (grave digger)",
  [10] = "Forest Dragon", [11] = "Miria (the dragon's fairy)", [12] = "Randalf VIII (former king)",
  [13] = "Ozzy Ranvalgve (graveyard caretaker)", [14] = "Evil Dark Magician", [15] = "Evil Dark Magician",
  [16] = "Evil Dark Magician", [17] = "Reinhardt III (former king)",
}

local function item_name(id)
  return kf.items.name(id) or ("Item " .. id)
end

-- What opens a lock: a key (a link below 128 names the key's item id) or something else.
local function lock_hint(link)
  if link == unlocked then return nil end
  if link >= 128 then return "Opened by a switch" end
  local name = kf.items.name(link)
  if name and name:find("Key", 1, true) then return "Needs the " .. name end
  return "Opened by an event or mechanism elsewhere"
end

local describe = {
  door = function() return "Door" end,
  locked_door = function(marker)
    local hint = config.key_hints and lock_hint(marker.label)
    return hint and "Locked door\n" .. hint or "Locked door"
  end,
  warp = function(marker)
    if marker.label == warp_teleport then return "Teleporter (same floor)" end
    if marker.label == warp_exit then return "The way out" end
    return "Warp to floor " .. marker.label
  end,
  save_point = function() return "Save point" end,
  fountain = function(marker) return marker.label ~= 0 and "Fountain" or "Fountain (dry)" end,
  npc = function(marker) return people[marker.label] or "Person" end,
  item = function(marker)
    local id = marker.items[1]
    if not id then return "Item" end
    local effect = kf.items.description(id)
    return effect and item_name(id) .. "\n" .. effect or item_name(id)
  end,
  chest = function(marker)
    local text = marker.label ~= unlocked and "Locked chest" or "Chest"
    for _, id in ipairs(marker.items) do text = text .. "\n  " .. item_name(id) end
    local hint = config.key_hints and lock_hint(marker.label)
    if hint then text = text .. "\n" .. hint end
    return text
  end,
  enemy = function(marker)
    return marker.items[1] and "Enemy\nDrops: " .. item_name(marker.items[1]) or "Enemy"
  end,
  harp_bridge = function() return "Harp bridge\nPlay the Harp nearby to raise it for a short time" end,
}

-- Word-wraps each line of text to width pixels.
local function wrap(text, size, width)
  local out = {}
  for line in (text .. "\n"):gmatch("(.-)\n") do
    local indent = line:match("^(%s*)")
    local current = indent
    for word in line:gmatch("%S+") do
      local candidate = current == indent and current .. word or current .. " " .. word
      if current ~= indent and ui.text_size(candidate, size) > width then
        out[#out + 1] = current
        current = indent .. word
      else
        current = candidate
      end
    end
    out[#out + 1] = current
  end
  return table.concat(out, "\n")
end

-- A label beside the cursor for the marker under it.
local function draw_hover(view, markers, mx, my, size, game)
  local reach = math.max(8, view.scale * 0.6)
  local best, best_distance = nil, reach * reach
  for _, marker in ipairs(markers) do
    if marker_known(marker) then
      local x, y = at(view, marker.x, marker.z)
      local d = (x - mx) ^ 2 + (y - my) ^ 2
      if d < best_distance then best, best_distance = marker, d end
    end
  end
  if not best then return end
  local text = wrap(describe[best.kind](best), size, size * 16)
  local w, h = ui.text_size(text, size)
  local pad = size * 0.4
  local x, y = mx + size, my + size * 0.6
  if x + w + pad > game.x + game.w then x = mx - size - w - pad end
  if y + h + pad > game.y + game.h then y = my - h - pad * 2 end
  ui.draw_rect(x - pad, y - pad, w + pad * 2, h + pad * 2, 20 / 255, 17 / 255, 12 / 255, 235 / 255)
  ui.draw_polygon(square(x + w / 2, y + h / 2, w / 2 + pad, h / 2 + pad), 200 / 255, 180 / 255, 130 / 255, 1, false, 1)
  ui.draw_text(x, y, text, 240 / 255, 230 / 255, 210 / 255, 1, size)
end

-- ---------- the minimap ----------
local function draw_minimap(game, markers)
  local size = game.h * config.size / 100
  local margin = game.h * 0.025
  local left, top = game.x + game.w - size - margin, game.y + game.h - size - margin
  if config.corner == 0 or config.corner == 2 then top = game.y + margin end
  if config.corner >= 2 then left = game.x + margin end
  local view = {
    origin_x = left + size / 2, origin_y = top + size / 2,
    centre_x = kf.automap.player_x, centre_z = kf.automap.player_z,
    scale = size / (2 * config.zoom), alpha = config.opacity / 100,
  }
  ui.layer("background")
  ui.draw_rect(left, top, size, size, colour(view, 10, 9, 8, 0.75))
  ui.push_clip(left, top, size, size)
  local radius = config.zoom + 1
  local cx, cz = kf.automap.player_x, kf.automap.player_z
  draw_cells(view, { cx - radius, cz - radius, cx + radius, cz + radius })
  draw_markers(view, markers, false)
  draw_players(view)
  ui.pop_clip()
  local br, bg, bb, ba = colour(view, 200, 180, 130, 0.8)
  ui.draw_polygon(square(left + size / 2, top + size / 2, size / 2, size / 2), br, bg, bb, ba, false, 1.5)
  draw_label(left + size / 2, top + size * 0.06, math.max(12, size * 0.07), "N", colour(view, 230, 60, 50))
  ui.layer("foreground")
end

-- ---------- the full map ----------
-- Zoom 1 fits what has been seen; dragging moves the centre (cells).
local full = { open = false, zoom = 1, centre_x = 0, centre_z = 0, centre_set = false, last_x = nil, last_y = nil, right_down = false }

-- The pause, the music and the mouse follow the full map.
local function apply_to_game()
  kf.game.paused = full.open and config.pause
  kf.game.music_while_paused = full.open and config.keep_music
  kf.game.mouse = full.open
end

local function set_open(open)
  full.open = open
  full.zoom, full.centre_set = 1, false
  apply_to_game()
end

local legend = {
  { { 150, 90, 220 }, "Warp (number = floor)" },
  { { 80, 220, 230 }, "Save point" },
  { { 70, 140, 255 }, "Fountain" },
  { { 190, 120, 50 }, "Door", true },
  { { 210, 60, 50 }, "Locked door", true },
  { { 120, 230, 120 }, "Person" },
  { { 120, 60, 40 }, "Pit / trap", true },
  { { 60, 110, 50 }, "Poison", true },
  { { 70, 70, 76 }, "Chasm (fatal drop)", true },
  { { 60, 140, 160 }, "Harp bridge", true },
  { { 250, 220, 90 }, "Item", false, "show_items" },
  { { 230, 170, 60 }, "Chest", true, "show_chests" },
  { { 235, 50, 50 }, "Enemy", false, "show_enemies" },
  { { 255, 205, 60 }, "Enemy with a drop", false, "show_enemies" },
}

local function draw_legend(x, y, size)
  for _, entry in ipairs(legend) do
    if not entry[4] or config[entry[4]] then
      local c = entry[1]
      local r, g, b = c[1] / 255, c[2] / 255, c[3] / 255
      local half = size * 0.4
      if entry[3] then
        ui.draw_rect(x, y + size * 0.1, half * 2, half * 2, r, g, b, 1)
      else
        ui.draw_circle(x + half, y + size * 0.5, half, r, g, b, 1)
      end
      ui.draw_text(x + size * 1.3, y, entry[2], 230 / 255, 220 / 255, 200 / 255, 1, size)
      y = y + size * 1.4
    end
  end
  return y
end

-- The seen part of the floor and the player, with a little room round them.
local function seen_bounds()
  local b = shown_bounds()
  local px, pz = math.floor(kf.automap.player_x), math.floor(kf.automap.player_z)
  return math.min(b[1], px) - 2, math.min(b[2], pz) - 2, math.max(b[3], px) + 3, math.max(b[4], pz) + 3
end

-- Wheel zooms towards the cursor, left drag moves, right click resets.
local function handle_mouse(view, area, fit_scale, bounds)
  local mx, my = ui.mouse()
  local inside = mx >= area.x and mx < area.x + area.w and my >= area.y and my < area.y + area.h
  local wheel = ui.mouse_wheel()
  if inside and wheel ~= 0 then
    local world_x = view.centre_x + (mx - view.origin_x) / view.scale
    local world_z = view.centre_z - (my - view.origin_y) / view.scale
    full.zoom = math.max(1, math.min(16, full.zoom * 1.25 ^ wheel))
    view.scale = fit_scale * full.zoom
    full.centre_x = world_x - (mx - view.origin_x) / view.scale
    full.centre_z = world_z + (my - view.origin_y) / view.scale
    full.centre_set = true
  end
  if inside and ui.mouse_down(1) and full.last_x then
    full.centre_x = full.centre_x - (mx - full.last_x) / view.scale
    full.centre_z = full.centre_z + (my - full.last_y) / view.scale
    full.centre_set = true
  end
  full.last_x, full.last_y = mx, my
  local right = ui.mouse_down(2)
  if inside and right and not full.right_down then
    full.zoom, full.centre_set = 1, false
    full.centre_x, full.centre_z = (bounds[1] + bounds[3]) / 2, (bounds[2] + bounds[4]) / 2
    view.scale = fit_scale
  end
  full.right_down = right
  -- Keep some of the floor on screen.
  full.centre_x = math.max(bounds[1], math.min(bounds[3], full.centre_x))
  full.centre_z = math.max(bounds[2], math.min(bounds[4], full.centre_z))
  view.centre_x, view.centre_z = full.centre_x, full.centre_z
  return inside, mx, my
end

local function draw_full_map(game, markers)
  -- Over the game and the other mods' drawing: the map replaces the picture while open.
  ui.draw_rect(game.x, game.y, game.w, game.h, 6 / 255, 5 / 255, 4 / 255, 1)
  local min_x, min_z, max_x, max_z = seen_bounds()
  local bounds = { min_x, min_z, max_x, max_z }
  local legend_width = game.h * 0.28
  local area = { x = game.x + game.h * 0.04, y = game.y + game.h * 0.08, w = game.w - legend_width - game.h * 0.08, h = game.h * 0.86 }
  -- Small explored areas are not blown up past a readable size.
  local fit_scale = math.min(area.w / (max_x - min_x), area.h / (max_z - min_z), game.h / 28)
  local text = math.max(14, game.h / 34)
  if not full.centre_set then
    full.centre_x, full.centre_z = (min_x + max_x) / 2, (min_z + max_z) / 2
  end
  local view = {
    origin_x = area.x + area.w / 2, origin_y = area.y + area.h / 2,
    centre_x = full.centre_x, centre_z = full.centre_z, scale = fit_scale * full.zoom, alpha = 1,
  }
  local inside, mx, my = handle_mouse(view, area, fit_scale, bounds)

  ui.push_clip(area.x, area.y, area.w, area.h)
  local half_w, half_h = area.w / 2 / view.scale + 1, area.h / 2 / view.scale + 1
  draw_cells(view, { view.centre_x - half_w, view.centre_z - half_h, view.centre_x + half_w, view.centre_z + half_h })
  draw_markers(view, markers, true)
  draw_players(view)
  ui.pop_clip()
  if inside then draw_hover(view, markers, mx, my, text * 0.9, game) end

  ui.draw_text(game.x + game.h * 0.04, game.y + game.h * 0.02, "Floor " .. kf.automap.floor, 240 / 255, 220 / 255, 170 / 255, 1, text * 1.4)
  local y = draw_legend(game.x + game.w - legend_width, game.y + game.h * 0.1, text)
  ui.draw_text(game.x + game.w - legend_width, y + text,
    "Scroll: zoom\nDrag: move\nRight click: reset\nPoint at a marker for details\nM closes the map",
    170 / 255, 160 / 255, 140 / 255, 1, text * 0.85)
end

-- ---------- callbacks ----------
local function in_game()
  local context = kf.context()
  return (context == "gameplay" or context == "scripted") and kf.automap.floor ~= 0
end

function on_draw()
  if not in_game() then
    if full.open then set_open(false) end
    return
  end
  if kf.action_pressed("full_map") then set_open(not full.open) end
  if not full.open and not config.minimap then return end
  refresh_cache()
  refresh_geometry()
  local x, y, w, h = kf.view()
  local game = { x = x, y = y, w = w, h = h }
  local markers = kf.automap.markers()
  if full.open then
    draw_full_map(game, markers)
  else
    draw_minimap(game, markers)
  end
end

local corners = { "Top right", "Bottom right", "Top left", "Bottom left" }

function on_settings()
  config.minimap = ui.checkbox("Minimap", config.minimap)
  config.corner = ui.combo("Corner", config.corner + 1, corners) - 1
  config.size = ui.slider_int("Size (% of the height)", config.size, 10, 50)
  config.zoom = ui.slider_int("Cells to the edge", config.zoom, 4, 30)
  config.opacity = ui.slider_int("Opacity (%)", config.opacity, 20, 100)
  config.fog = ui.checkbox("Only what you have seen", config.fog)
  ui.tooltip("Off: the whole floor. Carrying the floor's map shows it whole either way.")
  ui.separator()
  config.pause = ui.checkbox("The full map pauses the game", config.pause)
  ui.tooltip("Off: the game keeps running, so you can walk around with the map open.")
  config.keep_music = ui.checkbox("Keep the music playing while paused", config.keep_music)
  ui.separator()
  ui.text("Cheats")
  config.show_items = ui.checkbox("Items on the ground", config.show_items)
  config.show_chests = ui.checkbox("Chests and their contents", config.show_chests)
  config.show_enemies = ui.checkbox("Enemies and their drops", config.show_enemies)
  config.key_hints = ui.checkbox("Name the key a lock takes", config.key_hints)
  apply_to_game()   -- a changed pause or music applies at once
end
