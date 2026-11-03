-- The Ember Crypt: an example of the layers that change the game.
--   kf.content  two items, a monster kind and a floor
--   kf.rules    the kind's numbers
--   kf.hook     double experience while the draught burns
--   kf.call     the reward, which must wait for on_between_updates
--   kf.raw      the draught's healing
--   exports     what other mods may ask, and mod.raise what they may hear
local CRYPT_FOLDER = "floors/crypt"
local BRUTES = 3
local DRAUGHT_UPDATES = 900      -- a minute, at 15 updates a second
local REWARD_EXPERIENCE = 200

local draught, sigil, crypt      -- ids from kf.content
local brute                      -- this floor's kind: a floor's kinds go when it does
local brutes_left = 0
local draught_updates = 0
local arrived_in_crypt = false
local reward_due = false

function on_load()
    draught = kf.content.item{
        key = "ember_draught",
        name = "EMBER DRAUGHT",
        description = "Heals fully. Experience is doubled for a minute.",
        model_item = 48,
        sell = 100,
    }
    sigil = kf.content.item{
        key = "crypt_sigil",
        name = "CRYPT SIGIL",
        description = "Opens the way to the crypt, and back.",
        model_item = 41,
    }
    if mod.fs.exists(CRYPT_FOLDER .. "/MIX.TIM") then
        crypt = kf.content.floor{key = "crypt", folder = CRYPT_FOLDER, entry_x = 14, entry_z = 1}
    else
        mod.log("no crypt: run tools/mod_floor_from_disc.py KF/B1 mods/crypt/floors/crypt")
    end
end

-- ---- the brutes ----

local function make_brute(floor)
    local kinds = kf.actors.kinds()
    if #kinds == 0 then
        brute = nil
        return
    end
    local base = kinds[1]
    brute = kf.content.monster(base.kind)
    kf.rules.set_monster(floor, brute, {
        hp = math.min(65535, base.hp * 3),
        experience = math.min(32767, base.experience * 3),
        drop_item = draught,
        drop_chance = 255,
    })
end

-- A brute on each open cell around the player, nearest last, so they do not arrive on top of the player.
local function rouse_brutes()
    if not brute then return end
    local p = kf.player
    local cell_size = 2000
    local offsets = {{2, 0}, {-2, 0}, {0, 2}, {0, -2}, {2, 2}, {-2, 2}, {2, -2}, {-2, -2}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}}
    brutes_left = 0
    for _, offset in ipairs(offsets) do
        if brutes_left == BRUTES then break end
        local cell_x, cell_z = p.cell_x + offset[1], p.cell_z + offset[2]
        if not kf.world.cell_blocked(cell_x, cell_z) and
           kf.actors.spawn(brute, p.x + offset[1] * cell_size, p.z + offset[2] * cell_size) then
            brutes_left = brutes_left + 1
        end
    end
    mod.log("roused " .. brutes_left .. " brutes")
end

function on_map_load(e)
    if not save.sigil_given then
        save.sigil_given = true
        kf.items.give(sigil, 1)
    end
    make_brute(e.floor)
    brutes_left = 0
    arrived_in_crypt = e.floor == crypt
end

function on_kill(e)
    if e.kind ~= brute then return end
    save.slain = (save.slain or 0) + 1
    mod.raise("brute_slain", {total = save.slain})
    brutes_left = brutes_left - 1
    if brutes_left == 0 then reward_due = true end
end

-- ---- the items ----

local function use_sigil()
    if not crypt then
        rouse_brutes()
    elseif kf.player.floor == crypt then
        local back = save.way_back or {floor = 1}
        kf.warp(back.floor, back.cell_x, back.cell_z, back.yaw)
    else
        local p = kf.player
        save.way_back = {floor = p.floor, cell_x = p.cell_x, cell_z = p.cell_z, yaw = p.yaw}
        kf.warp(crypt)
    end
end

local function use_draught()
    local vitals = kf.raw.player_state.vitals
    vitals.current_hp = vitals.maximum_hp
    draught_updates = DRAUGHT_UPDATES
    kf.items.take(draught, 1)
end

function on_use_item(e)
    if e.item == sigil then
        use_sigil()
        e.used = true
    elseif e.item == draught then
        use_draught()
        e.used = true
    end
end

kf.hook.before.player_add_experience(function(amount)
    if draught_updates > 0 then
        return math.min(32767, amount * 2)
    end
end)

function on_update()
    if draught_updates > 0 and kf.context() == "gameplay" then
        draught_updates = draught_updates - 1
    end
    if arrived_in_crypt and kf.context() == "gameplay" then
        arrived_in_crypt = false
        rouse_brutes()
    end
end

function on_between_updates()
    if reward_due then
        reward_due = false
        kf.call.player_add_experience(REWARD_EXPERIENCE)
        mod.log("the crypt is quiet: " .. REWARD_EXPERIENCE .. " experience")
    end
end

-- ---- for other mods ----

function exports.brutes_slain()
    return save.slain or 0
end

function exports.draught_burning()
    return draught_updates > 0
end
