"""The knight's walk: legs and the free arm posed through a cycle, shaped on the game's own
bipedal walks (the skeleton's, B1 actor model 5), from a ready stance when standing. Kept in step
with online_player_render.cpp."""
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

# Standing, ready: degrees, each part's pitch (forward positive) and roll (out from the body).
# The left foot a little ahead, the right behind, knees soft, feet apart; the free arm bent.
STANCE_THIGH = {'r': (-7, 6), 'l': (9, 6)}
STANCE_KNEE = {'r': 9, 'l': 13}
STANCE_UPPER_ARM_L = (8, 7)
STANCE_FOREARM_L = 30
# The weapon arm at rest, standing or walking: the upper arm a little forward and out, the forearm
# forward past level so the hand is in front of the belt.
REST_UPPER_ARM_R = (18, 9)
REST_FOREARM_R = 68
BREATH_SECONDS = 3.6
BREATH_TORSO = 0.8             # degrees the torso leans back and forth
BREATH_ARMS = 1.5              # degrees the arms rise and settle with it

def turn(pitch, roll=0.0, side=1):
    """A part's rotation: forward by pitch, then out by roll on that side (degrees)."""
    return knight.rot('x', -math.radians(pitch)) @ knight.rot('z', side * math.radians(roll))

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

def mix(stance, walking, amount):
    return stance + (walking - stance) * amount

def pose(phase, amount=1.0, breath=0.0):
    """(pose, lift) for knight.posed at a phase of the cycle (the right foot leaves the ground at
    0, the left at 0.5), from the stance by amount (0 standing, 1 a full stride); breath is 0..1
    through a breath. The weapon arm is the hold's (arm.py), at rest here."""
    still = 1.0 - amount
    legs = {}
    gait = {}
    out = {}
    for side, s, at in (('r', 1, phase), ('l', -1, phase + 0.5)):
        thigh, knee = gait[side] = leg(at)
        pitch, roll = STANCE_THIGH[side]
        legs[side] = (mix(pitch, thigh, amount), mix(STANCE_KNEE[side], knee, amount))
        out['thigh_' + side] = turn(legs[side][0], roll * still, s)
        out['shin_' + side] = knight.rot('x', math.radians(legs[side][1]))
    rise = math.sin(2 * math.pi * breath)
    out['torso'] = knight.rot('x', math.radians(BREATH_TORSO * rise * still))
    swing = ARM_SWING * (gait['r'][0] - GAIT_MIDDLE) * amount
    out['upper_arm_l'] = turn(STANCE_UPPER_ARM_L[0] * still + swing + BREATH_ARMS * rise * still,
                              STANCE_UPPER_ARM_L[1] * still, -1)
    out['forearm_l'] = turn(mix(STANCE_FOREARM_L, FOREARM_BEND, amount))
    out['upper_arm_r'] = turn(REST_UPPER_ARM_R[0] + BREATH_ARMS * rise * still, REST_UPPER_ARM_R[1], 1)
    out['forearm_r'] = turn(REST_FOREARM_R)
    standing = THIGH + SHIN
    lift = max(reach(*legs['r'])[0], reach(*legs['l'])[0]) - standing
    return out, lift
