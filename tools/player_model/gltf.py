"""Write a posed model (kfmodel's one-object form, as preview.flatten makes) as a binary glTF, its
texture pages decoded to images, for looking at in Blender or any glTF viewer."""
import io, json, struct
import numpy as np
from PIL import Image
import kfmodel as kf

def page_image(vram, tpage, clut):
    u, v = np.meshgrid(np.arange(256), np.arange(256))
    texel = vram.sample(tpage, clut, u, v)
    rgba = np.zeros((256, 256, 4), np.uint8)
    rgba[..., :3] = kf.rgb15(texel)
    rgba[..., 3] = np.where(texel == 0, 0, 255)   # colour 0 is transparent
    out = io.BytesIO(); Image.fromarray(rgba, 'RGBA').save(out, 'PNG')
    return out.getvalue()

def to_gltf(p):
    """Actor space (right -x, up -y, forward -z) to glTF (up +y, forward +z, right -x)."""
    return np.stack([p[:, 0], -p[:, 1], -p[:, 2]], axis=1)

def linear(c):
    return (np.asarray(c, float) / 255.0) ** 2.2

def write(tmd, vram, path):
    obj = tmd.objects[0]
    V = obj['verts'].astype(float) / 1000.0           # metres, roughly: the player is 1.7 tall
    N = obj['norms'].astype(float)
    groups = {}                                        # (tpage, clut) or None -> corner lists
    for p in obj['prims']:
        if p.get('kind') != 'poly': continue
        key = (p['tpage'], p['clut']) if p['tex'] else None
        tris = [(0, 1, 2)] + ([(1, 3, 2)] if p['n'] == 4 else [])
        for t in tris:
            # The game keeps the face whose corners wind clockwise in glTF's terms; glTF wants
            # counter-clockwise, so each triangle is written reversed.
            for k in (t[0], t[2], t[1]):
                corner = (p['verts'][k], p['normals'][k],
                          p['uv'][k] if p['tex'] else None, None if p['tex'] else p['color'])
                groups.setdefault(key, []).append(corner)

    blob = bytearray(); views, accessors, meshes_prims, materials, images, textures = [], [], [], [], [], []
    def add(data, target=None):
        while len(blob) % 4: blob.append(0)
        views.append(dict(buffer=0, byteOffset=len(blob), byteLength=len(data), **({'target': target} if target else {})))
        blob.extend(data); return len(views) - 1
    def accessor(array, kind):
        array = np.ascontiguousarray(array, np.float32)
        view = add(array.tobytes(), 34962)
        a = dict(bufferView=view, componentType=5126, count=len(array), type=kind)
        if kind == 'VEC3': a.update(min=array.min(0).tolist(), max=array.max(0).tolist())
        accessors.append(a); return len(accessors) - 1

    for key, corners in groups.items():
        pos = to_gltf(np.array([V[c[0]] for c in corners]))
        nrm = to_gltf(np.array([N[c[1]] for c in corners])); nrm /= np.linalg.norm(nrm, axis=1)[:, None] + 1e-9
        attrs = dict(POSITION=accessor(pos, 'VEC3'), NORMAL=accessor(nrm, 'VEC3'))
        if key is None:
            attrs['COLOR_0'] = accessor(np.array([linear(c[3]) for c in corners]), 'VEC3')
            materials.append(dict(name='colour', pbrMetallicRoughness=dict(metallicFactor=0, roughnessFactor=1)))
        else:
            attrs['TEXCOORD_0'] = accessor(np.array([((c[2][0] + 0.5) / 256, (c[2][1] + 0.5) / 256) for c in corners]), 'VEC2')
            images.append(dict(bufferView=add(page_image(vram, *key)), mimeType='image/png'))
            textures.append(dict(source=len(images) - 1, sampler=0))
            materials.append(dict(name=f'page{key[0]}_clut{key[1]}', alphaMode='MASK',
                                  pbrMetallicRoughness=dict(baseColorTexture=dict(index=len(textures) - 1),
                                                            metallicFactor=0, roughnessFactor=1)))
        meshes_prims.append(dict(attributes=attrs, material=len(materials) - 1))

    doc = dict(asset=dict(version='2.0', generator='Verdite1 tools/player_model'),
               scene=0, scenes=[dict(nodes=[0])], nodes=[dict(name='knight', mesh=0)],
               meshes=[dict(name='knight', primitives=meshes_prims)],
               materials=materials, images=images, textures=textures,
               samplers=[dict(magFilter=9728, minFilter=9728)],   # nearest, as the PlayStation samples
               accessors=accessors, bufferViews=views, buffers=[dict(byteLength=len(blob))])
    js = json.dumps(doc).encode()
    js += b' ' * (-len(js) % 4); blob += b'\0' * (-len(blob) % 4)
    with open(path, 'wb') as f:
        f.write(struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob)))
        f.write(struct.pack('<II', len(js), 0x4E4F534A) + js)
        f.write(struct.pack('<II', len(blob), 0x004E4942) + bytes(blob))
