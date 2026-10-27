#!/usr/bin/env python3
"""Render the online player's knight, and the game's own models, to PNG for judging by eye.

Reads the game's files from the disc data (never committed); the pictures hold its textures, so
they go under build/ (gitignored). See docs/PLAYER_MODEL.md.

  preview.py survey                 every floor's model banks: vertices, primitives, clips, size
  preview.py floor N                floor N's map-event and actor models, three angles each
  preview.py knight                 the knight beside floor 2's standing knight, four angles
  preview.py swing W [--yaw A,B]    the knight attacking with weapon W, one picture per update
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
from body import build_tmd

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
        pieces.append((-(builder @ M.T + T), obj['norms'].astype(float) @ M.T, obj['prims']))
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

def swing(args):
    vr = vram(args.data)
    parts = [p for p in knight.knight() if p.name != 'forearm_r']   # the weapon arm replaces it
    tmd = build_tmd(parts)
    weapon = arm.WeaponArm(args.data, args.weapon)
    phases = [None] + list(range(0, arm.PHASE_END, 300))           # KF_WEAPON_ATTACK_PHASE_STEP
    imgs = []
    for yaw in args.yaw:
        for phase in phases:
            vertices, pitch = weapon.state(phase)
            pose = {'upper_arm_r': knight.rot('x', pitch)}
            M, T = knight.posed(parts, pose)['upper_arm_r']
            elbow = -(M @ arm.ELBOW + T)
            flat = flatten(parts, tmd, pose, [weapon.placed(vertices, elbow)])
            imgs.append(kf.render(flat, vr, yaw=yaw, pitch=0.1, size=args.size, extent=1300,
                                  center=np.array([0, -1100, -250])))
    return kf.grid(imgs, len(phases))

def walk_sheet(args):
    """The knight through its walk cycle holding weapon W at rest, above the skeleton's walk
    (B1 actor model 5, its move clip) at the same phases."""
    vr = vram(args.data, 1)
    parts = [p for p in knight.knight() if p.name != 'forearm_r']
    tmd = build_tmd(parts)
    weapon = arm.WeaponArm(args.data, args.weapon)
    skeleton = floor_banks(args.data, 1)['actor'][5]
    frames = 8
    centre = np.array([0, -950, 0])
    imgs = []
    for yaw in args.yaw:
        for k in range(frames):
            pose, lift = walk.pose(k / frames, args.amount)
            vertices, _ = weapon.state(None)
            M, T = knight.posed(parts, pose, lift)['upper_arm_r']
            flat = flatten(parts, tmd, pose, [weapon.placed(vertices, -(M @ arm.ELBOW + T))], lift)
            imgs.append(kf.render(flat, vr, yaw=yaw, pitch=0.05, size=args.size, extent=1100, center=centre))
        for k in range(frames):
            v = skeleton.pose(1, k * 4096 // frames)
            imgs.append(kf.render(skeleton.tmd, vr, verts=v, yaw=yaw, pitch=0.05, size=args.size, extent=1100,
                                  center=centre))
    return kf.grid(imgs, frames)

def gltf_model(args):
    import gltf
    vr = vram(args.data)
    if args.weapon is None:
        parts = knight.knight()
        flat = flatten(parts, build_tmd(parts), {})
    else:
        parts = [p for p in knight.knight() if p.name != 'forearm_r']
        weapon = arm.WeaponArm(args.data, args.weapon)
        vertices, pitch = weapon.state(args.phase)
        pose = {'upper_arm_r': knight.rot('x', pitch)}
        M, T = knight.posed(parts, pose)['upper_arm_r']
        flat = flatten(parts, build_tmd(parts), pose, [weapon.placed(vertices, -(M @ arm.ELBOW + T))])
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, 'knight.glb')
    gltf.write(flat, vr, path)
    print(path)

# The order of kf::OnlinePlayerPart (port/include/kf/platform/online_players.h).
PART_ORDER = ['pelvis', 'torso', 'head', 'upper_arm_r', 'forearm_r', 'upper_arm_l', 'forearm_l',
              'thigh_r', 'shin_r', 'thigh_l', 'shin_l']

def write_cpp(path):
    parts = knight.knight()
    names = [p.name for p in parts]
    if names != PART_ORDER:
        sys.exit(f"the knight's parts {names} are not in kf::OnlinePlayerPart's order")
    tmd = build_tmd(parts)
    def actor(v):   # builder space to the model's (actor) space
        return '{%d, %d, %d, 0}' % tuple(int(round(-c)) for c in v)
    joints = '\n'.join(f'    {{{names.index(p.parent) if p.parent else -1}, {actor(p.pivot)}}}'
                        f'{"," if i + 1 < len(parts) else ""} // {p.name}' for i, p in enumerate(parts))
    rows = ',\n'.join('    ' + ', '.join(f'0x{b:02x}' for b in tmd[i:i + 16]) for i in range(0, len(tmd), 16))
    text = f'''// Generated by tools/player_model/preview.py cpp: do not edit. The knight other players are drawn
// as (docs/PLAYER_MODEL.md): {len(parts)} parts, {sum(len(p.verts) for p in parts)} vertices, \
{sum(len(p.faces) for p in parts)} primitives.
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

}}

std::span<const u8> online_player_model_tmd() {{
    return knight_tmd;
}}

std::span<const OnlinePlayerPartJoint, online_player_part_count> online_player_model_joints() {{
    return knight_joints;
}}

SVECTOR online_player_model_right_elbow() {{
    return {actor(arm.ELBOW)};
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
    p = sub.add_parser('swing'); p.add_argument('weapon', type=int, choices=range(12))
    p.add_argument('--yaw', type=lambda s: [float(x) for x in s.split(',')], default=[0.8, math.pi / 2])
    p.add_argument('--size', type=int, default=200)
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
    image = {'floor': floor, 'knight': knight_sheet, 'swing': swing, 'walk': walk_sheet}[args.command](args)
    os.makedirs(args.out, exist_ok=True)
    name = {'floor': lambda: f'floor{args.floor}', 'knight': lambda: 'knight',
            'swing': lambda: f'swing{args.weapon:02d}', 'walk': lambda: 'walk'}[args.command]()
    path = os.path.join(args.out, name + '.png')
    kf.save(image, path)
    print(path)

if __name__ == '__main__':
    sys.exit(main())
