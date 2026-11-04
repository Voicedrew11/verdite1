"""The knight: its part table (builder space, see body.py) and the rig that poses it."""
import math
import numpy as np
from body import Part, PLAYER

DARK = (24, 22, 20)
LEATHER = 'leather'
HEAD_PIVOT = (0, 1530, 0)

# The helmets, in kf::online_player_helmet_count's order: each builds its pieces on a part that
# follows the head. A new helmet is a new entry here and its name in port/ext/coop_profile.cpp.
def great_helm(helm, colour):
    """A great helm with a T visor, a crest in the player's colour."""
    M = 'knight'
    helm.loft([((0, 1565, 0), 116, 128), ((0, 1680, 0), 126, 138), ((0, 1770, 0), 108, 120), ((0, 1826, 0), 58, 64)], 8, M)
    helm.box((-86, 1664, 120), (86, 1688, 140), DARK)
    helm.box((-14, 1606, 126), (14, 1668, 142), DARK)
    helm.loft([((0, 1770, -124), 15, 44), ((0, 1846, -22), 15, 50), ((0, 1856, 76), 15, 40), ((0, 1796, 130), 11, 22)],
              4, colour, axis='z', phase=math.pi / 4)

def mirrored(right):
    """A ring from its right half, listed from the middle out: the left half follows, back to the
    middle. Points on the middle line appear once."""
    left = [(-x, y, z) for x, y, z in reversed(right) if x != 0]
    return list(right) + left

def fluted_skull_ring(z, width, top, bottom):
    """One section of the fluted helm's skull, across the head at z: a comb, then three flutes
    each stepping out over the next, down to the cheek."""
    spring = 1660
    def on_dome(degrees, out):
        a = math.radians(degrees)
        return (width * out * math.sin(a), spring + (top - spring) * out * math.cos(a), z)
    return mirrored([on_dome(6, 1.05), on_dome(9, 0.99), on_dome(32, 1.03), on_dome(37, 0.97),
                     on_dome(60, 1.03), on_dome(65, 0.97), on_dome(92, 1.02), (width * 0.86, bottom, z)])

def fluted_helm(helm, colour):
    """The fluted helm: a skull ribbed front to back, a flame-edged brow, and a visor drawn out to a
    beak in three tiers, the dark undercuts between them its breaths."""
    M = 'knight'
    # Skull, back to front: the steps between the flutes are dark, drawing the helm's concentric
    # arcs from the side; the comb is the player's colour, as the game's own knight has a fin.
    rings = [fluted_skull_ring(*section) for section in
             ((-146, 66, 1716, 1600), (-112, 118, 1796, 1556), (-40, 130, 1838, 1546),
              (50, 128, 1828, 1546), (118, 100, 1800, 1580))]
    def skull_mat(k, i):
        if i in (0, 14, 15): return colour
        return DARK if i in (2, 4, 10, 12) else M
    helm.shell(rings, skull_mat, lambda k, i: i, caps=(M, DARK))

    # The neck: flaring at the foot, the nape running on from the skull's back.
    helm.loft([((0, 1528, 26), 118, 156), ((0, 1600, 16), 108, 130)], 8, M, caps=(True, False))

    # The brow: a plate over the skull's front, peaked in the middle, flaring at the temples.
    columns = ((0, 132), (56, 122), (104, 80), (128, 26))
    brow_low = (1726, 1756, 1738, 1722)
    brow_high = (1812, 1782, 1776, 1792)
    helm.shell([mirrored([(x, y, z) for (x, z), y in zip(columns, heights)]) for heights in (brow_low, brow_high)],
               lambda k, i: M, lambda k, i: i)

    # The visor, chin to rim: each level's beak (z in the middle), then the sides fall back to the
    # hinge. The faces between the levels marked dark are the undercuts: the breaths.
    levels = ((1548, 160, 0.70), (1586, 196, 0.90), (1598, 172, 0.90), (1628, 226, 0.96),
              (1640, 200, 0.96), (1672, 262, 1.0), (1692, 150, 1.0))
    breaths = {1, 3}
    def visor_ring(y, tip, wide):
        face = 124   # just before the skull's dark front
        rim = y >= 1690
        up = (y - 1548) / (1692 - 1548)   # the cheek's back edge slants up from the chin to the hinge
        return mirrored([(0, y + (26 if rim else 0), tip - (8 if rim else 0)),
                         (54 * wide, y - (12 if rim else 0), face + (tip - face) * 0.52),
                         (108 * wide, y, face + (tip - face) * 0.16),
                         (126 + 22 * up, y + (54 if rim else 0), 104 - 112 * up)])
    def visor_mat(k, i):
        if i in (2, 3, 4): return 'plate'   # the cheeks, a darker metal, set the visor off the skull
        return DARK if k in breaths else M
    helm.shell([visor_ring(*level) for level in levels], visor_mat, lambda k, i: (k, i), caps=(M, M))

HELMETS = [great_helm, fluted_helm]

def helmets(colour=PLAYER):
    """Each helmet as a part of its own on the head's pivot."""
    out = []
    for i, build in enumerate(HELMETS):
        helm = Part(f'helmet_{i}', 'head', HEAD_PIVOT)
        build(helm, colour)
        out.append(helm)
    return out

def knight(helmet=0, colour=PLAYER):
    """The knight's parts; with a helmet (an index of HELMETS) it follows them, as a picture
    shows it; with None the parts alone, as the game's model has them."""
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

    # Head: the mail coif; the helmet is a part of its own (helmets()).
    head = part('head', 'torso', HEAD_PIVOT)
    head.loft([((0, 1500, 0), 80, 80), ((0, 1580, 0), 78, 80)], 6, M, caps=(False, False))

    # Arms: a sleeve in the player's colour from the shoulder, a flared gauntlet from the elbow.
    # The hands are parts of their own, after the legs: the right one gives way to a weapon's.
    for side, s in (('r', 1), ('l', -1)):
        upper = part('upper_arm_' + side, 'torso', (s * 272, 1455, 0))
        upper.loft([((s * 272, 1460, 0), 82, 84), ((s * 276, 1310, 0), 78, 80), ((s * 278, 1170, 0), 72, 74)], 6, colour)
        fore = part('forearm_' + side, 'upper_arm_' + side, (s * 278, 1160, 0))
        fore.loft([((s * 278, 1175, 0), 76, 78), ((s * 278, 1060, 6), 78, 80), ((s * 278, 940, 8), 64, 66),
                   ((s * 278, 900, 8), 86, 88)], 6, 'plate')

    # Legs: thigh, knee cop, greave, boot.
    for side, s in (('r', 1), ('l', -1)):
        thigh = part('thigh_' + side, 'pelvis', (s * 108, 940, 0))
        thigh.loft([((s * 108, 970, 0), 100, 106), ((s * 112, 740, 4), 92, 98), ((s * 114, 540, 6), 78, 84)], 6, M)
        shin = part('shin_' + side, 'thigh_' + side, (s * 114, 525, 0))
        shin.loft([((s * 114, 575, 12), 74, 80), ((s * 114, 515, 20), 82, 88), ((s * 114, 455, 12), 74, 80)], 6, colour)
        shin.loft([((s * 114, 470, 4), 76, 84), ((s * 114, 310, -4), 74, 86), ((s * 114, 140, 0), 60, 66)], 6, M)
        shin.loft([((s * 114, 84, -78), 64, 84), ((s * 114, 74, 44), 66, 72), ((s * 114, 48, 186), 52, 46)],
                  6, LEATHER, axis='z', phase=0)

    for side, s in (('r', 1), ('l', -1)):
        hand = part('hand_' + side, 'forearm_' + side, (s * 278, 900, 8))
        hand.loft([((s * 278, 900, 10), 54, 46), ((s * 278, 820, 14), 52, 40), ((s * 278, 755, 18), 40, 30)], 6, 'plate')
    if helmet is not None:
        parts.append(helmets(colour)[helmet])
    return parts

# ---- rig: part transforms about pivots, parents first ------------------------------------
def rot(ax, a):
    c, s = math.cos(a), math.sin(a)
    if ax == 'x': return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    if ax == 'y': return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])

def posed(parts, pose, lift=0.0):
    """World (builder space) vertices per part. pose: name -> 3x3 rotation about the pivot,
    in the parent's frame; lift raises the whole body (negative lowers it)."""
    world = {}
    for p in parts:
        R = pose.get(p.name, np.eye(3))
        if p.parent:
            PR, PT = world[p.parent]
            # The pivot moves with the parent: x' = PR (pivot + R (x - pivot)) + PT
            M = PR @ R; T = PR @ p.pivot + PT - M @ p.pivot
        else:
            M = R; T = p.pivot - R @ p.pivot + np.array([0, lift, 0])
        world[p.name] = (M, T)
    return world
