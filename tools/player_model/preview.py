#!/usr/bin/env python3
"""Render the online player's knight, and the game's own models, to PNG for judging by eye.

Reads the game's files from the disc data (never committed); the pictures hold its textures, so
they go under build/ (gitignored). See docs/PLAYER_MODEL.md.

  preview.py survey                 every floor's model banks: vertices, primitives, clips, size
  preview.py floor N                floor N's map-event and actor models, three angles each
  preview.py knight                 the knight beside floor 2's standing knight, four angles
  preview.py helmet H               helmet H close up beside floor 2's knight's head, and whole
  preview.py symbol S               symbol S on the tabard in the game's depth order, round about,
                                    on a white and a black knight too
  preview.py swing W [--yaw A,B]    the knight attacking with weapon W, one picture per update
  preview.py rest W                 the knight standing with weapon W, from round about and above
  preview.py walk [W]                the knight's walk cycle (holding weapon W) above the skeleton's
  preview.py tmd OUT.tmd            write the knight's TMD
  preview.py cpp [OUT.cpp]          write the knight as the game's source
                                    (port/ext/online_player_model.cpp)
  preview.py gltf [--weapon W] [--phase P]
                                    the knight as knight.glb, to turn around in Blender; with
                                    weapon W's arm at attack phase P (at rest without --phase)

Needs numpy and Pillow.
"""
import argparse, collections, math, os, sys
import numpy as np
import kfmodel as kf, knight, arm, walk
from body import build_tmd, build_tmd_with_colours

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def vram(data, floor=None):
    v = kf.Vram()
    v.load_tims(open(f'{data}/COM/MIX.TIM', 'rb').read())
    if floor:
        v.load_tims(open(f'{data}/B{floor}/MIX.TIM', 'rb').read())
    return v

def floor_banks(data, floor):
    """MIXB.DAT: entity TMD, map TMD, then the map-event, effect and actor model archives."""
    chunks = kf.chunks(open(f'{data}/B{floor}/MIXB.DAT', 'rb').read())
    names = {2: 'event', 3: 'effect', 4: 'actor'}
    return {name: kf.archive(chunks[i]) for i, name in names.items() if i < len(chunks)}

def flatten(parts, tmd_bytes, pose, extra=(), lift=0.0):
    """The knight's objects posed (knight.posed) and joined into one object, with extra
    (vertices, normals, primitives) already in actor space, for kf.render."""
    tmd = kf.Tmd(tmd_bytes)
    world = knight.posed(parts, pose, lift)
    pieces = []
    for part, obj in zip(parts, tmd.objects):
        M, T = world[part.name]
        builder = part.pivot - obj['verts'].astype(float)    # objects are pivot-relative, actor space
        prims = obj['prims']
        if part.name.startswith('tabard'):
            prims = [dict(p, depth_bias=-knight.TABARD_DEPTH_BIAS) for p in prims]
        pieces.append((-(builder @ M.T + T), obj['norms'].astype(float) @ M.T, prims))
    pieces += list(extra)
    verts, norms, prims = [], [], []
    for g, n, ps in pieces:
        vo, no = sum(map(len, verts)), sum(map(len, norms))
        verts.append(g); norms.append(n)
        for p in ps:
            q = dict(p)
            if 'verts' in p:
                q['verts'] = [i + vo for i in p['verts']]; q['normals'] = [i + no for i in p['normals']]
            prims.append(q)
    flat = kf.Tmd.__new__(kf.Tmd)
    flat.objects = [dict(verts=np.concatenate(verts), norms=np.concatenate(norms), prims=prims, scale=0)]
    return flat

def survey(args):
    for floor in range(1, 6):
        for bank, assets in floor_banks(args.data, floor).items():
            for i, a in enumerate(assets):
                o = a.tmd.objects[0]; v = o['verts']
                modes = collections.Counter(hex(p['mode']) for p in o['prims'])
                print(f'B{floor} {bank}{i}: verts={len(v)} prims={len(o["prims"])} clips={a.clip_count} '
                      f'height={v[:, 1].max() - v[:, 1].min()} {dict(modes)}')

def floor(args):
    vr = vram(args.data, args.floor)
    banks = floor_banks(args.data, args.floor)
    imgs = [kf.render(a.tmd, vr, yaw=yaw, pitch=0.15, size=256)
            for bank in ('event', 'actor') for a in banks.get(bank, [])
            for yaw in (0, math.pi / 2, math.pi)]
    return kf.grid(imgs, 6)

def knight_sheet(args):
    vr = vram(args.data, 2)
    parts = knight.knight()
    tmd = build_tmd(parts)
    print('knight:', len(parts), 'objects,', sum(len(p.verts) for p in parts), 'vertices,',
          sum(len(p.faces) for p in parts), 'primitives,', len(tmd), 'bytes')
    flat = flatten(parts, tmd, {})
    reference = floor_banks(args.data, 2)['event'][0]
    centre = np.array([0, -900, 0])
    yaws = (0, math.pi * 0.75, math.pi / 2, math.pi)
    imgs = [kf.render(m, vr, yaw=y, pitch=0.12, size=360, extent=1150, center=centre)
            for m in (flat, reference.tmd) for y in yaws]
    return kf.grid(imgs, len(yaws))

def helmet_sheet(args):
    """Helmet H round about, above floor 2's knight's head at the same scale; then each in its
    silhouette, in the game's depth order (no depth buffer) and on the whole knight."""
    vr = vram(args.data, 2)
    parts = knight.knight(helmet=args.helmet)
    tmd = build_tmd(parts)
    helm = parts[-1]
    print(f'helmet {args.helmet}:', len(helm.verts), 'vertices,', len(helm.faces), 'primitives,', len(tmd), 'bytes with the knight')
    flat = flatten(parts, tmd, {})
    reference = floor_banks(args.data, 2)['event'][0].tmd
    top = reference.objects[0]['verts'][:, 1].min()
    close = dict(pitch=0.08, size=args.size, extent=300)
    yaws = (0, math.pi / 4, math.pi / 2, math.pi * 0.75, math.pi, -math.pi / 2)
    imgs = [kf.render(flat, vr, yaw=y, center=np.array([0, -1700, 40]), **close) for y in yaws]
    imgs += [kf.render(flat, vr, yaw=y, center=np.array([0, -1700, 40]), painter=True, **close) for y in yaws]
    imgs += [kf.render(reference, vr, yaw=y, center=np.array([0, top + 160, 0]), **close) for y in yaws]
    imgs += [kf.render(flat, vr, yaw=y, pitch=0.1, size=args.size, extent=1150, center=np.array([0, -900, 0])) for y in yaws]
    return kf.grid(imgs, len(yaws))

def symbol_sheet(args):
    """Symbol S round about in the game's depth order (no depth buffer), then with a depth buffer,
    then from above and below; then on the colours that change its ink."""
    vr = vram(args.data, 2)
    def drawn(**look):
        parts = knight.knight(symbol=args.symbol, **look)
        return flatten(parts, build_tmd(parts), {})
    flat = drawn()
    print(f'symbol {args.symbol}:', len(knight.tabards()[args.symbol].faces), 'primitives')
    whole = dict(size=args.size, extent=1150, center=np.array([0, -900, 0]))
    close = dict(size=args.size, extent=420, center=np.array([0, -1250, 0]))
    yaws = [i * math.pi / 4 for i in range(8)]
    imgs = [kf.render(flat, vr, yaw=y, pitch=0.1, painter=True, **close) for y in yaws]
    imgs += [kf.render(flat, vr, yaw=y, pitch=0.1, **close) for y in yaws]
    imgs += [kf.render(flat, vr, yaw=y, pitch=p, painter=True, **close) for p in (0.7, -0.5) for y in yaws]
    imgs += [kf.render(flat, vr, yaw=y, pitch=0.1, painter=True, **whole) for y in yaws]
    for colour, ink in (((200, 196, 184), (24, 22, 20)), ((40, 38, 40), (232, 228, 216))):
        look = drawn(colour=colour, ink=ink)
        imgs += [kf.render(look, vr, yaw=y, pitch=0.1, painter=True, **whole) for y in yaws]
    if args.symbol > 0:   # its badge, as the title's PROFILE page turns it
        for look in ({}, dict(colour=(200, 196, 184), ink=(24, 22, 20))):
            badge = [knight.badges(**look)[args.symbol - 1]]
            flat_badge = flatten(badge, build_tmd(badge), {})
            imgs += [kf.render(flat_badge, vr, yaw=y, pitch=0.0, painter=True, size=args.size, extent=250,
                               center=np.array([0, -1000, 0])) for y in yaws]
    return kf.grid(imgs, len(yaws))

def holding(parts, tmd, pose, lift, hold, phase):
    """The knight posed with the weapon arm through the hold at an attack phase (None at rest),
    the hand and weapon drawn on its wrist, joined for kf.render."""
    if hold is None:
        return flatten(parts, tmd, pose, lift=lift)
    pose = dict(pose)
    if phase is not None:
        w = arm.attack_weight(phase, hold.hit)
        rest_upper, _ = knight.posed(parts, pose, lift)['upper_arm_r']
        rest_forearm = rest_upper @ pose.get('forearm_r', np.eye(3))
        forearm = arm.slerp(rest_forearm, hold.swing(phase)[1], w)
        upper = arm.slerp(rest_upper, arm.upper_arm_follow(forearm), w)
        torso, _ = knight.posed(parts, pose, lift)['torso']
        pose['upper_arm_r'] = torso.T @ upper
        pose['forearm_r'] = upper.T @ forearm
    M, T = knight.posed(parts, pose, lift)['forearm_r']
    hand = hold.placed(hold.turn(phase), -(M @ arm.WRIST + T))
    shown = [p for p in parts if p.name != 'hand_r']
    return flatten(shown, build_tmd(shown), pose, [hand], lift)

def swing(args):
    vr = vram(args.data)
    parts = knight.knight()
    tmd = build_tmd(parts)
    hold = arm.WeaponHold(args.data, args.weapon)
    phases = [None] + list(range(0, arm.PHASE_END, 300))           # KF_WEAPON_ATTACK_PHASE_STEP
    stance, lift = walk.pose(0, 0.0)
    imgs = []
    for yaw in args.yaw:
        for phase in phases:
            flat = holding(parts, tmd, stance, lift, hold, phase)
            imgs.append(kf.render(flat, vr, yaw=yaw, pitch=0.1, size=args.size, extent=1300,
                                  center=np.array([0, -1100, -250])))
    return kf.grid(imgs, len(phases))

def rest(args):
    """The knight standing with weapon W, from round about, at four points of a breath."""
    vr = vram(args.data, 2)
    parts = knight.knight()
    tmd = build_tmd(parts)
    hold = arm.WeaponHold(args.data, args.weapon)
    imgs = []
    for pitch in (0.1, -0.5):
        for yaw in np.linspace(0, 2 * math.pi, args.views, endpoint=False):
            pose, lift = walk.pose(0, 0.0)
            imgs.append(kf.render(holding(parts, tmd, pose, lift, hold, None), vr, yaw=yaw, pitch=pitch,
                                  size=args.size, extent=1250, center=np.array([0, -950, -150])))
    return kf.grid(imgs, args.views)

def walk_sheet(args):
    """The knight through its walk cycle holding weapon W at rest, above the skeleton's walk
    (B1 actor model 5, its move clip) at the same phases."""
    vr = vram(args.data, 1)
    parts = knight.knight()
    tmd = build_tmd(parts)
    hold = arm.WeaponHold(args.data, args.weapon)
    skeleton = floor_banks(args.data, 1)['actor'][5]
    frames = 8
    centre = np.array([0, -950, 0])
    imgs = []
    for yaw in args.yaw:
        for k in range(frames):
            pose, lift = walk.pose(k / frames, args.amount)
            imgs.append(kf.render(holding(parts, tmd, pose, lift, hold, None), vr, yaw=yaw, pitch=0.05,
                                  size=args.size, extent=1100, center=centre))
        for k in range(frames):
            v = skeleton.pose(1, k * 4096 // frames)
            imgs.append(kf.render(skeleton.tmd, vr, verts=v, yaw=yaw, pitch=0.05, size=args.size, extent=1100,
                                  center=centre))
    return kf.grid(imgs, frames)

def gltf_model(args):
    import gltf
    vr = vram(args.data)
    parts = knight.knight()
    hold = None if args.weapon is None else arm.WeaponHold(args.data, args.weapon)
    pose, lift = walk.pose(0, 0.0)
    flat = holding(parts, build_tmd(parts), pose, lift, hold, args.phase)
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, 'knight.glb')
    gltf.write(flat, vr, path)
    print(path)

# The order of kf::OnlinePlayerPart (port/include/kf/platform/online_players.h).
PART_ORDER = ['pelvis', 'torso', 'head', 'upper_arm_r', 'forearm_r', 'upper_arm_l', 'forearm_l',
              'thigh_r', 'shin_r', 'thigh_l', 'shin_l', 'hand_r', 'hand_l']

def write_cpp(path):
    parts = knight.knight(helmet=None, symbol=None)
    names = [p.name for p in parts]
    if names != PART_ORDER:
        sys.exit(f"the knight's parts {names} are not in kf::OnlinePlayerPart's order")
    helmets = knight.helmets()
    tabards = knight.tabards()
    badges = knight.badges()
    tmd, colours, inks, pages = build_tmd_with_colours(parts + helmets + tabards + badges)
    def actor(v):   # builder space to the model's (actor) space
        return '{%d, %d, %d, 0}' % tuple(int(round(-c)) for c in v)
    joints = '\n'.join(f'    {{{names.index(p.parent) if p.parent else -1}, {actor(p.pivot)}}}'
                        f'{"," if i + 1 < len(parts) else ""} // {p.name}' for i, p in enumerate(parts))
    rows = ',\n'.join('    ' + ', '.join(f'0x{b:02x}' for b in tmd[i:i + 16]) for i in range(0, len(tmd), 16))
    def offset_rows(offsets):
        return ',\n'.join('    ' + ', '.join(str(o) for o in offsets[i:i + 12]) for i in range(0, len(offsets), 12))
    text = f'''// Generated by tools/player_model/preview.py cpp: do not edit. The knight other players are drawn
// as (docs/PLAYER_MODEL.md): {len(parts)} parts, {sum(len(p.verts) for p in parts)} vertices, \
{sum(len(p.faces) for p in parts)} primitives, then {len(helmets)} helmet{'' if len(helmets) == 1 else 's'}, {len(tabards)} tabards and {len(badges)} badge{'' if len(badges) == 1 else 's'}.
#include <kf/platform/online_players.h>

#include <array>

namespace kf {{
namespace {{

alignas(4) constexpr std::array<u8, {len(tmd)}> knight_tmd = {{
{rows}
}};

constexpr std::array<OnlinePlayerPartJoint, online_player_part_count> knight_joints = {{{{
{joints}
}}}};

// The helmets follow the parts, as many as the game offers.
static_assert({len(helmets)} == online_player_helmet_count);
// The tabards after them, one per symbol, then a badge for each symbol but none.
static_assert({len(tabards)} == online_player_symbol_count);
static_assert({len(badges)} == online_player_symbol_count - 1);

constexpr std::array<u32, {len(colours)}> knight_colour_offsets = {{
{offset_rows(colours)}
}};

constexpr std::array<u32, {len(inks)}> knight_ink_offsets = {{
{offset_rows(inks)}
}};

constexpr std::array<u32, {len(pages)}> knight_page_offsets = {{
{offset_rows(pages)}
}};

}}

std::span<const u8> online_player_model_tmd() {{
    return knight_tmd;
}}

std::span<const OnlinePlayerPartJoint, online_player_part_count> online_player_model_joints() {{
    return knight_joints;
}}

SVECTOR online_player_model_right_wrist() {{
    return {actor(arm.WRIST)};
}}

u16 online_player_model_helmet_object(u8 helmet) {{
    return static_cast<u16>(online_player_part_count + helmet);
}}

u16 online_player_model_tabard_object(u8 symbol) {{
    return static_cast<u16>(online_player_part_count + online_player_helmet_count + symbol);
}}

u16 online_player_model_badge_object(u8 symbol) {{
    return static_cast<u16>(online_player_part_count + online_player_helmet_count + online_player_symbol_count + symbol - 1);
}}

s16 online_player_model_tabard_depth_bias() {{
    return {-(knight.TABARD_DEPTH_BIAS >> 2)};   // in the ordering table's steps: KF_GTE_DEPTH_TO_OT_SHIFT
}}

std::span<const u32> online_player_model_colour_offsets() {{
    return knight_colour_offsets;
}}

std::span<const u32> online_player_model_ink_offsets() {{
    return knight_ink_offsets;
}}

std::span<const u32> online_player_model_page_offsets() {{
    return knight_page_offsets;
}}

}}
'''
    with open(path, 'w') as f:
        f.write(text)
    print(path)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', default=os.path.join(ROOT, 'data/en/KF'), help='the disc\'s KF directory')
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/player_model'), help='where pictures go')
    sub = ap.add_subparsers(dest='command', required=True)
    sub.add_parser('survey')
    p = sub.add_parser('floor'); p.add_argument('floor', type=int, choices=range(1, 6))
    sub.add_parser('knight')
    p = sub.add_parser('helmet'); p.add_argument('helmet', type=int)
    p.add_argument('--size', type=int, default=300)
    p = sub.add_parser('symbol'); p.add_argument('symbol', type=int)
    p.add_argument('--size', type=int, default=240)
    p = sub.add_parser('swing'); p.add_argument('weapon', type=int, choices=range(12))
    p.add_argument('--yaw', type=lambda s: [float(x) for x in s.split(',')], default=[0.8, math.pi / 2])
    p.add_argument('--size', type=int, default=200)
    p = sub.add_parser('rest'); p.add_argument('weapon', type=int, choices=range(12))
    p.add_argument('--views', type=int, default=6)
    p.add_argument('--size', type=int, default=320)
    p = sub.add_parser('walk'); p.add_argument('weapon', type=int, choices=range(12), nargs='?', default=0)
    p.add_argument('--yaw', type=lambda s: [float(x) for x in s.split(',')], default=[math.pi / 2, 0.8])
    p.add_argument('--size', type=int, default=200)
    p.add_argument('--amount', type=float, default=1.0)
    p = sub.add_parser('tmd'); p.add_argument('path')
    p = sub.add_parser('cpp'); p.add_argument('path', nargs='?',
                                              default=os.path.join(ROOT, 'port/ext/online_player_model.cpp'))
    p = sub.add_parser('gltf'); p.add_argument('--weapon', type=int, choices=range(12))
    p.add_argument('--phase', type=int)
    args = ap.parse_args()

    if args.command == 'survey':
        return survey(args)
    if args.command == 'gltf':
        return gltf_model(args)
    if args.command == 'cpp':
        return write_cpp(args.path)
    if args.command == 'tmd':
        with open(args.path, 'wb') as f:
            f.write(build_tmd(knight.knight()))
        return
    image = {'floor': floor, 'knight': knight_sheet, 'helmet': helmet_sheet, 'symbol': symbol_sheet, 'swing': swing, 'rest': rest, 'walk': walk_sheet}[args.command](args)
    os.makedirs(args.out, exist_ok=True)
    name = {'floor': lambda: f'floor{args.floor}', 'knight': lambda: 'knight',
            'helmet': lambda: f'helmet{args.helmet}',
            'symbol': lambda: f'symbol{args.symbol}',
            'swing': lambda: f'swing{args.weapon:02d}', 'rest': lambda: f'rest{args.weapon:02d}',
            'walk': lambda: 'walk'}[args.command]()
    path = os.path.join(args.out, name + '.png')
    kf.save(image, path)
    print(path)

if __name__ == '__main__':
    sys.exit(main())
