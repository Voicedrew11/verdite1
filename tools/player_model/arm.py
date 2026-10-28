"""The weapon in the knight's hand: the game's first-person model (WEPON/WEPxx.MIM: a forearm, a
gauntlet and the weapon, each its own closed piece but the forearm) without its forearm, which is
open at both ends and made for one view. The gauntlet and weapon move as one rigid piece through
the clip; drawn at the hit's size on the end of the knight's own forearm, turned as the clip turns
them. Kept in step with online_player_render.cpp and port/ext/online_weapon_hold.cpp."""
import math, struct
import numpy as np
import kfmodel as kf, knight

ARM_SCALE = 0.5        # the first-person gauntlet and weapon at the knight's size
UPPER_ARM_FOLLOW = 0.7 # of the forearm's lift the upper arm takes through a swing
ATTACK_BLEND = 600     # phase over which the hold turns into the swing, and back at its end
PHASE_END = 4096
HIT_PHASE = 3072       # PLAYER_WEAPON_HIT_PHASE; the Colichemarde hits at 1000
COLICHEMARDE = 3
WRIST = np.array([278, 900, 8.0])   # the right forearm's end, builder space

# At rest the blade points forward and up, its flat to the side (actor space: +y down, -z ahead).
REST_BLADE = np.array([0.0, -0.9, -1.0]) / math.hypot(0.9, 1.0)
REST_SIDE = np.array([1.0, 0.0, 0.0])

def weapon_records(data):
    """The weapon table (COM/COM.DAT chunk 2): 16 records of 44 bytes."""
    table = kf.chunks(open(f'{data}/COM/COM.DAT', 'rb').read())[2]
    out = []
    for i in range(16):
        r = table[i * 44:(i + 1) * 44]
        rot = list(struct.unpack_from('<3h', r, 36))
        rot[1] = -rot[1]   # weapon_records_load_and_mirror_angles
        out.append(dict(rot=rot, translation=struct.unpack_from('<3h', r, 28)))
    return out

def rot_xyz(angles):
    """The game's matrix_set_rotation_xyz: Rx Ry Rz, angles in 4096ths of a turn."""
    x, y, z = (a * 2 * math.pi / 4096 for a in angles)
    return knight.rot('x', x) @ knight.rot('y', y) @ knight.rot('z', z)

# Actor models face -z, the camera +z: the view model's orientation turned a half turn.
HALF_TURN_Y = knight.rot('y', math.pi)

def pieces(obj):
    """The model's closed pieces: vertex lists joined by faces, in order of their first vertex."""
    parent = list(range(len(obj['verts'])))
    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]; i = parent[i]
        return i
    for p in obj['prims']:
        if p['kind'] == 'poly':
            for q in p['verts'][1:]:
                parent[root(q)] = root(p['verts'][0])
    groups = {}
    for i in range(len(parent)):
        groups.setdefault(root(i), []).append(i)
    return sorted(groups.values())

def frame(a, b, c):
    """An orthonormal frame (columns) from three points: a to b, then toward c."""
    x = (b - a) / np.linalg.norm(b - a)
    z = np.cross(x, c - a); z /= np.linalg.norm(z)
    return np.stack([x, np.cross(z, x), z], 1)

def aligned(axes_from, axes_to):
    """The rotation taking two directions onto two others, the first exactly."""
    def basis(first, second):
        x = first / np.linalg.norm(first)
        y = second - x * np.dot(second, x); y /= np.linalg.norm(y)
        return np.stack([x, y, np.cross(x, y)], 1)
    return basis(*axes_to) @ basis(*axes_from).T

def turned_onto(direction, onto):
    """The shortest rotation taking one unit direction onto another."""
    axis = np.cross(direction, onto); s = np.linalg.norm(axis); c = np.dot(direction, onto)
    if s < 1e-9:
        return np.eye(3)
    k = axis / s
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3) + s * K + (1 - c) * K @ K

def slerp(a, b, t):
    """Between two rotations, the short way round."""
    rel = a.T @ b
    angle = math.acos(max(-1.0, min(1.0, (np.trace(rel) - 1) / 2)))
    if angle < 1e-6:
        return b
    k = np.array([rel[2, 1] - rel[1, 2], rel[0, 2] - rel[2, 0], rel[1, 0] - rel[0, 1]]) / (2 * math.sin(angle))
    K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    a_t = angle * t
    return a @ (np.eye(3) + math.sin(a_t) * K + (1 - math.cos(a_t)) * K @ K)

def attack_weight(phase, hit):
    """How far the hold has turned into the swing: in over the blend, out after the hit."""
    if phase is None:
        return 0.0
    if phase < ATTACK_BLEND:
        return phase / ATTACK_BLEND
    return min(1.0, (PHASE_END - phase) / ATTACK_BLEND)

class WeaponHold:
    def __init__(self, data, weapon):
        self.asset = kf.Asset(open(f'{data}/WEPON/WEP{weapon:02d}.MIM', 'rb').read())
        self.view = HALF_TURN_Y @ rot_xyz(weapon_records(data)[weapon]['rot'])
        self.hit = 1000 if weapon == COLICHEMARDE else HIT_PHASE
        obj = self.asset.tmd.objects[0]
        ref = self.asset.pose(0, self.hit).astype(float)
        # The forearm reaches the elbow, the model's origin; the gauntlet is the piece nearest its far end.
        groups = pieces(obj)
        forearm = min(groups, key=lambda g: np.linalg.norm(ref[g], axis=1).min())
        axis = np.linalg.svd(ref[forearm] - ref[forearm].mean(0))[2][0]
        rest = [i for g in groups if g is not forearm for i in g]
        if np.dot(ref[rest].mean(0) - ref[forearm].mean(0), axis) < 0:
            axis = -axis
        along = (ref[forearm] - ref[forearm].mean(0)) @ axis
        wrist = ref[forearm].mean(0) + along.max() * axis
        self.forearm, self.held = forearm, rest
        self.wrist = self.view @ wrist * ARM_SCALE
        # Three vertices to follow: two far apart, the third far from the line between them.
        p = ref[rest]
        i = int(np.argmax(np.linalg.norm(p - p.mean(0), axis=1)))
        j = int(np.argmax(np.linalg.norm(p - p[i], axis=1)))
        line = (p[j] - p[i]) / np.linalg.norm(p[j] - p[i])
        off = (p - p[i]) - np.outer((p - p[i]) @ line, line)
        k = int(np.argmax(np.linalg.norm(off, axis=1)))
        self.follow = [rest[i], rest[j], rest[k]]
        self.ref_frame = self.frame_at(ref)
        self.wrist_from_follow = self.view @ (wrist - ref[self.follow[0]])
        # The blade: from the wrist to the held vertex furthest from it.
        tip = p[int(np.argmax(np.linalg.norm(p - wrist, axis=1)))]
        self.blade = self.view @ ((tip - wrist) / np.linalg.norm(tip - wrist))
        self.rest_turn = aligned((self.blade, self.view @ np.array([1.0, 0, 0])), (REST_BLADE, REST_SIDE))
        # Drawn: the hit's vertices; the forearm's folded onto the wrist, so its faces have no area.
        self.vertices = ref.copy()
        self.vertices[forearm] = wrist

    def frame_at(self, v):
        return frame(*(self.view @ v[i] for i in self.follow))

    def swing(self, phase):
        """(the gauntlet and weapon's rotation, the forearm's) in the knight's body at a phase of
        the clip: the first as the clip turns them, the second from the elbow (the model's origin)
        toward where the clip has the wrist."""
        v = self.asset.pose(0, phase).astype(float)
        turn = self.frame_at(v) @ self.ref_frame.T
        wrist = self.view @ v[self.follow[0]] + turn @ self.wrist_from_follow
        return turn, turned_onto(np.array([0.0, 1.0, 0.0]), wrist / np.linalg.norm(wrist))

    def turn(self, phase):
        """The gauntlet and weapon's rotation in the knight's body at an attack phase (None at rest)."""
        if phase is None:
            return self.rest_turn
        return slerp(self.rest_turn, self.swing(phase)[0], attack_weight(phase, self.hit))

    def placed(self, turn, wrist_game):
        """(vertices, normals, primitives) in actor space, the wrist at wrist_game, for preview.flatten."""
        obj = self.asset.tmd.objects[0]
        M = turn @ self.view
        g = self.vertices * ARM_SCALE @ M.T + (wrist_game - turn @ self.wrist)
        return g, obj['norms'].astype(float) @ M.T, obj['prims']

def upper_arm_follow(forearm):
    """The upper arm's rotation in the body through a swing: lifted forward by UPPER_ARM_FOLLOW of
    the forearm's angle from hanging down (past overhead when it points up and back)."""
    d = forearm @ np.array([0.0, 1.0, 0.0])
    angle = math.atan2(-d[2], d[1])            # forward of hanging down: -z ahead, +y down
    if angle < -math.pi / 4:
        angle += 2 * math.pi
    return knight.rot('x', -UPPER_ARM_FOLLOW * angle)
