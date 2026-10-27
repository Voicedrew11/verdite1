"""The knight: its part table (builder space, see body.py) and the rig that poses it."""
import math
import numpy as np
from body import Part

DARK = (24, 22, 20)
LEATHER = 'leather'

def knight(colour=(60, 84, 180)):
    parts = []
    def part(name, parent, pivot):
        p = Part(name, parent, pivot); parts.append(p); return p
    M = 'knight'   # the NPC knights' grey metal

    # Pelvis: the root. Faulds, a leather belt, the tabard's skirt front and back.
    pelvis = part('pelvis', None, (0, 960, 0))
    pelvis.loft([((0, 850, 0), 175, 118), ((0, 960, 0), 192, 126), ((0, 1070, 0), 182, 122)], 8, M, caps=(True, False))
    pelvis.loft([((0, 1040, 0), 196, 132), ((0, 1100, 0), 192, 130)], 8, LEATHER)
    pelvis.panel([(-135, 1050, 138), (135, 1050, 138), (-165, 590, 158), (165, 590, 158)], 16, colour)
    pelvis.panel([(135, 1050, -134), (-135, 1050, -134), (165, 590, -154), (-165, 590, -154)], 16, colour)

    # Torso: breastplate, the tabard's chest, pauldrons.
    torso = part('torso', 'pelvis', (0, 1080, 0))
    torso.loft([((0, 1075, 0), 182, 122), ((0, 1220, 6), 212, 140), ((0, 1380, 8), 236, 146),
                ((0, 1480, 0), 214, 126), ((0, 1530, 0), 100, 78)], 8, M)
    torso.panel([(-130, 1420, 152), (130, 1420, 152), (-134, 1090, 131), (134, 1090, 131)], 10, colour)
    torso.panel([(130, 1425, -136), (-130, 1425, -136), (134, 1090, -126), (-134, 1090, -126)], 10, colour)
    for s in (1, -1):
        torso.loft([((s * 190, 1470, 0), 108, 138), ((s * 290, 1485, 0), 104, 134), ((s * 356, 1440, 0), 70, 96)],
                   6, M, axis='x', phase=0)

    # Head: mail coif, helm with a T visor, a crest in the player's colour.
    head = part('head', 'torso', (0, 1530, 0))
    head.loft([((0, 1500, 0), 80, 80), ((0, 1580, 0), 78, 80)], 6, M, caps=(False, False))
    head.loft([((0, 1565, 0), 116, 128), ((0, 1680, 0), 126, 138), ((0, 1770, 0), 108, 120), ((0, 1826, 0), 58, 64)], 8, M)
    head.box((-86, 1664, 120), (86, 1688, 140), DARK)
    head.box((-14, 1606, 126), (14, 1668, 142), DARK)
    head.loft([((0, 1770, -124), 15, 44), ((0, 1846, -22), 15, 50), ((0, 1856, 76), 15, 40), ((0, 1796, 130), 11, 22)],
              4, colour, axis='z', phase=math.pi / 4)

    # Arms: a sleeve in the player's colour from the shoulder, a flared gauntlet from the elbow.
    for side, s in (('r', 1), ('l', -1)):
        upper = part('upper_arm_' + side, 'torso', (s * 272, 1455, 0))
        upper.loft([((s * 272, 1460, 0), 82, 84), ((s * 276, 1310, 0), 78, 80), ((s * 278, 1170, 0), 72, 74)], 6, colour)
        fore = part('forearm_' + side, 'upper_arm_' + side, (s * 278, 1160, 0))
        fore.loft([((s * 278, 1175, 0), 76, 78), ((s * 278, 1060, 6), 78, 80), ((s * 278, 940, 8), 64, 66),
                   ((s * 278, 900, 8), 86, 88)], 6, 'plate')
        fore.loft([((s * 278, 900, 10), 54, 46), ((s * 278, 820, 14), 52, 40), ((s * 278, 755, 18), 40, 30)], 6, 'plate')

    # Legs: thigh, knee cop, greave, boot.
    for side, s in (('r', 1), ('l', -1)):
        thigh = part('thigh_' + side, 'pelvis', (s * 108, 940, 0))
        thigh.loft([((s * 108, 970, 0), 100, 106), ((s * 112, 740, 4), 92, 98), ((s * 114, 540, 6), 78, 84)], 6, M)
        shin = part('shin_' + side, 'thigh_' + side, (s * 114, 525, 0))
        shin.loft([((s * 114, 575, 12), 74, 80), ((s * 114, 515, 20), 82, 88), ((s * 114, 455, 12), 74, 80)], 6, colour)
        shin.loft([((s * 114, 470, 4), 76, 84), ((s * 114, 310, -4), 74, 86), ((s * 114, 140, 0), 60, 66)], 6, M)
        shin.loft([((s * 114, 84, -78), 64, 84), ((s * 114, 74, 44), 66, 72), ((s * 114, 48, 186), 52, 46)],
                  6, LEATHER, axis='z', phase=0)
    return parts

# ---- rig: part transforms about pivots, parents first ------------------------------------
def rot(ax, a):
    c, s = math.cos(a), math.sin(a)
    if ax == 'x': return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    if ax == 'y': return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])

def posed(parts, pose):
    """World (builder space) vertices per part. pose: name -> 3x3 rotation about the pivot,
    in the parent's frame."""
    world = {}
    for p in parts:
        R = pose.get(p.name, np.eye(3))
        if p.parent:
            PR, PT = world[p.parent]
            # The pivot moves with the parent: x' = PR (pivot + R (x - pivot)) + PT
            M = PR @ R; T = PR @ p.pivot + PT - M @ p.pivot
        else:
            M = R; T = p.pivot - R @ p.pivot
        world[p.name] = (M, T)
    return world
