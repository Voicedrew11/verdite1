-- Run: the run action raises the walking speed limit (kf.play.top_speed), as the port's run key did.
-- The game keeps the slowed speed while the player is slowed.

local default_speed = 1.6
local message_s = 1.8

if type(config.speed) ~= "number" then config.speed = default_speed end
if type(config.toggle) ~= "boolean" then config.toggle = false end

local running = false   -- toggle mode's state
local message, message_until = nil, 0

local function show(text)
  message, message_until = text, kf.time() + message_s
end

function on_update()
  if config.toggle then
    if kf.context() == "gameplay" and kf.action_pressed("run") then
      running = not running
      show(running and "Running" or "Walking")
    end
  else
    running = kf.action_down("run")
  end
  kf.play.top_speed = running and config.speed or 1.0
end

function on_draw()
  if not message or kf.time() >= message_until then return end
  local x, y, w, h = kf.view()
  local size = math.max(18, h / 30)
  local tw, th = ui.text_size(message, size)
  local alpha = math.min(1, (message_until - kf.time()) / 0.4)
  local left, top = x + (w - tw) / 2, y + h * 0.2
  ui.draw_rect(left - size * 0.6, top - size * 0.3, tw + size * 1.2, th + size * 0.6, 0, 0, 0, 0.6 * alpha)
  ui.draw_text(left, top, message, 1, 0.92, 0.67, alpha, size)
end

function on_settings()
  config.toggle = ui.checkbox("Press to switch running on and off", config.toggle)
  ui.tooltip("Off: hold the run key to run.")
  config.speed = ui.slider_float("Run speed", config.speed, 1.0, 2.5, "%.2fx")
  if config.speed ~= default_speed then
    ui.same_line()
    if ui.button("Default") then
      config.speed = default_speed
      kf.save_config()
    end
  end
end
