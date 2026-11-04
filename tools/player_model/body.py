"""The online player's body: a segmented knight built from a table of lofted parts, written as a TMD.

Builder coordinates: X right, Y up, Z forward, feet at Y = 0, in game units (1700 = the player's
collision height). The game's actor space is right = -x, up = -y, forward = -z, so a point is
written as (-X, -Y, -Z). Each part is its own TMD object, its vertices relative to its pivot.
"""
import math, struct
import numpy as np

# Materials: always-resident common textures (COM/MIX.TIM), flat colours (r, g, b), or PLAYER: a
# flat colour the game paints in each player's own (port/ext/coop_profile.h), written as the first.
PLAYER = 'player'
PLAYER_DEFAULT = (60, 84, 180)
TPAGE_142, CLUT_142 = 142, 31296   # 8-bit page at (896, 0), palette row 489: the weapon arm's
TPAGE_151, CLUT_151 = 151, 31552   # 8-bit page at (448, 256), palette row 493: the NPC knights'
MATERIALS = {
    'plate':   dict(tpage=TPAGE_142, clut=CLUT_142, u=(1, 62), v=(193, 254)),   # the arm's metal
    'mail':    dict(tpage=TPAGE_142, clut=CLUT_142, u=(65, 126), v=(193, 254)),  # chainmail
    'knight':  dict(tpage=TPAGE_151, clut=CLUT_151, u=(65, 126), v=(129, 254)),  # NPC knights' metal
    'leather': dict(tpage=TPAGE_151, clut=CLUT_151, u=(193, 254), v=(131, 185)),
}
# The box the metal's side view is projected across: the knight with its great helm. A constant, so
# a new helmet moves no texture on the others; what reaches past it takes the box's edge.
PROJECTION_LO = np.array((-364.0, 0.0, -169.98489853368096))
PROJECTION_HI = np.array((364.0, 1884.284271247462, 186.0))

class Part:
    def __init__(self, name, parent, pivot):
        self.name, self.parent, self.pivot = name, parent, np.array(pivot, float)
        self.verts = []
        self.faces = []   # (vertex ids, material, outward reference point, smoothing group)
        self.groups = 0

    def vert(self, p):
        self.verts.append(np.array(p, float)); return len(self.verts) - 1

    def face(self, ids, mat, inside, group):
        P = [self.verts[i] for i in ids]
        n = np.cross(P[1] - P[0], P[2] - P[0])
        if np.linalg.norm(n) < 1e-6:
            return
        # Builder space is the game's actor space mirrored through the origin, so the game keeps
        # a face whose cross(v1 - v0, v2 - v0) points outward here (it is inward in actor space).
        if np.dot(n, np.mean(P, axis=0) - inside) < 0:
            ids = [ids[1], ids[0], ids[3], ids[2]] if len(ids) == 4 else [ids[0], ids[2], ids[1]]
        self.faces.append((list(ids), mat, group))

    def group(self):
        self.groups += 1; return self.groups

    def loft(self, sections, n, mat, caps=(True, True), cap_mat=None, axis='y', phase=None):
        """A tube through sections (centre (X, Y, Z), half-width, half-depth); its cross-section is
        an n-gon in the plane across the axis ('y': X-Z, 'x': Y-Z, 'z': X-Y)."""
        if phase is None: phase = math.pi / n
        rings, centres = [], []
        for centre, w, d in sections:
            ids = []
            for i in range(n):
                t = phase + 2 * math.pi * i / n
                a, b = w * math.sin(t), d * math.cos(t)
                off = {'y': (a, 0, b), 'x': (0, a, b), 'z': (a, b, 0)}[axis]
                ids.append(self.vert(np.add(centre, off)))
            rings.append(ids); centres.append(np.array(centre, float))
        side = self.group()
        for k in range(len(rings) - 1):
            inside = (centres[k] + centres[k + 1]) / 2
            for i in range(n):
                j = (i + 1) % n
                self.face([rings[k][i], rings[k][j], rings[k + 1][i], rings[k + 1][j]], mat, inside, side)
        whole = np.mean(centres, axis=0)
        for r, c, on in ((rings[0], centres[0], caps[0]), (rings[-1], centres[-1], caps[1])):
            if not on: continue
            g = self.group()
            inside = c + (whole - c) * 0.5 if len(centres) > 1 else c
            if np.allclose(inside, c): inside = c - np.array({'y': (0, 1, 0), 'x': (1, 0, 0), 'z': (0, 0, 1)}[axis])
            mid = self.vert(c)
            for i in range(n):
                self.face([mid, r[i], r[(i + 1) % n]], cap_mat or mat, inside, g)
        return rings

    def shell(self, rings, mat, group, caps=(None, None)):
        """A closed tube through rings of points, all the same length, for sections a loft's n-gon
        cannot make. mat(k, i) and group(k, i) name the material and smoothing group of the face
        from ring k to the next, corner i to the next; caps are the end rings' materials, None for
        an end left open because the helmet hides it."""
        ids = [[self.vert(p) for p in ring] for ring in rings]
        groups = {}
        def smoothing(key):
            if key not in groups: groups[key] = self.group()
            return groups[key]
        faces = []
        for k in range(len(ids) - 1):
            for i in range(len(ids[k])):
                j = (i + 1) % len(ids[k])
                faces.append(([ids[k][i], ids[k][j], ids[k + 1][i], ids[k + 1][j]], mat(k, i), smoothing(group(k, i))))
        for ring, cap, first in ((ids[0], caps[0], True), (ids[-1], caps[1], False)):
            if cap is None: continue
            mid = self.vert(np.mean([self.verts[i] for i in ring], axis=0))
            end = smoothing(('cap', first))
            for i in range(len(ring)):
                a, b = ring[i], ring[(i + 1) % len(ring)]
                faces.append(([mid, b, a] if first else [mid, a, b], cap, end))
        # Rings may go either way round: the volume's sign says which way is outward.
        middle = np.mean([self.verts[i] for f, _, _ in faces for i in f], axis=0)
        volume = 0.0
        for f, _, _ in faces:
            P = [self.verts[i] - middle for i in f]
            volume += np.dot(P[0], np.cross(P[1], P[2]))
            if len(f) == 4: volume += np.dot(P[1], np.cross(P[3], P[2]))
        for f, m, g in faces:
            if volume < 0:
                f = [f[1], f[0], f[3], f[2]] if len(f) == 4 else [f[0], f[2], f[1]]
            self.faces.append((f, m, g))

    def box(self, lo, hi, mat):
        x0, y0, z0 = lo; x1, y1, z1 = hi
        v = [self.vert((x, y, z)) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]
        inside = (np.array(lo, float) + np.array(hi, float)) / 2
        for q in ((0, 1, 2, 3), (4, 6, 5, 7), (0, 4, 1, 5), (2, 3, 6, 7), (0, 2, 4, 6), (1, 5, 3, 7)):
            self.face([v[i] for i in q], mat, inside, self.group())

    def panel(self, corners, thickness, mat, back_mat=None):
        """A thin quad slab: corners are the front face in strip order (top-left, top-right,
        bottom-left, bottom-right as seen from the front); the back is offset by thickness."""
        P = [np.array(c, float) for c in corners]
        n = np.cross(P[1] - P[0], P[2] - P[0]); n /= np.linalg.norm(n)
        front = [self.vert(p) for p in P]
        back = [self.vert(p - n * thickness) for p in P]
        inside = np.mean(P, axis=0) - n * thickness / 2
        self.face(front, mat, inside, self.group())
        self.face(back, back_mat or mat, inside, self.group())
        for a, b in ((0, 1), (1, 3), (3, 2), (2, 0)):
            self.face([front[a], front[b], back[a], back[b]], back_mat or mat, inside, self.group())

# ---- writer ---------------------------------------------------------------------------------
def to_game(p):
    return np.array((-p[0], -p[1], -p[2]))

def build_tmd_with_colours(parts):
    # Textures are projected once across the whole body, as the game's NPC knights are.
    objects = []
    for part in parts:
        verts = [to_game(v - part.pivot) for v in part.verts]
        face_normal = []
        acc = {}
        for ids, mat, g in part.faces:
            P = [verts[i] for i in ids]
            n = -np.cross(P[1] - P[0], P[2] - P[0])   # the game's outward normal
            n /= np.linalg.norm(n)
            face_normal.append(n)
            for i in ids: acc[(i, g)] = acc.get((i, g), 0) + n
        normals, index = [], {}
        def normal_id(n):
            n = n / (np.linalg.norm(n) or 1)
            key = tuple(int(round(c * 4096)) for c in n)
            if key not in index: index[key] = len(normals); normals.append(key)
            return index[key]
        prims = []
        for (ids, mat, g), fn in zip(part.faces, face_normal):
            ns = [normal_id(acc[(i, g)]) for i in ids]
            if mat == PLAYER:
                prims.append(('col', ids, ns, PLAYER_DEFAULT, True))
            elif isinstance(mat, str):
                m = MATERIALS[mat]
                W = [part.verts[i] for i in ids]
                # A side view for every face (U from depth, V from height): front faces smear one
                # texel column across, the streaked metal of the game's own knights.
                span = PROJECTION_HI - PROJECTION_LO
                uv = []
                for w in W:
                    a = min(max((w[2] - PROJECTION_LO[2]) / span[2], 0.0), 1.0)
                    b = min(max((PROJECTION_HI[1] - w[1]) / span[1], 0.0), 1.0)
                    uv.append((int(round(m['u'][0] + a * (m['u'][1] - m['u'][0]))),
                               int(round(m['v'][0] + b * (m['v'][1] - m['v'][0])))))
                prims.append(('tex', ids, ns, uv, m['tpage'], m['clut']))
            else:
                prims.append(('col', ids, ns, mat, False))
        objects.append(([tuple(int(round(c)) for c in v) for v in verts], normals, prims))
    return write_tmd(objects)

def build_tmd(parts):
    return build_tmd_with_colours(parts)[0]

def write_tmd(objects):
    """The TMD, and the offsets of the R, G, B of each primitive in the player's colour."""
    header = 12
    table_size = 28 * len(objects)
    blobs, table, cursor = [], b'', table_size
    colours = []
    for verts, normals, prims in objects:
        vb = b''.join(struct.pack('<4h', *v, 0) for v in verts)
        nb = b''.join(struct.pack('<4h', *n, 0) for n in normals)
        pb = b''
        for p in prims:
            ids, ns = p[1], p[2]
            n = len(ids)
            if p[0] == 'tex':
                uv, tpage, clut = p[3], p[4], p[5]
                mode = 0x3c if n == 4 else 0x34
                body = b''.join(struct.pack('<BBH', uv[i][0], uv[i][1], clut if i == 0 else tpage if i == 1 else 0) for i in range(n))
            else:
                mode = 0x38 if n == 4 else 0x30
                body = struct.pack('<4B', *p[3], mode)
                if p[4]:
                    colours.append(header + cursor + len(vb) + len(nb) + len(pb) + 4)
            body += b''.join(struct.pack('<HH', ns[i], ids[i]) for i in range(n))
            pb += struct.pack('<4B', len(body) // 4 + 1, len(body) // 4, 0, mode) + body
        table += struct.pack('<6Ii', cursor, len(verts), cursor + len(vb), len(normals),
                             cursor + len(vb) + len(nb), len(prims), 0)
        cursor += len(vb) + len(nb) + len(pb)
        blobs += [vb, nb, pb]
    return struct.pack('<3I', 0x41, 0, len(objects)) + table + b''.join(blobs), colours
