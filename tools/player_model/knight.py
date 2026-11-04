"""The knight: its part table (builder space, see body.py) and the rig that poses it."""
import math
import numpy as np
from body import Part, PLAYER

DARK = (24, 22, 20)
LEATHER = 'leather'
HEAD_PIVOT = (0, 1530, 0)

# The helmets, in kf::online_player_helmet_count's order: each builds its pieces on a part that
# follows the head. A new helmet is a new entry here and its name in port/ext/coop_profile.cpp.
def great_helm_ring(y):
    """One section of the great helm at height y: an octagon with its flat to the front, with extra
    corners where the cross's edges cross it. Listed from the upright's left edge going right."""
    profile = ((1565, 116, 128), (1680, 126, 138), (1770, 108, 120), (1826, 58, 64))
    heights, widths, depths = zip(*profile)
    width, depth = np.interp(y, heights, widths), np.interp(y, heights, depths)
    def corner(degrees):
        a = math.radians(degrees)
        return np.array((width * math.sin(a), y, depth * math.cos(a)))
    upright, bar_end = 0.29, 0.55   # across half the flat front; along the next face round
    front_left, front_right = corner(-22.5), corner(22.5)
    middle, half = (front_left + front_right) / 2, (front_right - front_left) / 2
    return [middle - half * upright, middle + half * upright,
            front_right, front_right + (corner(67.5) - front_right) * bar_end, corner(67.5),
            corner(112.5), corner(157.5), corner(202.5), corner(247.5), corner(292.5),
            front_left + (corner(292.5) - front_left) * bar_end, front_left]

def great_helm(helm, colour):
    """A great helm with a cross painted on its visor, a crest in the player's colour."""
    M = 'knight'
    # The cross is the shell's own faces, not quads laid over it: in the game's depth order the
    # helm's larger faces would draw over those from above and below.
    bar, upright = {0, 1, 2, 10, 11}, {0}
    painted = {1: upright, 2: bar, 3: upright}   # by the ring below: 1606 to 1664, the bar, to 1730
    rings = [great_helm_ring(y) for y in (1565, 1606, 1664, 1688, 1730, 1770, 1826)]
    helm.shell(rings, lambda k, i: DARK if i in painted.get(k, ()) else M, lambda k, i: 0, caps=(M, M))
    helm.loft([((0, 1770, -124), 15, 44), ((0, 1846, -22), 15, 50), ((0, 1856, 76), 15, 40), ((0, 1796, 130), 11, 22)],
              4, colour, axis='z', phase=math.pi / 4)

def mirrored(right):
    """A ring from its right half, listed from the middle out: the left half follows, back to the
    middle. Points on the middle line appear once."""
    left = [(-x, y, z) for x, y, z in reversed(right) if x != 0]
    return list(right) + left

def fluted_skull_ring(z, width, top, bottom):
    """One section of the fluted helm's skull, across the head at z: a comb, two flutes stepping
    out over the side, down to the cheek."""
    spring = 1660
    def on_dome(degrees, out):
        a = math.radians(degrees)
        return (width * out * math.sin(a), spring + (top - spring) * out * math.cos(a), z)
    return mirrored([on_dome(5, 1.08), on_dome(10, 0.97), on_dome(30, 1.03), on_dome(36, 0.95),
                     on_dome(58, 1.03), on_dome(64, 0.95), on_dome(90, 1.0), (width * 0.88, bottom, z)])

def fluted_helm(helm, colour):
    """The fluted helm, within what a PS1 model would spend: a dome with two flutes down each side,
    a comb pointing between angry eyes, a short duckbill, and breaths painted on."""
    M = 'knight'
    # Skull, back to front: the step of each flute is dark, the comb the player's colour.
    rings = [fluted_skull_ring(*section) for section in
             ((-180, 24, 1692, 1644), (-134, 92, 1786, 1570), (-46, 130, 1838, 1546),
              (46, 128, 1830, 1546), (130, 90, 1782, 1578), (176, 28, 1698, 1650))]
    def skull_mat(k, i):
        if i in (0, 14, 15): return colour
        return DARK if i in (2, 4, 10, 12) and 1 <= k <= 3 else M   # the flutes stop short of each end
    helm.shell(rings, skull_mat, lambda k, i: i, caps=(M, M))

    # The neck: flaring at the foot, the nape running on from the skull's back. The body hides its ends.
    helm.loft([((0, 1528, 0), 116, 128), ((0, 1600, 0), 104, 106)], 6, M, caps=(False, False), phase=0)

    # The bill, out from under the forehead: thin and broad at its end. The skull hides its root.
    helm.loft([((0, 1642, 112), 84, 44), ((0, 1626, 212), 76, 24), ((0, 1612, 268), 44, 8)],
              6, M, caps=(False, True), axis='z', phase=math.pi / 6)

    # Breaths on the bill, seen from above, then eye holes on the forehead, seen from ahead: a decal
    # lands on what is built so far, so these do not land on each other. Lifted clear of the surface:
    # the faces bulge between their corners.
    forward, up = (0, 0, 1), (0, 1, 0)
    for s in (1, -1):
        for x in (14, 34):
            helm.decal([(s * x, 2000, 232), (s * (x + 8), 2000, 232), (s * (x - 2), 2000, 178), (s * (x + 6), 2000, 178)], DARK, up, lift=6)
        helm.decal([(s * 12, 1700, 400), (s * 32, 1720, 400), (s * 32, 1690, 400), (s * 54, 1706, 400)], DARK, forward, lift=7)

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
