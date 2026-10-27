"""The knight's walk: legs and the free arm posed through a cycle, shaped on the game's own
bipedal walks (the skeleton's, B1 actor model 5). Kept in step with online_player_render.cpp."""
import math
import knight

# One leg through a cycle, from the moment its foot leaves the ground behind: (phase, thigh
# forward of vertical, knee bend), degrees, linear between keys as the game's keyframes are. The
# skeleton's own: the thigh swings forward fast to a reach past the contact angle, then settles
# on it and sweeps back through the stance over half the cycle.
GAIT = [(0.0, -22, 14), (0.25, 42, 50), (0.5, 30, 12), (0.75, 4, 16)]
THIGH, SHIN = 415.0, 525.0     # hip to knee, knee to the sole (the knight's pivots: 940, 525, 0)
ARM_SWING = 0.4                # of the opposite thigh's swing about its middle
GAIT_MIDDLE = 10.0             # the thigh's middle angle
FOREARM_BEND = 12.0

def leg(phase):
    phase %= 1.0
    for i, (p0, t0, k0) in enumerate(GAIT):
        p1, t1, k1 = GAIT[(i + 1) % len(GAIT)]
        p1 = p1 if p1 > p0 else 1.0
        if phase < p1:
            f = (phase - p0) / (p1 - p0)
            return t0 + (t1 - t0) * f, k0 + (k1 - k0) * f
    raise AssertionError

def reach(thigh, knee):
    """The sole's height below and distance ahead of the hip."""
    t, s = math.radians(thigh), math.radians(thigh - knee)
    return THIGH * math.cos(t) + SHIN * math.cos(s), THIGH * math.sin(t) + SHIN * math.sin(s)

def pose(phase, amount=1.0):
    """(pose, lift) for knight.posed at a phase of the cycle (the right foot leaves the ground at
    0, the left at 0.5), scaled by amount (0 standing, 1 a full stride)."""
    right = [a * amount for a in leg(phase)]
    left = [a * amount for a in leg(phase + 0.5)]
    out = {}
    for side, (thigh, knee) in (('r', right), ('l', left)):
        out['thigh_' + side] = knight.rot('x', -math.radians(thigh))
        out['shin_' + side] = knight.rot('x', math.radians(knee))
    out['upper_arm_l'] = knight.rot('x', -math.radians(ARM_SWING * (right[0] - GAIT_MIDDLE * amount)))
    out['forearm_l'] = knight.rot('x', -math.radians(FOREARM_BEND * amount))
    standing = THIGH + SHIN
    lift = max(reach(*right)[0], reach(*left)[0]) - standing
    return out, lift
