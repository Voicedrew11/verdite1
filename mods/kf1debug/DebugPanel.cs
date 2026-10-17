// ModCompiler compiles mods with no implicit usings, so every namespace the
// file needs must be named here -- including System.
using System;
using System.Collections.Generic;
using System.Numerics;
using ImGuiNET;
using RecompOne.Runtime.Host.Window;
using RecompOne.Runtime.Memory;

namespace Kf1.Mods.Debug;

/// <summary>
/// The dockable panel.
///
/// A registered IPanel rather than everything living in IMod.DrawSettings,
/// because DrawSettings only draws while the Mods popup is open -- and a readout
/// you can only see with a modal over the game is not a readout. This docks
/// alongside CPU State and Memory Editor and stays up while you play.
///
/// Verdite3's tab set, written against what this game's feature files expose.
/// </summary>
internal sealed class DebugPanel : IPanel
{
    internal static readonly DebugPanel Instance = new();

    public string Name => "KF1 Debug";
    public string TitleKey => "panel.kf1_debug";
    public bool IsOpen { get; set; }

    int _warpX, _warpY, _warpZ;
    bool _coordsPrimed;

    public void Draw()
    {
        ImGui.SetNextWindowSize(new Vector2(400, 620), ImGuiCond.FirstUseEver);
        bool open = IsOpen;
        if (!ImGui.Begin(this.Title(), ref open))
        {
            IsOpen = open;
            ImGui.End();
            return;
        }
        IsOpen = open;

        var mem = RecompOne.Runtime.Runtime.Mem;
        if (mem == null)
        {
            ImGui.TextDisabled("Not running.");
            ImGui.End();
            return;
        }

        // Max HP is zero until an area is up, which covers the title screen and
        // the attract demo. Everything here no-ops in that state anyway; saying so
        // is better than showing a panel of dead controls.
        if (!GameState.IsInGame(mem))
        {
            ImGui.TextDisabled("No area running.");
            ImGui.TextWrapped("Load a save or start a game -- the player state block is cleared "
                            + "until an area is up, so there is nothing to read or change yet.");
            ImGui.End();
            return;
        }

        if (ImGui.BeginTabBar("##kf1debugtabs"))
        {
            if (ImGui.BeginTabItem("Cheats"))     { DrawCheats(mem);     ImGui.EndTabItem(); }
            if (ImGui.BeginTabItem("Attributes")) { DrawAttributes(mem); ImGui.EndTabItem(); }
            if (ImGui.BeginTabItem("Items"))      { DrawItems(mem);      ImGui.EndTabItem(); }
            if (ImGui.BeginTabItem("Magic"))      { DrawMagic(mem);      ImGui.EndTabItem(); }
            if (ImGui.BeginTabItem("Warp"))       { DrawWarp(mem);       ImGui.EndTabItem(); }
            if (ImGui.BeginTabItem("State"))      { DrawState(mem);      ImGui.EndTabItem(); }
            if (ImGui.BeginTabItem("Keys"))       { DrawKeys();          ImGui.EndTabItem(); }
            ImGui.EndTabBar();
        }

        ImGui.End();
    }

    // ---- cheats ----

    void DrawCheats(IMemory mem)
    {
        bool noclip = Noclip.Enabled;
        if (ImGui.Checkbox("Noclip flight", ref noclip)) Noclip.Enabled = noclip;
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Fly through walls, with the body coming along. Forward goes where "
                           + "the camera is looking, pitch included. Your own walk keys or the "
                           + "left stick move; Page Up and Page Down -- or the pad's R2 and L2 -- "
                           + "go up and down; Left Shift or R3 is fast. F3 toggles it. Collision "
                           + "is not disabled -- the position is written after the game's own "
                           + "movement, so enemies and items keep theirs.");

        if (Noclip.Enabled)
        {
            ImGui.Indent();

            ImGui.TextWrapped("Flight is held to the 100 x 100 cell map. Exits do not fire while "
                            + "flying; use the Warp tab to change area.");

            long strayed = Noclip.DistanceFromEntry(mem);
            ImGui.Text($"{strayed:n0} units from where you switched it on");
            ImGui.SameLine();
            if (ImGui.Button("Go back")) Noclip.ReturnToEntry();
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("Back to where noclip was switched on -- the undo for a flight "
                               + "that went too far.");

            ImGui.SliderFloat("Speed", ref Noclip.Speed, 200f, 30000f, "%.0f units/s");
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("Units a second, spent against real elapsed time -- so the "
                               + "flight is the same speed whatever the frame rate.");
            ImGui.SliderFloat("Fast multiplier", ref Noclip.FastMultiplier, 1f, 10f, "x%.1f");

            bool invY = Noclip.InvertVertical;
            if (ImGui.Checkbox("Invert up/down", ref invY)) Noclip.InvertVertical = invY;
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("Negative Y is up in this game. Flip this if Page Up takes you "
                               + "into the floor.");

            bool invP = Noclip.InvertPitch;
            if (ImGui.Checkbox("Invert dive", ref invP)) Noclip.InvertPitch = invP;
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("Which sign of pitch looks down has not been checked. Flip this if "
                               + "looking down and pushing forward climbs.");

            bool cine = Noclip.Cinematic;
            if (ImGui.Checkbox("Cinematic camera", ref cine)) Noclip.Cinematic = cine;
            if (ImGui.IsItemHovered())
                ImGui.SetTooltip("Ease the flight and the turn instead of landing on the input: "
                               + "the velocity builds up and coasts down, and the view trails "
                               + "where you are looking. For filming a flythrough, not for "
                               + "getting somewhere. The hotkey toasts stay silent while it is on.");

            if (Noclip.Cinematic)
            {
                ImGui.Indent();
                ImGui.SliderFloat("Move smoothing", ref Noclip.MoveSmoothing, 0.05f, 2f, "%.2f s");
                if (ImGui.IsItemHovered())
                    ImGui.SetTooltip("Seconds to about two thirds of the speed you asked for, and "
                                   + "the same again coasting back down.");
                ImGui.SliderFloat("Look smoothing", ref Noclip.LookSmoothing, 0.05f, 2f, "%.2f s");
                if (ImGui.IsItemHovered())
                    ImGui.SetTooltip("How far the camera trails the stick or the mouse. The turn "
                                   + "never loses ground -- it arrives late, not short.");
                ImGui.Unindent();
            }

            bool invS = Noclip.InvertStrafe;
            if (ImGui.Checkbox("Invert strafe", ref invS)) Noclip.InvertStrafe = invS;

            ImGui.TextDisabled($"entered at {Noclip.Format(Noclip.EntryPosition)}");
            ImGui.Unindent();
        }

        ImGui.Separator();

        bool god = Cheats.Invincible;
        if (ImGui.Checkbox("Invincibility", ref god)) Cheats.Invincible = god;
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Skips the player-hit routine (no HP loss, no poison, no flinch), "
                           + "clamps the HP drains to zero, refuses the death routine -- which "
                           + "also covers the hazard cells that kill at full HP -- and restores HP "
                           + "each tick as a backstop.");

        bool mp = Cheats.InfiniteMp;
        if (ImGui.Checkbox("Infinite MP", ref mp)) Cheats.InfiniteMp = mp;
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Restores MP to its maximum at the end of every player tick, so a "
                           + "cast still pays its cost on the way in.");

        bool calm = Cheats.Peaceful;
        if (ImGui.Checkbox("Enemies ignore you", ref calm)) Cheats.Peaceful = calm;
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Tells the creature AI's behaviour picker that you are across the "
                           + "map, so each creature picks what it does with nobody near. They "
                           + "still appear, animate, block and take damage.");

        ImGui.Separator();

        bool speed = Cheats.SpeedEnabled;
        if (ImGui.Checkbox("Speed multiplier", ref speed)) Cheats.SpeedEnabled = speed;
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Scales each walk step on its way into the game's own collision move, "
                           + "and adds the rest of the turn after the game's. A high multiplier "
                           + "takes longer steps than the collision was written for.");
        if (Cheats.SpeedEnabled)
        {
            ImGui.Indent();
            ImGui.SliderFloat("Multiplier", ref Cheats.SpeedMultiplier, 0.25f, 8f, "x%.2f");
            ImGui.Unindent();
        }

        ImGui.Separator();
        ImGui.TextDisabled($"hits blocked {Cheats.BlockedHits}, deaths refused {Cheats.BlockedDeaths}, "
                         + $"HP restores {Cheats.RestoredHp}, MP restores {Cheats.RestoredMp}, "
                         + $"AI picks faked {Cheats.IgnoredPicks}");
    }

    // ---- attributes ----
    //
    // The character is plain memory and a write sticks; the adjusted STR and MAG
    // and the ratings are a cache func_80015714 rebuilds. Attributes.cs has why.

    void DrawAttributes(IMemory mem)
    {
        ImGui.TextWrapped("Everything here writes the game's own state, so it is saved with your "
                        + "character and it takes effect immediately.");

        if (ImGui.Button("Full heal")) Attributes.FullHeal(mem);
        ImGui.SameLine();
        if (ImGui.Button("Cure conditions")) Attributes.CureConditions(mem);
        ImGui.SameLine();
        if (ImGui.Button("Level up")) Attributes.QueueLevelUp();
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Runs the game's own level-up, so the level, both maxima, STR, MAG and "
                           + "the next threshold move by the game's table. EXP is topped up to the "
                           + "threshold first. HP and MP are not refilled, as in the game.");
        ImGui.SameLine();
        if (ImGui.Button("Recalculate")) Attributes.QueueRecalculate();
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Runs func_80015714, which rebuilds the adjusted STR and MAG and the "
                           + "ratings from your equipment.");

        if (!string.IsNullOrEmpty(Attributes.Status))
            ImGui.TextDisabled(Attributes.Status);

        ImGui.Separator();

        if (ImGui.CollapsingHeader("Character", ImGuiTreeNodeFlags.DefaultOpen))
        {
            ImGui.Indent();
            DrawFields(mem, Attributes.Progress);
            ImGui.Spacing();
            DrawFields(mem, Attributes.Vitals);
            ImGui.Spacing();
            DrawFields(mem, Attributes.BaseAttributes);
            ImGui.Unindent();
        }

        if (ImGui.CollapsingHeader("Condition"))
        {
            ImGui.Indent();
            DrawFields(mem, Attributes.Conditions, "cond");
            ImGui.Unindent();
        }

        if (ImGui.CollapsingHeader("Ratings"))
        {
            ImGui.Indent();

            ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.72f, 0.25f, 1f));
            ImGui.TextWrapped("These are a cache. func_80015714 rebuilds them from your equipment on "
                            + "every equip and level-up, so a number typed here lasts until then "
                            + "unless you hold it. What each rating means is not resolved.");
            ImGui.PopStyleColor();

            bool locked = Attributes.LockDerived;
            if (ImGui.Checkbox("Hold these values", ref locked))
            {
                if (locked && !Attributes.LockDerived) Attributes.PrimeHold(mem);
                Attributes.LockDerived = locked;
            }

            ImGui.Spacing();
            DrawFields(mem, Attributes.Adjusted);
            ImGui.Spacing();
            DrawFields(mem, Attributes.RatingsA, "a");
            ImGui.Spacing();
            DrawFields(mem, Attributes.RatingsB, "b");

            ImGui.Unindent();
        }
    }

    void DrawFields(IMemory mem, Attributes.Field[] fields, string id = "")
    {
        for (int i = 0; i < fields.Length; i++)
        {
            ref readonly var f = ref fields[i];
            int value = Attributes.Read(mem, f);

            ImGui.PushID($"{id}{f.Address:X8}");
            ImGui.SetNextItemWidth(150);
            if (ImGui.InputInt(f.Name, ref value))
            {
                Attributes.Write(mem, f, value);
                int held = Attributes.HeldIndex(f.Address);
                if (held >= 0) Attributes.Held[held] = Attributes.Read(mem, f);
            }
            ImGui.PopID();

            if (f.Tip.Length != 0 && ImGui.IsItemHovered()) ImGui.SetTooltip(f.Tip);
        }
    }

    // ---- items and equipment ----

    bool _itemsHeldOnly;

    void DrawItems(IMemory mem)
    {
        ImGui.TextWrapped("The inventory is one count byte per item id. The game's names are glyph "
                        + "codes this panel cannot render, so items are shown by id.");

        ImGui.Text($"{Items.DistinctHeld(mem)} of {Items.Count} ids held");
        ImGui.SameLine();
        if (ImGui.SmallButton("Give one of each")) Items.QueueGiveAll(mem);
        ImGui.SameLine();
        if (ImGui.SmallButton("Clear all")) Items.ClearAll(mem);
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Zeroes every count. What you are wearing stays equipped.");
        ImGui.SameLine();
        ImGui.Checkbox("Held only", ref _itemsHeldOnly);

        if (!string.IsNullOrEmpty(Items.Status))
        {
            ImGui.Separator();
            ImGui.TextWrapped(Items.Status);
        }

        ImGui.Separator();

        if (ImGui.BeginChild("##itemlist", new Vector2(0, 250), ImGuiChildFlags.None))
        {
            foreach (var g in Items.Groups(mem))
            {
                var shown = new List<int>();
                for (int id = g.First; id <= g.Last; id++)
                {
                    if (Items.IsUnused(mem, id)) continue;
                    if (_itemsHeldOnly && !Items.Has(mem, id)) continue;
                    shown.Add(id);
                }
                if (shown.Count == 0) continue;

                if (_itemsHeldOnly) ImGui.SetNextItemOpen(true, ImGuiCond.Always);
                if (!ImGui.CollapsingHeader($"{g.Name}  ({g.First}-{g.Last})###grp{g.First}")) continue;

                ImGui.Indent();
                foreach (int id in shown) DrawItemRow(mem, id);
                ImGui.Unindent();
            }
        }
        ImGui.EndChild();

        if (ImGui.CollapsingHeader("Equipment", ImGuiTreeNodeFlags.DefaultOpen))
        {
            ImGui.Indent();
            DrawEquipment(mem);
            ImGui.Unindent();
        }
    }

    void DrawItemRow(IMemory mem, int id)
    {
        int held = Items.Held(mem, id);

        ImGui.PushID(id);
        if (ImGui.SmallButton("+1")) Items.QueueGive(id);
        ImGui.SameLine();
        if (ImGui.SmallButton("-1")) Items.QueueRemove(id);
        ImGui.SameLine();
        ImGui.SetNextItemWidth(90);
        if (ImGui.InputInt("##count", ref held)) Items.SetHeld(mem, id, held);
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("The count byte itself. Zero is not holding it; the shop stops at 99.");
        ImGui.SameLine();
        if (Items.Has(mem, id)) ImGui.Text(Items.Label(mem, id));
        else ImGui.TextDisabled(Items.Label(mem, id));
        ImGui.PopID();
    }

    void DrawEquipment(IMemory mem)
    {
        ImGui.TextWrapped("Equipping runs the game's own setter and its stat rebuild. The combo "
                        + "offers the ids you hold that the equip menu allows in that slot.");

        foreach (var slot in Items.EquipSlots)
        {
            int equipped = Items.Equipped(mem, slot);
            string current = equipped < 0 ? "(empty)" : Items.Label(mem, equipped);

            ImGui.Text(Items.SlotName(slot));
            ImGui.SameLine();
            ImGui.SetNextItemWidth(200);
            if (ImGui.BeginCombo($"##equip{slot}", current))
            {
                if (ImGui.Selectable("(empty)")) Items.QueueEquip(Items.EmptySlot, slot);
                var (first, last) = Items.SlotRange(slot);
                for (int id = first; id <= last; id++)
                {
                    if (Items.IsUnused(mem, id) || !Items.Has(mem, id)) continue;
                    if (ImGui.Selectable(Items.Label(mem, id))) Items.QueueEquip(id, slot);
                }
                ImGui.EndCombo();
            }
        }
    }

    // ---- magic ----

    void DrawMagic(IMemory mem)
    {
        ImGui.TextWrapped("The player's spells are records 0..8: 0..3 are cast from the magic menu, "
                        + "4..8 are held for the cast button. Byte 0 of a record is 1 when known.");

        ImGui.Text($"held: {Magic.Held(mem)}");
        if (ImGui.SmallButton("Learn all")) Magic.QueueLearnAll();

        if (!string.IsNullOrEmpty(Magic.Status))
            ImGui.TextDisabled(Magic.Status);

        ImGui.Separator();

        int heldId = Magic.HeldId(mem);
        for (int i = 0; i < Magic.Count; i++)
        {
            bool known = Magic.Known(mem, i);

            ImGui.PushID(i);
            if (ImGui.Checkbox("##known", ref known))
            {
                if (known) Magic.QueueLearn(i);
                else       Magic.QueueForget(i);
            }
            ImGui.SameLine();
            ImGui.Text($"{i}  {Magic.Cost(mem, i),3} mp  {Magic.Name(i)}");
            if (i >= Magic.FirstHoldable)
            {
                ImGui.SameLine();
                if (heldId == i) ImGui.TextDisabled("(held)");
                else if (ImGui.SmallButton("Hold")) Magic.QueueHold(i);
            }
            ImGui.PopID();
        }
    }

    // ---- warp ----

    void DrawWarp(IMemory mem)
    {
        if (!_coordsPrimed)
        {
            (_warpX, _warpY, _warpZ) = GameState.Position(mem);
            _coordsPrimed = true;
        }

        ImGui.TextWrapped("Bookmarks remember the area they were taken in. One from another area "
                        + "warps there first, then places you.");
        ImGui.Separator();

        for (int i = 0; i < Warp.SlotCount; i++)
        {
            ImGui.PushID(i);

            if (ImGui.Button("Save")) Warp.Save(i);
            ImGui.SameLine();

            if (!Warp.IsSet(i)) ImGui.BeginDisabled();
            if (ImGui.Button("Go")) Warp.Restore(i);
            ImGui.SameLine();
            if (ImGui.Button("Clear")) Warp.Clear(i);
            if (!Warp.IsSet(i)) ImGui.EndDisabled();

            ImGui.SameLine();
            if (Warp.IsSet(i))
                ImGui.Text($"{i + 1}: {Warp.AreaName(Warp.SlotArea(i))} {Noclip.Format(Warp.SlotPosition(i))}");
            else
                ImGui.TextDisabled($"{i + 1}: empty");

            ImGui.PopID();
        }

        ImGui.Separator();
        ImGui.Text("Coordinates");
        ImGui.InputInt("X", ref _warpX);
        ImGui.InputInt("Y (eye height)", ref _warpY);
        ImGui.InputInt("Z", ref _warpZ);

        if (ImGui.Button("Go to coordinates")) Warp.Teleport(_warpX, _warpY, _warpZ);
        ImGui.SameLine();
        if (ImGui.Button("Read current")) (_warpX, _warpY, _warpZ) = GameState.Position(mem);
        ImGui.SameLine();
        if (ImGui.Button("Snap to floor")) Noclip.SnapToFloor();
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Puts your eye 1500 above the floor of the cell you are over. The way "
                           + "out of a flight that ended inside something.");

        ImGui.Separator();
        ImGui.Text($"Area (currently {Warp.AreaName(mem.ReadU8(GameState.Area))})");
        ImGui.TextWrapped("Each button runs the game's own area transition (func_80036850, the one "
                        + "the exits call). The areas share coordinates, so you arrive at the same "
                        + "X/Z over the new area's floor.");
        bool fly = Warp.FlyAfterWarp;
        if (ImGui.Checkbox("Fly after an area warp", ref fly))
        {
            Warp.FlyAfterWarp = fly;
            DebugMod.Persist(DebugMod.FlyAfterWarpKey, fly);
        }
        if (ImGui.IsItemHovered())
            ImGui.SetTooltip("Turns noclip on when you arrive, in case the cell is inside a wall. "
                           + "Fly somewhere open and turn noclip off (F3).");

        bool busy = Warp.Busy;
        if (busy) ImGui.BeginDisabled();
        for (int area = Warp.FirstArea; area <= Warp.LastArea; area++)
        {
            ImGui.PushID(area);
            if (ImGui.SmallButton("Warp")) Warp.ToArea(area);
            ImGui.SameLine();
            if (area == mem.ReadU8(GameState.Area)) ImGui.Text($"{Warp.AreaName(area)}  (here)");
            else ImGui.TextDisabled(Warp.AreaName(area));
            ImGui.PopID();
        }
        if (busy) ImGui.EndDisabled();

        if (!string.IsNullOrEmpty(Warp.Status))
        {
            ImGui.Separator();
            ImGui.TextWrapped(Warp.Status);
        }
    }

    // ---- readout ----

    void DrawState(IMemory mem)
    {
        var (x, y, z) = GameState.Position(mem);
        var (pitch, yaw, roll) = GameState.Angles(mem);

        ImGui.Text("Position");
        ImGui.Indent();
        ImGui.Text($"X {x,8}   Y {y,8}   Z {z,8}");
        ImGui.Text($"floor {GameState.ReadS32(mem, GameState.Floor)}   cell ({mem.ReadU8(GameState.CellX)}, "
                 + $"{mem.ReadU8(GameState.CellZ)})   last ({mem.ReadU8(GameState.LastCellX)}, {mem.ReadU8(GameState.LastCellZ)})");
        ImGui.Unindent();

        ImGui.Text("View");
        ImGui.Indent();
        ImGui.Text($"yaw   {yaw,6}  ({yaw * 360f / GameState.AngleFull,6:0.0} deg)  increases turning left");
        ImGui.Text($"pitch {pitch,6}  ({pitch * 360f / GameState.AngleFull,6:0.0} deg)  limit +-{GameState.PitchLimit}");
        ImGui.Text($"roll  {roll,6}");
        ImGui.Unindent();

        ImGui.Separator();
        ImGui.Text("Character");
        ImGui.Indent();
        ImGui.Text($"HP  {mem.ReadU16(GameState.Hp),5} / {mem.ReadU16(GameState.MaxHp),-5}");
        ImGui.Text($"MP  {mem.ReadU16(GameState.Mp),5} / {mem.ReadU16(GameState.MaxMp),-5}");
        ImGui.Text($"LV  {mem.ReadU8(GameState.Level),5}   EXP {mem.ReadU32(GameState.Exp)} / {mem.ReadU32(GameState.ExpNext)}");
        ImGui.Text($"STR {mem.ReadU16(Attributes.AdjStr),5}   MAG {mem.ReadU16(Attributes.AdjMag),5}   gold {mem.ReadU32(Attributes.Gold)}");
        ImGui.Unindent();

        ImGui.Separator();
        ImGui.Text("Engine");
        ImGui.Indent();
        byte state = mem.ReadU8(GameState.State);
        string what = state switch
        {
            0 => "standing",
            GameState.StateDying => "dying",
            GameState.StateRespawn => "respawn",
            _ => "flinch",
        };
        ImGui.Text($"state      0x{state:X2}  ({what})");
        ImGui.Text($"area       {Warp.AreaName(mem.ReadU8(GameState.Area))}");
        ImGui.Text($"condition  0x{mem.ReadU16(Attributes.Condition):X4}");
        ImGui.Text($"walk speed {mem.ReadU32(GameState.MoveSpeed)}   turn rate {mem.ReadU32(GameState.TurnRate)}");
        ImGui.Unindent();

        ImGui.Separator();
        ImGui.Text("Velocities");
        ImGui.Indent();
        ImGui.Text($"walk {GameState.ReadS16(mem, GameState.FwdVel),6}   strafe {GameState.ReadS16(mem, GameState.StrafeVel),6}");
        ImGui.Text($"turn {GameState.ReadS16(mem, GameState.TurnVel),6}   pitch  {GameState.ReadS16(mem, GameState.PitchVel),6}");
        ImGui.Unindent();
    }

    // ---- keys ----

    void DrawKeys()
    {
        bool on = Hotkeys.Enabled;
        if (ImGui.Checkbox("Hotkeys enabled", ref on)) Hotkeys.Enabled = on;

        ImGui.Separator();
        if (ImGui.BeginTable("##keys", 2, ImGuiTableFlags.SizingStretchProp))
        {
            Row("F2", "toggle this panel");
            Row("F3", "toggle noclip");
            Row("F4", "toggle invincibility");
            Row("F5 / F6", "save / go to bookmark 1");
            Row("F7", "snap to floor");
            Row("F8", "back to where noclip was switched on");
            Row("V", "film mode: cinematic camera, enemies ignore you and noclip, together (no toast)");
            Row("Page Up / Page Down", "fly up / down");
            Row("Left Shift", "fly fast (hold)");
            ImGui.EndTable();
        }

        ImGui.Separator();
        ImGui.TextDisabled("On a pad");
        if (ImGui.BeginTable("##padkeys", 2, ImGuiTableFlags.SizingStretchProp))
        {
            Row("Mute / Share", "film mode (as V)");
            Row("Left stick", "fly forward and strafe");
            Row("R2 / L2", "fly up / down");
            Row("R3", "fly fast (hold)");
            ImGui.EndTable();
        }

        ImGui.Separator();
        ImGui.TextWrapped("F1 and F11 are the host's own (top bar, fullscreen) and are left alone. "
                        + "The shipped keyboard layout binds W A S D, the arrows, Space (attack), "
                        + "F, Q, Tab, Enter and Right Shift, so the flight keys are Page Up, "
                        + "Page Down and Left Shift; V is free too. R2 and L2 also pitch the view.");

        static void Row(string key, string what)
        {
            ImGui.TableNextRow();
            ImGui.TableNextColumn(); ImGui.Text(key);
            ImGui.TableNextColumn(); ImGui.TextDisabled(what);
        }
    }
}
