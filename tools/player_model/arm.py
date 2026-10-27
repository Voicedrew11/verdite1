"""The weapon arm: the game's first-person model (WEPON/WEPxx.MIM, a forearm and gauntlet holding
the weapon) posed at an attack phase and placed on the knight's elbow."""
import math, struct
import numpy as np
import kfmodel as kf, knight

ARM_SCALE = 0.5        # the first-person forearm is about twice a 1750-tall body's
UPPER_ARM_RAISE = -0.9 # radians the right upper arm lifts forward during a swing
ATTACK_BLEND = 600     # phase over which the guard blends into the swing's first frame
PHASE_END = 4096
HIT_PHASE = 3072       # PLAYER_WEAPON_HIT_PHASE; the Colichemarde hits at 1000
COLICHEMARDE = 3
ELBOW = np.array([278, 1160, 0.0])  # the right upper arm's end, builder space

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

class WeaponArm:
    def __init__(self, data, weapon):
        self.asset = kf.Asset(open(f'{data}/WEPON/WEP{weapon:02d}.MIM', 'rb').read())
        self.rotation = HALF_TURN_Y @ rot_xyz(weapon_records(data)[weapon]['rot'])
        self.hit = 1000 if weapon == COLICHEMARDE else HIT_PHASE
        self.guard = self.asset.pose(0, PHASE_END - 1)   # the swing's last frame: a low guard

    def state(self, phase):
        """(model vertices, upper arm pitch) at an attack phase, or at rest for None."""
        if phase is None:
            return self.guard, 0.0
        v = self.asset.pose(0, phase)
        if phase < ATTACK_BLEND:
            t = phase / ATTACK_BLEND
            return self.guard + (v - self.guard) * t, UPPER_ARM_RAISE * t
        if phase < self.hit:
            return v, UPPER_ARM_RAISE
        return v, UPPER_ARM_RAISE * max(0.0, 1 - (phase - self.hit) / (PHASE_END - self.hit))

    def placed(self, vertices, elbow_game):
        """(vertices, normals, primitives) in actor space, for preview.flatten."""
        obj = self.asset.tmd.objects[0]
        g = vertices.astype(float) * ARM_SCALE @ self.rotation.T + elbow_game
        return g, obj['norms'].astype(float) @ self.rotation.T, obj['prims']
