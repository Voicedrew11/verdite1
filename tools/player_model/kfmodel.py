"""King's Field (SLPS-00017) model reader and offline renderer: TMD, TIM, MIX archives, morphs."""
import struct
import numpy as np

def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def s16(b, o): return struct.unpack_from('<h', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]

# ---- chunk streams (MIXA/MIXB/COM): u32 size + payload -----------------------------------
def chunks(data):
    out, o = [], 0
    while o + 4 <= len(data):
        n = u32(data, o)
        if n > len(data) - o - 4: break
        out.append(data[o + 4:o + 4 + n]); o += 4 + n
    return out

# ---- asset (KfAsset): size, clip count, tmd offset, morph table, clip table --------------
class Asset:
    def __init__(self, b):
        self.size, self.clip_count, self.tmd_off, self.morph_tab, self.clip_tab = struct.unpack_from('<5I', b, 0)
        self.bytes = b[:self.size]
        self.tmd = Tmd(self.bytes[self.tmd_off:])
    def animation(self):
        b = self.bytes; clips = []
        for c in range(self.clip_count):
            co = u32(b, self.clip_tab + c * 4)
            count = u16(b, co); frames = []
            for k in range(count):
                fo = u32(b, co + 4 + k * 4)
                rev, dur, rest, mc = struct.unpack_from('<4H', b, fo)
                ids = list(struct.unpack_from('<%dH' % mc, b, fo + 8))
                frames.append(dict(reverse=rev, duration=dur, rest=rest, morphs=ids))
            clips.append(frames)
        return clips
    def morph(self, mid):
        b = self.bytes
        mo = u32(b, self.morph_tab + mid * 4)
        base, count = struct.unpack_from('<2I', b, mo + 4)
        d = np.frombuffer(b, dtype='<i2', count=count * 4, offset=mo + 12).reshape(count, 4)[:, :3]
        return base, d.astype(np.int32)
    def pose(self, clip, phase, obj=0):
        """Vertices at a phase of a clip, as the game's sampler computes them."""
        clips = self.animation()
        v = self.tmd.objects[obj]['verts'].astype(np.int32).copy()
        frames = clips[clip]; end = 0
        for i, f in enumerate(frames):
            start = end; end += f['duration']
            if phase < end:
                fwd = ((phase - start) << 12) // f['duration']
                blend = 4096 - fwd if f['reverse'] else fwd
                break
        else:
            f = frames[-1]; blend = 4096
        for m in f['morphs']:
            base, d = self.morph(m); v[base:base + len(d)] += d
        base, d = self.morph(f['rest']); v[base:base + len(d)] += (d * blend) >> 12
        return v

def archive(b):
    """A TMD archive: u16 count, u16 pad, then assets back to back."""
    count = u16(b, 0); o = 4; out = []
    for _ in range(count):
        a = Asset(b[o:]); out.append(a); o += a.size
    return out

# ---- TMD -----------------------------------------------------------------------------------
class Tmd:
    def __init__(self, b):
        self.id, self.flags, n = struct.unpack_from('<3I', b, 0)
        assert self.id == 0x41, hex(self.id)
        base = 12; self.objects = []
        for i in range(n):
            vo, vn, no, nn, po, pn, sc = struct.unpack_from('<6Ii', b, base + i * 28)
            verts = np.frombuffer(b, '<i2', vn * 4, base + vo).reshape(vn, 4)[:, :3].copy()
            norms = np.frombuffer(b, '<i2', nn * 4, base + no).reshape(nn, 4)[:, :3].copy() if nn else np.zeros((0, 3), np.int16)
            prims = []; o = base + po
            for _ in range(pn):
                olen, ilen, flag, mode = b[o], b[o + 1], b[o + 2], b[o + 3]
                body = b[o + 4:o + 4 + ilen * 4]; o += 4 + ilen * 4
                prims.append(decode_prim(mode, flag, body))
            self.objects.append(dict(verts=verts, norms=norms, prims=prims, scale=sc))

def decode_prim(mode, flag, body):
    m = mode & 0xfd
    p = dict(mode=mode, flag=flag, semi=bool(mode & 2))
    if (m & 0xe0) != 0x20:
        p['kind'] = 'other'; return p
    quad = bool(m & 8); tex = bool(m & 4); gour = bool(m & 0x10)
    n = 4 if quad else 3
    p.update(kind='poly', n=n, tex=tex, gouraud=gour)
    if tex:
        p['uv'] = [(body[i * 4], body[i * 4 + 1]) for i in range(n)]
        p['clut'] = u16(body, 2); p['tpage'] = u16(body, 6)
        io = n * 4
    else:
        p['color'] = tuple(body[0:3]); io = 4
    if flag & 1:  # unlit: colors instead of normals (not used by the gameplay enqueuers)
        p['kind'] = 'unlit'; return p
    if gour:
        p['normals'] = [u16(body, io + i * 4) for i in range(n)]
        p['verts'] = [u16(body, io + i * 4 + 2) for i in range(n)]
    else:
        p['normals'] = [u16(body, io)] * n
        p['verts'] = [u16(body, io + 2 + i * 2) for i in range(n)]
    return p

# ---- TIM into a 1024x512 VRAM image --------------------------------------------------------
class Vram:
    def __init__(self): self.w = np.zeros((512, 1024), np.uint16)
    def load_tims(self, b):
        o = 0
        while o + 8 <= len(b) and u32(b, o) == 0x10:
            fl = u32(b, o + 4); o += 8
            blocks = 2 if fl & 8 else 1
            for _ in range(blocks):
                n = u32(b, o); x, y, w, h = struct.unpack_from('<4H', b, o + 4)
                px = np.frombuffer(b, '<u2', w * h, o + 12).reshape(h, w)
                ys = (np.arange(h) + y) % 512; xs = (np.arange(w) + x) % 1024
                self.w[np.ix_(ys, xs)] = px; o += n
    def sample(self, tpage, clut, u, v):
        tx = (tpage & 0xf) * 64; ty = ((tpage >> 4) & 1) * 256; tp = (tpage >> 7) & 3
        cx = (clut & 0x3f) * 16; cy = (clut >> 6) & 0x1ff
        if tp == 0:
            word = self.w[ty + v, tx + (u >> 2)]; idx = (word >> ((u & 3) * 4)) & 0xf
            return self.w[cy, cx + idx]
        if tp == 1:
            word = self.w[ty + v, tx + (u >> 1)]; idx = (word >> ((u & 1) * 8)) & 0xff
            return self.w[cy, cx + idx]
        return self.w[ty + v, tx + u]

def rgb15(c):
    c = np.asarray(c, np.int32)
    return np.stack([(c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3], -1)

# ---- software renderer ---------------------------------------------------------------------
def rot_y(a):
    c, s = np.cos(a), np.sin(a); return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
def rot_x(a):
    c, s = np.cos(a), np.sin(a); return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])

def render(tmd, vram, verts=None, obj=0, yaw=0.0, pitch=0.2, size=384, extent=None, center=None,
           bg=(40, 40, 48), light=(0.4, -0.7, -0.6), point_sample=True, cull=True, painter=False):
    """Orthographic render. The game's +Y is down; the picture is drawn with -Y up. With painter,
    each face has one depth, its corners' average, as the game's ordering table sorts them, plus
    its primitive's 'depth_bias' (in these units, as the game's render_enqueue_tmd takes one)."""
    o = tmd.objects[obj]
    v = (o['verts'] if verts is None else verts).astype(np.float64)
    R = rot_x(pitch) @ rot_y(yaw)
    if center is None: center = (v.max(0) + v.min(0)) / 2
    pv = (v - center) @ R.T
    if extent is None: extent = np.abs(v - center).max() * 1.15 + 1
    sc = size / (2 * extent)
    sx = pv[:, 0] * sc + size / 2; sy = pv[:, 1] * sc + size / 2; sz = pv[:, 2]
    img = np.zeros((size, size, 3), np.float64); img[:] = bg
    zb = np.full((size, size), np.inf)
    L = np.array(light, float); L /= np.linalg.norm(L)
    norms = o['norms'].astype(np.float64) / 4096.0
    for p in o['prims']:
        if p['kind'] != 'poly': continue
        idx = p['verts']
        tris = [(0, 1, 2)] + ([(1, 3, 2)] if p['n'] == 4 else [])
        nr = [norms[i] if i < len(norms) else np.zeros(3) for i in p['normals']]
        sh = [0.35 + 0.75 * max(0.0, -np.dot(R @ n, L @ np.eye(3))) for n in nr]
        if cull:
            a, b, c = idx[0], idx[1], idx[2]
            if (sx[b] - sx[a]) * (sy[c] - sy[a]) - (sy[b] - sy[a]) * (sx[c] - sx[a]) <= 0: continue
        for t in tris:
            a, b, c = (idx[t[0]], idx[t[1]], idx[t[2]])
            X = np.array([sx[a], sx[b], sx[c]]); Y = np.array([sy[a], sy[b], sy[c]]); Z = np.array([sz[a], sz[b], sz[c]])
            if painter: Z[:] = np.mean(sz[idx]) + p.get('depth_bias', 0)
            area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0])
            if abs(area) < 1e-9: continue
            x0, x1 = int(max(0, np.floor(X.min()))), int(min(size - 1, np.ceil(X.max())))
            y0, y1 = int(max(0, np.floor(Y.min()))), int(min(size - 1, np.ceil(Y.max())))
            if x0 > x1 or y0 > y1: continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            w0 = ((X[1] - gx) * (Y[2] - gy) - (X[2] - gx) * (Y[1] - gy)) / area
            w1 = ((X[2] - gx) * (Y[0] - gy) - (X[0] - gx) * (Y[2] - gy)) / area
            w2 = 1 - w0 - w1
            inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
            if not inside.any(): continue
            z = w0 * Z[0] + w1 * Z[1] + w2 * Z[2]
            sub = zb[y0:y1 + 1, x0:x1 + 1]
            m = inside & (z < sub)
            if not m.any(): continue
            s = w0 * sh[t[0]] + w1 * sh[t[1]] + w2 * sh[t[2]]
            if p['tex']:
                U = np.array([p['uv'][k][0] for k in t], float); V = np.array([p['uv'][k][1] for k in t], float)
                uu = np.clip((w0 * U[0] + w1 * U[1] + w2 * U[2]), 0, 255).astype(int)
                vv = np.clip((w0 * V[0] + w1 * V[1] + w2 * V[2]), 0, 255).astype(int)
                texel = vram.sample(p['tpage'], p['clut'], uu, vv)
                m = m & (texel != 0)  # colour 0 is transparent
                col = rgb15(texel).astype(float) * s[..., None]
            else:
                col = np.array(p['color'], float)[None, None, :] * 2 * s[..., None] * 0.5
            sub[m] = z[m]
            img[y0:y1 + 1, x0:x1 + 1][m] = np.clip(col[m], 0, 255)
    return img.astype(np.uint8)

def save(img, path, scale=1):
    from PIL import Image
    im = Image.fromarray(img)
    if scale != 1: im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    im.save(path)

def grid(imgs, cols):
    h, w = imgs[0].shape[:2]; rows = (len(imgs) + cols - 1) // cols
    out = np.zeros((rows * h, cols * w, 3), np.uint8)
    for i, im in enumerate(imgs):
        r, c = divmod(i, cols); out[r * h:(r + 1) * h, c * w:(c + 1) * w] = im
    return out
