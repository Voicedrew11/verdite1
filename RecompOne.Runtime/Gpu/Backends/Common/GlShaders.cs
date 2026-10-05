using Silk.NET.OpenGL;

namespace RecompOne.Runtime.Hle;

internal static class GlShaders
{
    public const string FullscreenVs = """
        #version 330 core
        layout(location = 0) in vec2 aPos;
        out vec2 vUv;
        void main() {
            vUv = aPos * 0.5 + 0.5;
            gl_Position = vec4(aPos, 0.0, 1.0);
        }
        """;

    public const string PresentFs = """
        #version 330 core
        in vec2 vUv;
        uniform sampler2D uVram;
        uniform sampler2D uAo;
        uniform vec2 uOrigin;
        uniform vec2 uSize;
        uniform vec2 uTexSize;
        // 0 leaves the present bit-identical to what it has always been: the
        // multiply is skipped, not multiplied by one, so a build with the pass
        // switched off cannot round a colour by a least significant bit.
        uniform float uAoOn;
        // 0067. The reflection, premultiplied by its own weight, over the same
        // rectangle; skipped rather than blended with zero when it is off.
        uniform sampler2D uSsr;
        uniform float uSsrOn;
        // The target's depth. The pass runs coarser than the picture, and a plain
        // bilinear read spread the murk's edge over whatever stood in front of the
        // water, and that thing's empty texels over the water beside it; with the
        // surface buffer, drawn at the render scale while the pass runs, it says
        // what each of the pass's texels was computed from.
        uniform sampler2D uSsrDepth;
        // How much the occlusion reaches each material: the surface buffer's id and
        // row 2 of SurfaceMaterial's table, whose green is the share taken off.
        uniform sampler2D uSurface;
        uniform sampler2D uMatTable;
        uniform float uAoMatOn;
        out vec4 oColor;

        // What the reflection pass computed at uv from: the surface there (its
        // material and view depth), or material 0 where the pass writes nothing --
        // no surface, or one the depth buffer shows is behind an opaque one. A
        // crack between two water texels is water, as the pass takes it.
        vec2 ssrKey(vec2 uv) {
            vec2 tt = (uOrigin + uv * uSize) / uTexSize;
            vec4 s = texture(uSurface, tt);
            // Above 511 is a see-through 2D box's mark over the surface (SsrFs).
            int m = int(s.a + 0.5) & 511;
            // UI ink is an intentional cover, never a crack between water texels.
            if (m == 3) return vec2(0.0);
            if (m != 2) {
                vec2 tx = 1.0 / uTexSize;
                vec4 a0 = texture(uSurface, tt - vec2(tx.x, 0.0)), a1 = texture(uSurface, tt + vec2(tx.x, 0.0));
                vec4 b0 = texture(uSurface, tt - vec2(0.0, tx.y)), b1 = texture(uSurface, tt + vec2(0.0, tx.y));
                if ((int(a0.a + 0.5) & 511) == 2 && (int(a1.a + 0.5) & 511) == 2) { s.b = 0.5 * (a0.b + a1.b); m = 2; }
                else if ((int(b0.a + 0.5) & 511) == 2 && (int(b1.a + 0.5) & 511) == 2) { s.b = 0.5 * (b0.b + b1.b); m = 2; }
            }
            if (m <= 0 || m >= 256 || s.b * 65536.0 <= 1.0) return vec2(0.0);
            float d = texture(uSsrDepth, tt).r;
            if (d > 0.0 && d < 1.0 && d < s.b * 0.99 - 8.0 / 65536.0) return vec2(0.0);
            return vec2(float(m), s.b);
        }

        // The pass's four texels around this pixel, each weighed as bilinear only
        // when it was computed from this pixel's surface (the same material, the
        // depth within 10%); with none, the nearest such in depth.
        vec4 ssrAt(vec2 uv) {
            vec2 key = ssrKey(uv);
            if (key.x == 0.0) return vec4(0.0);
            vec2 sz = vec2(textureSize(uSsr, 0));
            vec2 f = uv * sz - 0.5;
            vec2 i0 = floor(f);
            vec2 fr = f - i0;
            vec4 sum = vec4(0.0);
            float wsum = 0.0, best = 1e9;
            ivec2 bestT = ivec2(clamp(floor(uv * sz), vec2(0.0), sz - 1.0));
            for (int k = 0; k < 4; k++) {
                vec2 o = vec2(k & 1, k >> 1);
                vec2 ik = clamp(i0 + o, vec2(0.0), sz - 1.0);
                vec2 kk = ssrKey((ik + 0.5) / sz);
                if (kk.x != key.x) continue;
                float diff = abs(kk.y - key.y) / max(min(kk.y, key.y), 1e-5);
                if (diff < best) { best = diff; bestT = ivec2(ik); }
                if (diff < 0.1) {
                    float bw = mix(1.0 - fr.x, fr.x, o.x) * mix(1.0 - fr.y, fr.y, o.y);
                    sum += texelFetch(uSsr, ivec2(ik), 0) * bw;
                    wsum += bw;
                }
            }
            return wsum > 1e-4 ? sum / wsum : texelFetch(uSsr, bestT, 0);
        }

        void main() {
            vec2 t = (uOrigin + vUv * uSize) / uTexSize;
            vec3 c = texture(uVram, t).rgb;
            // The occlusion texture is rendered at exactly this framebuffer's
            // size, so it is indexed by the present's own uv and needs no
            // geometry of its own.
            if (uAoOn > 0.5) {
                float ao = texture(uAo, vUv).r;
                if (uAoMatOn > 0.5) {
                    int m = int(texture(uSurface, t).a + 0.5) & 511;
                    if (m > 0 && m < 256) ao = mix(ao, 1.0, texelFetch(uMatTable, ivec2(m, 2), 0).g);
                }
                c *= ao;
            }
            if (uSsrOn > 0.5) {
                vec4 r = ssrAt(vUv);
                c = c * (1.0 - r.a) + r.rgb;
            }
            oColor = vec4(c, 1.0);
        }
        """;

    public const string Present24Fs = """
        #version 330 core
        in vec2 vUv;
        uniform sampler2D uVram;
        uniform vec2 uOrigin;
        uniform vec2 uSize;
        uniform int uScale;
        out vec4 oColor;

        int u5(float f) { return int(floor(f * 31.0 + 0.5)); }
        int texel16(int lin) {
            vec4 p = texelFetch(uVram, ivec2((lin & 1023) * uScale, ((lin >> 10) & 511) * uScale), 0);
            return u5(p.r) | (u5(p.g) << 5) | (u5(p.b) << 10) | (int(ceil(p.a)) << 15);
        }
        int byteAt(int b) {
            int t = texel16(b >> 1);
            return (b & 1) == 0 ? (t & 0xff) : ((t >> 8) & 0xff);
        }
        void main() {
            int px = int(floor(vUv.x * uSize.x));
            int py = int(floor(vUv.y * uSize.y));
            int ty = int(uOrigin.y) + py;
            int base = (ty * 1024 + int(uOrigin.x)) * 2 + px * 3;
            oColor = vec4(float(byteAt(base)) / 255.0, float(byteAt(base + 1)) / 255.0,
                          float(byteAt(base + 2)) / 255.0, 1.0);
        }
        """;

    /// <summary>
    /// Ambient occlusion, first pass: the finished frame's depth attachment in,
    /// one occlusion factor per output pixel out.
    ///
    /// The depth texel holds the GTE's own view depth over 65536, so undoing the
    /// game's projection gets a view position straight back out of it — the divide
    /// was <c>screen = centre + IR * H / z</c>, so <c>view.xy = (screen - centre) *
    /// z / H</c> with H and the centre published from <c>Gte.Rtp</c> rather than
    /// assumed. That is what makes this the game's geometry rather than a
    /// plausible-looking depth trick: the reconstructed normal is the surface's
    /// own, to whatever precision the recovered SZ has.
    ///
    /// A texel at the far plane is one nothing wrote: 2D, or a triangle the vertex
    /// map missed. It is neither shaded nor allowed to occlude, so the HUD, the
    /// menus and the death fade come through untouched and a hole in the depth
    /// buffer costs occlusion rather than inventing it.
    /// </summary>
    public const string AoFs = """
        #version 330 core
        in vec2 vUv;
        out vec4 oColor;

        uniform sampler2D uDepth;
        // 0058. The frame's geometry, redrawn as normals. uNormalOn is 0 when the
        // pass is off or nothing was collected, and a texel's alpha says whether
        // this particular pixel was reached.
        uniform sampler2D uNormal;
        uniform float uNormalOn;
        // Set only on the frame the census reads back: compute the old
        // depth-difference normal as well and report how far apart the two are, so
        // "the geometry's normal is better" is a reading rather than a claim.
        uniform float uNormalCompare;
        // The display area inside the render target, and the target's size, both
        // in the game's own 1x pixels -- the same three numbers the present shader
        // is given, so the two passes address the same rectangle.
        uniform vec2  uOrigin;
        uniform vec2  uSize;
        uniform vec2  uTexSize;
        // One depth texel as a step in this pass's own uv, which is not 1/uSize:
        // the target is rendered at the render scale and the fine structure of the
        // depth buffer is at that scale, not the game's.
        uniform vec2  uTexel;
        // The GTE's projection: distance, and the screen centre the divide is
        // offset by, expressed in this pass's uv so no pixel arithmetic has to be
        // repeated here.
        uniform float uProjH;
        uniform vec2  uCentre;
        uniform float uRadius;
        uniform float uStrength;
        uniform float uBias;
        uniform float uMaxDepth;
        uniform int   uSamples;
        // 0083. Past this view depth the surface is left as the game drew it.
        uniform float uPlainZ;

        // 0059. The area's floor plan, and the transform that reaches it. uViewR is
        // the game's own world-to-view rotation uploaded untransposed, which GLSL
        // reads column-major and so hands back already inverted: uViewR * v takes a
        // view vector to a world one. The game's Y is negative upwards, so
        // everything below works in "up" = -y.
        uniform float     uWorldOn;
        uniform sampler2D uHeight;
        uniform mat3      uViewR;
        uniform vec3      uCam;
        uniform float     uWorldStrength;
        uniform float     uWorldRadius;
        uniform float     uTileUnits;
        uniform float     uSpan;
        uniform float     uWallHeight;

        const float FAR = 65536.0;
        const float GOLDEN = 2.39996323;

        float depthAt(vec2 uv) {
            return texture(uDepth, (uOrigin + clamp(uv, 0.0, 1.0) * uSize) / uTexSize).r;
        }

        vec4 normalAt(vec2 uv) {
            return texture(uNormal, (uOrigin + clamp(uv, 0.0, 1.0) * uSize) / uTexSize);
        }

        // The view position of the point this pass's uv names, given its depth.
        // Screen X and Y are in the game's own pixels measured from the projection
        // centre, which is what uCentre and uSize turn a uv into.
        vec3 viewAt(vec2 uv, float d) {
            float z = d * FAR;
            return vec3((uv - uCentre) * uSize * (z / uProjH), z);
        }

        // The neighbour on each axis that is nearer in depth, so a pixel on a
        // silhouette takes its normal from the surface it belongs to instead of
        // straddling the edge and coming out facing the camera.
        vec3 nearer(vec3 p, vec3 a, vec3 b) {
            return abs(a.z - p.z) < abs(b.z - p.z) ? a - p : p - b;
        }

        // A tile: its two floors as heights above the world's zero, and whether it
        // stops sight. Zero in a floor channel is no tile there.
        // x and y are the two halves' floor heights, z the flag byte: bit 0 the
        // lower half is drawn, bit 1 the upper is, bit 2 the tile stops sight. A
        // height of zero is a real floor, which is why being drawn is its own bit.
        vec3 tileAt(vec2 wxz) {
            vec2 tile = floor(wxz / uTileUnits);
            if (tile.x < 0.0 || tile.y < 0.0 || tile.x >= uSpan || tile.y >= uSpan) return vec3(0.0, 0.0, 0.0);
            vec3 t = texture(uHeight, (tile + 0.5) / uSpan).rgb;
            return vec3(t.r * (255.0 * 128.0), t.g * (255.0 * 128.0), floor(t.b * 255.0 + 0.5));
        }

        // How much of this surface's sky the room takes, marched over the floor plan
        // in world space -- so a wall behind the camera occludes exactly as one in
        // front of it does, which is the whole point and is the one thing a
        // screen-space pass cannot be made to do.
        // `rot` is the pixel's own 4x4 interleaved angle, the same one the
        // screen-space spiral uses. Without it every pixel marches the *same* eight
        // directions at the *same* three distances, so the moment the camera crosses
        // a tile boundary every pixel changes its answer together and the whole
        // picture steps -- seen as the screen briefly darkening as you walk. With
        // it the crossings are spread over the 4x4 cell and the blur that follows
        // averages them, so the same change arrives as a gradient.
        float worldOcclusion(vec3 pv, vec3 nv, float rot) {
            vec3 w = uCam + uViewR * pv;
            vec3 nw = uViewR * nv;
            vec3 nUp = vec3(nw.x, -nw.y, nw.z);
            float pu = -w.y;

            // A half-step offset from the same pattern, so the three ring radii are
            // not the same three for every pixel either.
            float jitter = fract(rot * (8.0 / 6.28318531)) - 0.5;

            float occ = 0.0;
            for (int k = 0; k < 8; k++) {
                float a = rot + (float(k) + 0.5) * (6.28318531 / 8.0);
                vec2 dir = vec2(cos(a), sin(a));

                // The highest thing this direction puts against the sky, as a
                // slope. A step in the floor is one; a tile that stops sight is a
                // wall standing on its own floor, which is what a corridor is made
                // of and what the heights alone never show.
                float best = 0.0;
                for (int j = 1; j <= 3; j++) {
                    float t = uWorldRadius * (float(j) + jitter) / 3.0;
                    vec3 f = tileAt(w.xz + dir * t);
                    int flags = int(f.z);
                    // A drawn floor is a step: where it stands above this surface it
                    // is what a wall is made of.
                    if ((flags & 1) != 0) best = max(best, (f.x - pu) / t);
                    if ((flags & 2) != 0) best = max(best, (f.y - pu) / t);
                    // A tile with no floor at all is solid rock -- there is nowhere
                    // to stand there, which is what the edge of a room is in this
                    // grid. It stands its own height above whatever is being shaded.
                    if ((flags & 3) == 0 || (flags & 4) != 0)
                        best = max(best, uWallHeight / t);
                }
                if (best <= 0.0) continue;

                // Weighted by how much of this surface actually faces the horizon
                // it found, and averaged over every direction rather than over the
                // occluding ones -- a floor faces no horizontal direction at all,
                // so weighting on the flat direction would leave every floor in the
                // game unshaded.
                vec3 hdir = normalize(vec3(dir.x, best, dir.y));
                occ += max(0.0, dot(nUp, hdir)) * (best / sqrt(1.0 + best * best));
            }
            return occ / 8.0;
        }

        // The old answer: a normal differenced out of four neighbouring depth
        // texels. It straddles a silhouette wherever two surfaces meet and carries
        // the depth's quantisation everywhere else, which is why the geometry's own
        // plane replaced it -- and why it is worth measuring how far apart they are.
        vec3 depthNormal(vec3 p) {
            vec2 sx = vec2(uTexel.x, 0.0), sy = vec2(0.0, uTexel.y);
            float dl = depthAt(vUv - sx), dr = depthAt(vUv + sx);
            float du = depthAt(vUv - sy), dd = depthAt(vUv + sy);
            // A neighbour with no depth is not a position; fall back to this
            // pixel's own so the difference is taken against the other side.
            vec3 l = dl > 0.0 && dl < 1.0 ? viewAt(vUv - sx, dl) : p;
            vec3 r = dr > 0.0 && dr < 1.0 ? viewAt(vUv + sx, dr) : p;
            vec3 u = du > 0.0 && du < 1.0 ? viewAt(vUv - sy, du) : p;
            vec3 b = dd > 0.0 && dd < 1.0 ? viewAt(vUv + sy, dd) : p;

            vec3 n = cross(nearer(p, r, l), nearer(p, b, u));
            if (dot(n, n) < 1e-12) return vec3(0.0);
            n = normalize(n);
            // The camera is at the origin looking down +Z, so a surface facing it
            // has a negative dot with its own position.
            return dot(n, p) > 0.0 ? -n : n;
        }

        void main() {
            // Red is the occlusion factor the present multiplies by. Green says
            // whether there was a surface here at all -- it is not used to draw
            // anything, it is what lets the census tell "the mask refused this
            // pixel" from "the pass looked and found nothing to shade it with",
            // which are the two ways a blank patch happens and are not the same
            // bug. See KF2_AO_PROBE=2.
            float d = depthAt(vUv);
            // Nothing wrote here (2D, or a triangle with no recovered depth), or
            // the fog has the picture: unoccluded, and the present multiplies by 1.
            if (d >= 1.0 || d <= 0.0) { oColor = vec4(1.0, 0.0, 0.0, 0.0); return; }

            vec3 p = viewAt(vUv, d);
            if (p.z > uMaxDepth) { oColor = vec4(1.0, 0.0, 0.0, 0.0); return; }
            float plain = uPlainZ > 0.0 ? smoothstep(uPlainZ - 2048.0, uPlainZ, p.z) : 0.0;
            if (plain >= 1.0) { oColor = vec4(1.0, 1.0, 0.0, 0.0); return; }

            // 0058. The polygon's own plane, where the frame's geometry reached
            // this pixel. It is exact and constant across a face, where the
            // reconstruction below straddles every silhouette and carries the
            // depth buffer's quantisation into every flat wall.
            vec3 n;
            float geo = 0.0;
            if (uNormalOn > 0.5) {
                vec4 nb = normalAt(vUv);
                vec3 nv = nb.xyz * 2.0 - 1.0;
                // 0067. An edge-on polygon writes a zero vector rather than
                // clearing the texel, since the buffer is blended now.
                if (nb.a > 0.5 && dot(nv, nv) > 0.25) {
                    n = normalize(nv);
                    geo = 1.0;
                }
            }

            // How far the two answers are apart, in right angles, on the census
            // frame only. Zero where there is nothing to compare.
            float disagree = 0.0;

            if (geo < 0.5 || uNormalCompare > 0.5) {
                vec3 nd = depthNormal(p);
                if (geo < 0.5) {
                    if (dot(nd, nd) < 1e-12) { oColor = vec4(1.0, 1.0, 0.0, 0.0); return; }
                    n = nd;
                } else if (dot(nd, nd) > 1e-12) {
                    disagree = acos(clamp(dot(n, nd), -1.0, 1.0)) / 1.57079633;
                }
            }

            // The world radius as it projects at this depth, in uv. Clamped at the
            // near end because a sphere a hand's width across fills the screen when
            // the camera is inside it, and at the far end to a texel so the kernel
            // never collapses onto the pixel it is shading.
            float rPix = clamp(uRadius * uProjH / p.z, 1.5, 96.0);
            vec2 rUv = rPix / uSize;

            // A 4x4 interleaved rotation rather than a hash: the blur below is a
            // 4x4 box, so a pattern with that period cancels exactly and leaves no
            // residual grain, where noise leaves noise.
            ivec2 px = ivec2(gl_FragCoord.xy) & 3;
            float a0 = float((px.y << 2) | px.x) * (6.28318531 / 16.0);

            float occ = 0.0;
            for (int i = 0; i < uSamples; i++) {
                float t = (float(i) + 0.5) / float(uSamples);
                float ang = a0 + float(i) * GOLDEN;
                vec2 off = vec2(cos(ang), sin(ang)) * sqrt(t) * rUv;

                float sd = depthAt(vUv + off);
                // The far plane is the absence of a surface, not a surface a long
                // way off: it must not occlude, or every silhouette against the
                // HUD would draw a dark halo.
                if (sd >= 1.0 || sd <= 0.0) continue;

                vec3 v = viewAt(vUv + off, sd) - p;
                float len = length(v);
                if (len < 1e-4) continue;
                // Falls off past the radius instead of stopping at it, so a wall
                // sliding out of range dims rather than switching off.
                float range = uRadius / max(uRadius, len);
                occ += max(0.0, dot(v / len, n) - uBias) * range;
            }

            float ao = 1.0 - uStrength * (occ / float(uSamples));
            // 0059. The room the surface is in, on top of the picture it is in.
            if (uWorldOn > 0.5)
                ao *= 1.0 - uWorldStrength * clamp(worldOcclusion(p, n, a0), 0.0, 1.0);
            ao = mix(ao, 1.0, plain);
            oColor = vec4(clamp(ao, 0.0, 1.0), 1.0, geo, clamp(disagree, 0.0, 1.0));
        }
        """;

    /// <summary>
    /// Ambient occlusion, second pass: the 4x4 box that cancels the first pass's
    /// 4x4 rotation exactly, weighted by depth so it does not carry a wall's
    /// occlusion across a silhouette onto whatever is behind it.
    /// </summary>
    public const string AoBlurFs = """
        #version 330 core
        in vec2 vUv;
        out vec4 oColor;

        uniform sampler2D uAo;
        uniform sampler2D uDepth;
        uniform vec2  uOrigin;
        uniform vec2  uSize;
        uniform vec2  uTexSize;
        uniform vec2  uTexel;
        // How far apart two depths may be, as a fraction of the nearer one, and
        // still be treated as the same surface. Relative rather than absolute
        // because the recovered depth is a view depth: a step that is a crease at
        // arm's length is a rounding error across a room.
        uniform float uEdge;

        float depthAt(vec2 uv) {
            return texture(uDepth, (uOrigin + clamp(uv, 0.0, 1.0) * uSize) / uTexSize).r;
        }

        void main() {
            float d = depthAt(vUv);
            if (d >= 1.0 || d <= 0.0) { oColor = vec4(1.0, 0.0, 0.0, 0.0); return; }

            float sum = 0.0, wsum = 0.0;
            for (int y = -2; y <= 1; y++) {
                for (int x = -2; x <= 1; x++) {
                    vec2 uv = vUv + vec2(float(x), float(y)) * uTexel;
                    float sd = depthAt(uv);
                    if (sd >= 1.0 || sd <= 0.0) continue;
                    if (abs(sd - d) > uEdge * d) continue;
                    sum += texture(uAo, uv).r;
                    wsum += 1.0;
                }
            }
            // Blue is carried through from the centre tap rather than blurred: it
            // is the census's "this pixel's normal came from the geometry", which
            // is a fact about one pixel and not a quantity to average.
            oColor = vec4(wsum > 0.0 ? sum / wsum : texture(uAo, vUv).r, 1.0,
                          texture(uAo, vUv).b, texture(uAo, vUv).a);
        }
        """;

    /// <summary>
    /// 0058. The frame's own geometry, redrawn into a normal buffer after the frame
    /// is finished. The colour pass cannot write this itself: its fragment shader
    /// has a dual-source output (index 1) for the console's blend modes, and a
    /// program with one of those may not render to more than one draw buffer, so
    /// there is no MRT to hang a G-buffer off. Drawing the triangles again is what
    /// the port can do now that it assembles and enumerates them
    /// (<see cref="AoGeometry"/>).
    ///
    /// Position is <c>PrimVs</c>'s arithmetic to the letter, so a triangle lands on
    /// the same pixels. W is the view depth rather than the colour pass's 1 for an
    /// untextured polygon: nothing here has to match that pass's interpolation, and
    /// a perspective-correct depth is what makes the reconstructed surface the
    /// polygon's actual plane instead of a curve through its corners.
    /// </summary>
    public const string NormalVs = """
        #version 330 core
        layout(location = 0) in vec2  inPos;
        layout(location = 1) in float inZ;
        layout(location = 2) in float inM;
        layout(location = 3) in vec2  inUv;
        layout(location = 4) in uint  inTex;

        uniform vec2 uPosBias;
        uniform vec2 uFbInv;

        out float vDepth;
        flat out float vM;
        out vec2 vUv;
        flat out uint vTex;
        // 0089. The world program's; no packet is faded.
        out vec2 vFadeXZ;

        void main() {
            vec2 p = (inPos + uPosBias) * uFbInv - 1.0;
            float w = max(inZ, 1.0);
            gl_Position = vec4(p * w, 0.0, w);
            vDepth = inZ * (1.0/65536.0);
            vM = inM;
            vUv = inUv;
            vTex = inTex;
            vFadeXZ = vec2(0.0);
        }
        """;

    /// <summary>
    /// 0058. One polygon's plane, as a normal. The view position is reconstructed
    /// exactly as the occlusion pass reconstructs it — the same H and the same
    /// centre, read off the GTE — and the normal is the cross product of its two
    /// screen derivatives, which across a plane is constant and exact. That is the
    /// whole difference from the pass's own reconstruction: this one is taken
    /// *inside* one primitive, so it can never straddle a silhouette or average two
    /// surfaces, and it does not have the depth buffer's quantisation in it.
    ///
    /// Alpha is the "there is a normal here" bit the pass tests, so a pixel this
    /// never reached falls back to the old cross product rather than to a wrong
    /// normal.
    /// </summary>
    public static readonly string NormalFs = """
        #version 330 core
        in float vDepth;
        flat in float vM;
        in vec2 vUv;
        flat in uint vTex;
        // 0089. A world corner's X and Z, for DistanceFade (WorldNormalVs).
        in vec2 vFadeXZ;
        // 0067. Two outputs. The first is the occlusion pass's normal buffer and
        // is blended (ONE, ONE_MINUS_SRC_ALPHA), so a translucent surface writes
        // alpha 0 and leaves the opaque surface under it -- whose depth is the one
        // the occlusion pass reads. The second is the surface buffer, which is
        // not blended: the last surface drawn at a pixel, water included, with its
        // normal, its depth and its material (SurfaceMaterial).
        layout(location = 0) out vec4 oColor;
        layout(location = 1) out vec4 oSurface;

        uniform float uProjH;
        // The projection centre and the render scale, in the target's own pixels:
        // gl_FragCoord / uScale is where this fragment is in the game's.
        uniform vec2  uCentre;
        uniform float uScale;
        // A veil's texel comes from sample VRAM; 1 draws its see-through texels,
        // 2 a textured one's opaque texels, 0 is no veil.
        uniform sampler2D uVram;
        uniform int uVeilPass;
        // 0085. The frame's own depth, when the map was drawn on the GPU: the map is
        // drawn here first and the table's triangles after it, so order no longer
        // says which surface is in front, and a fragment behind the depth is dropped.
        uniform sampler2D uFrameDepth;
        uniform int uDepthCull;
        uniform vec2 uDepthStep;
        // The map's water, one slice of view depth, (x, y]; y 0 is none.
        uniform vec2 uZSlice;

        //@distanceFade

        vec4 vfetch(ivec2 c) { return texelFetch(uVram, c & ivec2(1023, 511), 0); }
        int vu5(float f) { return int(floor(f * 31.0 + 0.5)); }
        int vfetch16(ivec2 c) {
            vec4 p = vfetch(c);
            return vu5(p.r) | (vu5(p.g) << 5) | (vu5(p.b) << 10) | (int(ceil(p.a)) << 15);
        }
        // PrimFs's decode, without the texture window: 2D needs none.
        vec4 veilTexel(ivec2 uv) {
            int tp = int(vTex & 0xffffu), cl = int((vTex >> 16) & 0x7fffu);
            int mode = (tp >> 7) & 3;
            ivec2 page = ivec2((tp & 0xf) * 64, ((tp >> 4) & 1) * 256);
            ivec2 clut = ivec2((cl & 0x3f) * 16, (cl >> 6) & 0x1ff);
            uv &= ivec2(0xff);
            if (mode == 0) {
                int s = vfetch16(page + ivec2(uv.x >> 2, uv.y));
                return vfetch(ivec2(clut.x + ((s >> ((uv.x & 3) << 2)) & 0xf), clut.y));
            }
            if (mode == 1) {
                int s = vfetch16(page + ivec2(uv.x >> 1, uv.y));
                return vfetch(ivec2(clut.x + ((s >> ((uv.x & 1) << 3)) & 0xff), clut.y));
            }
            return vfetch(page + uv);
        }

        // Octahedral: a unit normal in two numbers, exact enough in half floats.
        vec2 octEncode(vec3 n) {
            n /= abs(n.x) + abs(n.y) + abs(n.z);
            vec2 e = n.xy;
            if (n.z < 0.0)
                e = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
            return e;
        }

        void main() {
            float z = vDepth * 65536.0;
            // Taken here, in uniform control flow, for a surface's texel below.
            vec2 dUvx = dFdx(vUv), dUvy = dFdy(vUv);
            // 0089. A pixel the colour pass dithers away by distance is no surface.
            if (fadeDropped(distanceFade(vFadeXZ, z))) discard;
            // A veil, a see-through 2D box: where it is see-through, both buffers
            // are left as they are and its mark is added to the id (the draw blends
            // it so); a texel without the semi-transparency bit is an overlay.
            if (vM > 511.5) {
                bool see = true;
                if ((vTex & 0x80000000u) != 0u) {
                    vec4 t = veilTexel(ivec2(floor(vUv)));
                    if (t.rgb == vec3(0.0) && t.a < 0.5) discard;
                    see = t.a >= 0.5;
                    // Add/subtract text uses STP ink too. It covers water effects;
                    // black STP texels remain holes in both modes.
                    uint blend = (vTex >> 5u) & 3u;
                    if (see && (blend == 1u || blend == 2u)) {
                        if (t.rgb == vec3(0.0)) discard;
                        see = false;
                    }
                }
                if (see != (uVeilPass == 1)) discard;
                oColor = vec4(0.0);
                oSurface = vec4(0.0, 0.0, 0.0, see ? vM : 3.0);
                return;
            }
            // A blended triangle arrives with 256 added to its material, so
            // opacity is the draw's and not a guess from the id.
            bool opaque = vM < 255.5;
            float id = opaque ? vM : vM - 256.0;
            // 0067. The HUD and anything else 2D: no normal and no depth, only the
            // fact that it covers what is under it.
            if (id > 2.5 && id < 3.5) { oColor = vec4(0.0); oSurface = vec4(0.0, 0.0, 0.0, id); return; }
            // A textured surface's transparent texel drew nothing and wrote no
            // depth, so it is no surface here either: a billboard's box would
            // otherwise lay its own normal over the wall the depth buffer holds.
            // The texel is PrimFs's centre tap, truncated the way it rounds.
            if ((vTex & 0x80000000u) != 0u) {
                ivec2 tc = ivec2(dUvx.x < 0.0 ? int(ceil(vUv.x - 0.0001)) : int(floor(vUv.x + 0.0001)),
                                 dUvy.y < 0.0 ? int(ceil(vUv.y - 0.0001)) : int(floor(vUv.y + 0.0001)));
                vec4 t = veilTexel(tc);
                if (t.rgb == vec3(0.0) && t.a < 0.5) discard;
            }
            if (z <= 0.0) { oColor = vec4(0.0); oSurface = vec4(0.0); return; }
            if (uZSlice.y > 0.0 && (z <= uZSlice.x || z > uZSlice.y)) discard;
            vec2 s = gl_FragCoord.xy / uScale;
            vec3 p = vec3((s - uCentre) * (z / uProjH), z);
            vec3 dpx = dFdx(p), dpy = dFdy(p);
            if (uDepthCull != 0) {
                float d = texelFetch(uFrameDepth, ivec2(gl_FragCoord.xy * uDepthStep), 0).r;
                float dz = d * 65536.0;
                // The depth texel may sit up to a pixel off this one's centre.
                if (d < 0.99999 && z > dz + 16.0 + dz / 128.0 + abs(dpx.z) + abs(dpy.z)) discard;
            }
            vec3 n = cross(dpx, dpy);
            // A polygon edge-on to the camera, or one degenerate after projection:
            // no plane to report. The zero vector leaves the occlusion pass its own
            // answer, and material None leaves this pixel out of the reflections.
            if (dot(n, n) < 1e-12) {
                oColor = opaque ? vec4(0.5, 0.5, 0.5, 1.0) : vec4(0.0);
                oSurface = vec4(0.0);
                return;
            }
            n = normalize(n);
            // The camera is at the origin looking down +Z.
            if (dot(n, p) > 0.0) n = -n;
            oColor = opaque ? vec4(n * 0.5 + 0.5, 1.0) : vec4(0.0);
            oSurface = vec4(octEncode(n), vDepth, id);
        }
        """.Replace("//@distanceFade", DistanceFade.Glsl);

    /// <summary>
    /// 0085. The retained map into the normal and surface buffers: <c>WorldVs</c>'s
    /// position to the letter, for <c>NormalFs</c>. The material is the face's own,
    /// or <c>Opaque</c>, as <c>SurfaceMaterial.Classify</c> gives an opaque packet.
    /// </summary>

    /// <summary>
    /// 0085. A model drawn from a cached mesh (Step 3's second slice), for <c>WorldVs</c>
    /// and <c>WorldNormalVs</c>: a corner's <c>inWorld.x</c> is its vertex in the
    /// instance's posed vertices, <c>inCue</c> its face's first three vertices and
    /// <c>inRgbc</c> the fourth (all ones for a triangle). A face is dropped where the
    /// lit assembler drops it: facing away on the screen, or its mean table depth at or
    /// before 0, before <c>uModelNear</c> or at or past <c>uModelFar</c>. Every corner of a face computes the
    /// same answer from the same four vertices.
    /// </summary>
    const string ModelGlsl = """
        uniform int   uModel;
        uniform isamplerBuffer uModelVerts;
        uniform int   uModelBase;
        uniform mat3  uModelR;
        uniform vec3  uModelT;
        uniform float uModelFar;
        uniform float uModelNear;
        uniform mat3  uModelLlm;
        uniform vec3  uModelCue;
        uniform uint  uModelRgbc;
        uniform uint  uModelMat;
        // The GTE's own screen centre (OFX, OFY), for its saturated projection.
        uniform vec2  uModelGteC;
        // The pose store: -1 takes the frame's vertices at uModelBase; otherwise the
        // first texel of a rigid model's vertices (weight -1), or of an MO keyframe
        // and its deltas, two texels a vertex, blended as the game's decoder blends
        // them: key + (short)((delta * weight) >> 12), in 16 bits.
        uniform isamplerBuffer uModelPoses;
        uniform int   uModelPose;
        uniform int   uModelPoseW;
        // The sky (func_8002F918): a face is kept by its facing alone, on whole
        // pixels, whatever its depth; lit per corner, with no cue.
        uniform int   uModelSky;
        // An object near the camera (func_80030540): no depth range; a face whose
        // corners its transform projects keeps the facing on the screen, a quad on its
        // whole loop, and any other goes to the game's clipper, which here is the GPU's
        // near clip and the face's plane against the eye.
        uniform int   uModelTile;

        ivec3 modelPosed(int i) {
            if (uModelPose < 0) return texelFetch(uModelVerts, uModelBase + i).xyz;
            if (uModelPoseW < 0) return texelFetch(uModelPoses, uModelPose + i).xyz;
            ivec3 k = texelFetch(uModelPoses, uModelPose + 2 * i).xyz;
            ivec3 d = texelFetch(uModelPoses, uModelPose + 2 * i + 1).xyz;
            ivec3 s = (((d * uModelPoseW) >> 12) << 16) >> 16;
            return ((k + s) << 16) >> 16;
        }

        vec3 modelVertex(uint i) {
            return uModelR * vec3(modelPosed(int(i))) + uModelT;
        }

        // A model placed in view space (the first-person arm), with the GTE's own
        // rotation (4.12) and translation: taken to the eye as RTPS takes it, in
        // integers, (R v >> 12) + T, IR saturated. Near the eye the divide magnifies a
        // unit into pixels, and a float transform there flips an edge-on face.
        uniform int   uModelView;
        uniform ivec3 uModelVR0, uModelVR1, uModelVR2, uModelVT;
        vec3 modelEye(uint i) {
            if (uModelView == 0) return uR * (modelVertex(i) - uCam) + uT;
            ivec3 p = modelPosed(int(i));
            ivec3 m = ivec3(uModelVR0.x * p.x + uModelVR0.y * p.y + uModelVR0.z * p.z,
                            uModelVR1.x * p.x + uModelVR1.y * p.y + uModelVR1.z * p.z,
                            uModelVR2.x * p.x + uModelVR2.y * p.y + uModelVR2.z * p.z);
            ivec3 v = (m >> 12) + uModelVT;
            return vec3(clamp(v.xy, ivec2(-32768), ivec2(32767)), v.z);
        }

        // RTPS as the GTE takes it, which the facing test is taken on: the divide
        // saturates below H/2 and the screen position at the ends of its range, so a
        // face reaching behind the eye is kept or dropped as the game keeps it.
        vec2 modelScreen(vec3 v) {
            float z = max(clamp(v.z, 0.0, 65535.0), uH * 0.5);
            vec2 s = clamp(uModelGteC + uH * clamp(v.xy, -32768.0, 32767.0) / z, -1024.0, 1023.0);
            return uWorldSnap != 0 ? floor(s) : s;
        }

        // Where the packets put a corner the GTE's divide saturates for (nearer than
        // H/2) or whose screen position it clamps: modelScreen's place, taken to the
        // target's pixels, W the true depth as the packets' is (at least 1). The lit
        // assembler clips nothing, so neither does the near plane here. Anywhere else
        // the projection is the ordinary one, unchanged.
        vec4 modelPlace(vec4 p, vec3 v) {
            if (uModel == 0 || uModelTile != 0) return p;
            if (v.z >= uH * 0.5) {
                vec2 raw = uModelGteC + uH * v.xy / v.z;
                if (all(greaterThanEqual(raw, vec2(-1024.0))) && all(lessThanEqual(raw, vec2(1023.0)))) return p;
            }
            float w = max(v.z, 1.0);
            return vec4(((modelScreen(v) - uModelGteC + uC) * 2.0 / uFb - 1.0) * w, 0.0, w);
        }

        // Whether the GTE projects a corner without saturating: in front of H/2 and on
        // the screen's range, as the near transform's flag test keeps it.
        bool modelProjects(vec3 v) {
            if (v.z <= uH * 0.5 || v.z > 32767.0) return false;
            vec2 raw = uModelGteC + uH * v.xy / v.z;
            return all(greaterThanEqual(raw, vec2(-1024.0))) && all(lessThanEqual(raw, vec2(1023.0)));
        }

        bool modelTileKept(vec3 v0, vec3 v1, vec3 v2, uint f3) {
            bool quad = f3 != 0xFFFFFFFFu;
            vec3 v3 = quad ? modelEye(f3) : v2;
            if (modelProjects(v0) && modelProjects(v1) && modelProjects(v2) && (!quad || modelProjects(v3))) {
                vec2 s0 = modelScreen(v0), s1 = modelScreen(v1), s2 = modelScreen(v2), s3 = modelScreen(v3);
                // A quad's loop 0, 1, 3, 2: its area is half the diagonals' cross.
                vec2 a = quad ? s3 - s0 : s1 - s0, b = quad ? s2 - s1 : s2 - s0;
                return a.x * b.y - a.y * b.x > 0.0;
            }
            // The plane against the eye: the facing of what the near clip leaves.
            vec3 n = quad ? cross(v3 - v0, v2 - v1) : cross(v1 - v0, v2 - v0);
            return dot(v0, n) > 0.0;
        }

        bool modelFaceKept(vec3 f, uint f3) {
            vec3 v0 = modelEye(uint(f.x));
            vec3 v1 = modelEye(uint(f.y));
            vec3 v2 = modelEye(uint(f.z));
            if (uModelTile != 0) return modelTileKept(v0, v1, v2, f3);
            // The vertex cache holds each corner's SZ over four; the face sits at their mean.
            int z0 = int(clamp(v0.z, 0.0, 65535.0)) >> 2;
            int z1 = int(clamp(v1.z, 0.0, 65535.0)) >> 2;
            int z2 = int(clamp(v2.z, 0.0, 65535.0)) >> 2;
            int z;
            if (f3 != 0xFFFFFFFFu) {
                vec3 v3 = modelEye(f3);
                z = (z0 + z1 + z2 + (int(clamp(v3.z, 0.0, 65535.0)) >> 2)) >> 2;
            } else z = (z0 + z1 + z2) / 3;
            if (uModelSky == 0 && (z <= 0 || float(z) < uModelNear || float(z) >= uModelFar)) return false;
            vec2 s0 = modelScreen(v0), s1 = modelScreen(v1), s2 = modelScreen(v2);
            if (uModelSky != 0) { s0 = floor(s0); s1 = floor(s1); s2 = floor(s2); }
            return (s1.x - s0.x) * (s2.y - s0.y) - (s1.y - s0.y) * (s2.x - s0.x) > 0.0;
        }
        """;

    public static readonly string WorldNormalVs = """
        #version 330 core
        layout(location = 0) in vec3  inWorld;
        layout(location = 5) in vec3  inCue;
        layout(location = 7) in uint  inFlags;
        layout(location = 8) in uint  inRgbc;

        invariant gl_Position;

        out float vDepth;
        flat out float vM;
        out vec2 vUv;
        flat out uint vTex;
        // 0089. As WorldVs's.
        out vec2 vFadeXZ;
        uniform vec4 uFade;

        uniform mat3  uR;
        uniform vec3  uCam;
        uniform vec3  uT;
        uniform float uH;
        uniform vec2  uC;
        uniform vec2  uFb;
        uniform float uNear;
        uniform usampler2D uHalves;
        uniform int uHalfGate;
        uniform int uWorldSnap;
        // 0085. WaterSwell's field: a corner the port flagged free (bit 27) moves by
        // it, rounded to the whole unit the packets move it by. Per wave: its
        // wavenumber along X and Z, its height, its phase.
        uniform int  uSwellOn;
        uniform vec4 uSwell[3];
        float swellDy(vec3 p) {
            float h = 0.0;
            for (int i = 0; i < 3; i++)
                h += uSwell[i].z * sin(uSwell[i].x * p.x + uSwell[i].y * p.z - uSwell[i].w);
            return floor(-h + 0.5);
        }
        //@model
        // A model's blended faces (uModelBlend 1), as their packets reached the surface
        // list: a solid model's as the opaque surface it stands for; else one with a
        // material, or on the water's texture in an averaging blend, as that blended
        // surface; any other is no surface. uModelTwin is the forced rate, or -1.
        uniform int uModelBlend;
        uniform int uModelSolid;
        uniform int uModelTwin;

        void main() {
            uint flags = inFlags;
            vec3 w = inWorld;
            vFadeXZ = vec2(0.0);
            if (uModel != 0) {
                uint m = uModelMat;
                if (uModelBlend != 0) {
                    int mode = uModelTwin >= 0 ? uModelTwin : int((inFlags >> 8) & 3u);
                    bool water = (inFlags & 0x10000000u) != 0u && (mode == 0 || mode == 3);
                    if (m == 0u) m = water ? 2u : uModelSolid != 0 ? 1u : 0u;
                }
                if ((uModelBlend != 0 && m == 0u) || !modelFaceKept(inCue, inRgbc)) {
                    gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
                    vDepth = 0.0; vM = 0.0; vUv = vec2(0.0); vTex = 0u;
                    return;
                }
                w = modelVertex(uint(inWorld.x));
                flags = (inFlags & ~(255u | 0x10000400u)) | m;
                if (uModelBlend != 0 && uModelSolid == 0) flags |= 0x10000400u;
            }
            uint hid = (flags >> 13) & 0x3FFFu;
            if (uHalfGate != 0 && hid != 0u
                && texelFetch(uHalves, ivec2(int((hid - 1u) % 160u), int((hid - 1u) / 160u)), 0).r == 0u) {
                gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
                vDepth = 0.0; vM = 0.0; vUv = vec2(0.0); vTex = 0u;
                return;
            }
            // A blended face is kept only as water (bit 28), with 256 over its id
            // as a blended packet carries it; any other blended face is no surface.
            bool semi = (flags & 0x400u) != 0u;
            if (semi && (flags & 0x10000000u) == 0u) {
                gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
                vDepth = 0.0; vM = 0.0; vUv = vec2(0.0); vTex = 0u;
                return;
            }
            if (uSwellOn != 0 && (flags & 0x8000000u) != 0u) w.y += swellDy(w);
            vFadeXZ = uModel != 0 && (uModelView != 0 || uModelSky != 0) ? uFade.xy : w.xz;
            vec3 v = uR * (w - uCam) + uT;
            if (uModel != 0 && uModelView != 0) v = modelEye(uint(inWorld.x));
            float z = v.z;
            gl_Position = vec4((uC * z + uH * v.xy) * 2.0 / uFb - z, z - 2.0 * uNear, z);
            if (uWorldSnap != 0 && z > 0.0)
                gl_Position.xy = (floor(uC + uH * v.xy / z) * 2.0 / uFb - 1.0) * z;
            gl_Position = modelPlace(gl_Position, v);
            vDepth = z > 0.0 ? z * (1.0 / 65536.0) : 0.0;
            uint m = flags & 255u;
            vM = semi ? float(uModel != 0 ? m : 2u) + 256.0 : float(m == 0u ? 1u : m);
            vUv = vec2(0.0);
            vTex = 0u;
        }
        """.Replace("//@model", ModelGlsl);

    /// <summary>
    /// 0067. Screen-space reflections. For each pixel whose surface reflects, the
    /// view ray is reflected about the surface's own plane (the surface buffer's
    /// normal, from the polygon rather than from the depth) and marched through
    /// the finished frame's depth buffer, projected with the GTE's own H and
    /// centre. Where it passes behind a depth it has hit that surface, and the
    /// colour there is the reflection.
    ///
    /// The ray starts on the water, not under it: the depth buffer at a water pixel
    /// is the floor of the pool, because the water is translucent and wrote none,
    /// and the surface buffer is where the water's own depth is kept. That same
    /// fact keeps the ray from hitting the pool floor -- the ray leaves upwards,
    /// and every depth below it is further away than it is.
    ///
    /// Output is premultiplied (colour times weight, weight), so the linear
    /// filtering the present samples it with does not bleed colour off an edge.
    /// </summary>
    public const string SsrFs = """
        #version 330 core
        in vec2 vUv;
        layout(location = 0) out vec4 oColor;
        // The probe's: alpha 1/255 no hit, 2/255 a surface, 3/255 the sky, 4/255 a
        // surface under the HUD, refused, 5/255 the planar texture, on every
        // reflective pixel; plus 8/255 when the ray passed behind something on the
        // way, and 16/255 on a planar pixel whose march found a surface too, with
        // green and blue the two colours' differences (see uCompare). Written to
        // nothing unless the probe attached it.
        layout(location = 1) out vec4 oInfo;

        uniform sampler2D uDepth;
        uniform sampler2D uSurface;
        uniform sampler2D uColor;
        // The same rectangle and projection the occlusion pass is given.
        uniform vec2  uOrigin;
        uniform vec2  uSize;
        uniform vec2  uTexSize;
        uniform float uProjH;
        uniform vec2  uCentre;
        uniform float uMaxDist;
        uniform float uThickness;
        uniform float uSky;
        uniform int   uSteps;
        // The screen march; 0 leaves the pass the planar lookups, the cubemap and
        // the murk.
        uniform int   uMarchOn;
        // Murk: water thickens towards its colour with the distance the view ray
        // runs through it, surface to the opaque floor behind. 0 is off.
        uniform float uMurkDist;
        // The world's vertical in view space, and the cosine a murked surface may lean to.
        uniform vec4  uMurkUp;
        // 0083. Past this view depth the surface is left as the game drew it.
        uniform float uPlainZ;
        uniform vec3  uMurkColor;
        // SurfaceMaterial's table, by id: row 0 is reflectivity, F0 and roughness.
        uniform sampler2D uMatTable;
        // The game's depth cue, off the GTE: IR0 = (DQA * H/SZ + DQB) / 4096, and
        // which of its curves turns that into a darkening (GteLightMap's numbering).
        uniform float uDqa;
        uniform float uDqb;
        uniform int   uFogCurve;
        // 0074. The area's fog colour and curve, as PrimFs takes them (the colour
        // 0..1 here), and the sky the frame is cleared to: fog turns a colour towards
        // the fog's rather than to black, and a cubemap miss reflects the sky.
        uniform int   uAtmosOn;
        uniform vec3  uAtmosColour;
        uniform vec2  uAtmosShape;
        uniform vec3  uAtmosSky;
        // 0068. The scene drawn from the camera mirrored in the water, at this
        // target's own size, and its depth, which is what says a texel was drawn.
        // The plane is this frame's, in this view: dot(xyz, p) + w is a surface's
        // height above the water in world units (negative is above, Y being down).
        uniform sampler2D uPlanar;
        uniform sampler2D uPlanarDepth;
        uniform int   uPlanarOn;
        uniform vec4  uPlanarPlane;
        uniform float uPlanarTol;
        uniform float uRipple;
        // The probe's check on the mirror: march a planar pixel as well, and where
        // both found a surface, write how far apart their brightness is, against
        // the planar texture read unmirrored as the control.
        uniform int   uCompare;
        // Rough surfaces: the colour and the planar texture shrunk into mip chains,
        // and each chain's level-0 height in texels. 0 when no id is rough.
        uniform sampler2D uColorMip;
        uniform sampler2D uPlanarMip;
        uniform float uColorMipH;
        uniform float uPlanarMipH;
        // 0072. The retained scene. Its planar texture is the one bound as uPlanar,
        // drawn straight into this target's pixels (no row flip), each pixel holding
        // the reflection in the plane its own surface lies on; the planes are this
        // frame's, in this view, as uPlanarPlane is. And a cubemap from the camera,
        // unfogged, with its depth, marched in place of the screen: uToWorld and
        // uViewT take a view position back to world axes about the camera.
        uniform int   uRetPlanarN;
        uniform vec4  uRetPlane[4];
        uniform int   uCubeOn;
        uniform samplerCube uCube;
        uniform samplerCube uCubeDepth;
        uniform mat3  uToWorld;
        uniform vec3  uViewT;
        uniform int   uCubeSteps;
        uniform float uCubeSize;

        const float FAR = 65536.0;
        const float OVERLAY = 3.0;

        // How much of a colour survives the fog at view depth z: the per-pixel
        // lighting shader's curve (shade8), evaluated at the GTE's own quotient.
        float fogKeep(float z) {
            if (uFogCurve == 0) return 1.0;
            float q = min(uProjH * 65536.0 / max(z, 1.0), 131071.0);
            float ir0 = clamp((uDqa * q + uDqb) / 4096.0, 0.0, 4096.0);
            float w = uFogCurve == 1 ? max(ir0 - 800.0, 0.0) * 2.0
                    : uFogCurve == 2 ? (ir0 < 2800.0 ? ir0 : 3.0 * ir0 - 5600.0)
                    : uFogCurve == 3 ? ir0 * 0.5
                    : ir0;
            if (uAtmosOn != 0 && uAtmosShape != vec2(1.0) && w > 0.0)
                w = 4096.0 * min(pow(min(w / 4096.0, 1.0), uAtmosShape.x), uAtmosShape.y);
            return clamp(1.0 - w / 4096.0, 0.0, 1.0);
        }

        // A colour kept `keep` of by the fog, the rest the fog's colour.
        vec3 fogTo(vec3 c, float keep) {
            return uAtmosOn != 0 ? mix(uAtmosColour, c, keep) : c * keep;
        }

        // A colour the fog took at one depth, taken at another instead: `ratio` is
        // the keep there over the keep where it was drawn.
        vec3 refog(vec3 c, float ratio) {
            return uAtmosOn != 0 ? uAtmosColour + (c - uAtmosColour) * ratio : c * ratio;
        }

        vec2 tc(vec2 uv) { return (uOrigin + uv * uSize) / uTexSize; }
        float depthAt(vec2 uv) { return texture(uDepth, tc(uv)).r; }
        // Above 511 a see-through 2D box lies over the surface: 512 for one that
        // shows half of it (blend mode 0), 1024 for any other.
        bool overlayAt(vec2 uv) { float a = texture(uSurface, tc(uv)).a; return a > 511.5 || abs(a - OVERLAY) < 0.5; }
        int surfId(float a) { return int(a + 0.5) & 511; }
        float veilShare(float a) { return (int(a + 0.5) >> 9) == 1 ? 0.5 : 1.0; }
        vec3 viewAt(vec2 uv, float z) { return vec3((uv - uCentre) * uSize * (z / uProjH), z); }
        vec2 project(vec3 q) { return uCentre + q.xy * (uProjH / q.z) / uSize; }

        vec3 octDecode(vec2 e) {
            vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
            if (n.z < 0.0)
                n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
            return normalize(n);
        }

        float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

        // A rough surface's reflection: the colour averaged over the footprint of the
        // cone the reflected ray stands for, `radius` of the picture high. Read from
        // a mip chain of the picture at the level whose texel is that wide: the
        // centre and four taps at half the radius, each trilinear, the same at every
        // pixel. Eight sparse taps turned per pixel left a woven 4x4 pattern on a
        // busy texture. Below a texel it fades back to the sharp read.
        vec3 blurAt(sampler2D tex, sampler2D mip, float mipH, vec2 uv, float radius, bool mirrored) {
            vec3 c0 = texture(tex, tc(uv)).rgb;
            if (radius <= 0.0 || mipH <= 0.0) return c0;
            float texels = radius * uSize.y / uTexSize.y * mipH;
            float lod = log2(max(texels, 1.0));
            vec3 sum = textureLod(mip, tc(uv), lod).rgb;
            float n = 1.0;
            vec2 d = 0.5 * radius * vec2(uSize.y / uSize.x, 1.0);
            for (int k = 0; k < 4; k++) {
                vec2 q = uv + d * vec2(k < 2 ? -1.0 : 1.0, (k & 1) == 0 ? -1.0 : 1.0);
                if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0) continue;
                if (!mirrored && overlayAt(q)) continue;
                sum += textureLod(mip, tc(q), lod).rgb;
                n += 1.0;
            }
            return mix(c0, sum / n, clamp(texels * 2.0, 0.0, 1.0));
        }

        // A metal's reflection takes its colour: the surface's hue at full value,
        // pushed a little away from grey, so a dark bronze tints without darkening
        // what it reflects and near-grey stone still reads as coloured.
        vec3 metalTint(float metal) {
            if (metal <= 0.0) return vec3(1.0);
            vec3 sc = texture(uColor, tc(vUv)).rgb;
            vec3 hue = sc / max(max(sc.r, max(sc.g, sc.b)), 1e-3);
            hue = max(mix(vec3(luma(hue)), hue, 1.0 + 0.5 * metal), 0.0);
            hue /= max(max(hue.r, max(hue.g, hue.b)), 1e-3);
            return mix(vec3(1.0), hue, metal);
        }

        // A metal's own colour is darker: half of it is taken off at metalness 1,
        // hit or miss, and the reflection's weight comes off what is left. With no
        // metal the weight is the reflection's alone, to the bit.
        float gMetalDark = 0.0;
        // The murk's share and its (fogged) colour, laid under the reflection.
        float gMurk = 0.0;
        vec3 gMurkCol = vec3(0.0);
        // How much of the surface a see-through box over it lets show.
        float gShare = 1.0;
        void emit(vec3 c, float w) {
            w = clamp(w, 0.0, 1.0);
            float a = gMetalDark > 0.0 ? 1.0 - (1.0 - gMetalDark) * (1.0 - w) : w;
            vec3 rgb = c * w;
            if (gMurk > 0.0) {
                rgb += gMurkCol * gMurk * (1.0 - a);
                a = 1.0 - (1.0 - a) * (1.0 - gMurk);
            }
            oColor = vec4(rgb, a) * gShare;
        }

        // 0068. The planar reflection at this pixel, when the surface lies on the
        // mirrored plane and something above the water was drawn where it looks:
        // the mirrored image of display row y is row 2*centre - y. The water's own
        // brightness gradient bends the lookup, so the reflection moves with the
        // scrolling texture rather than lying on it like glass. The colour was
        // fogged by the mirrored camera, whose distance to it is the length of the
        // path through the mirror, so it needs no correction here.
        bool onPlanar(vec3 p) {
            return uPlanarOn != 0 && abs(dot(uPlanarPlane.xyz, p) + uPlanarPlane.w) <= uPlanarTol;
        }
        // The surface is on the plane and the mirror drew nothing there: what it
        // sees is the background, past everything the walk drew.
        bool gPlanarEmpty = false;
        bool planarAt(vec3 p, float rough, out vec3 c) {
            c = vec3(0.0);
            if (!onPlanar(p)) return false;
            vec2 muv = vec2(vUv.x, 2.0 * uCentre.y - vUv.y);
            if (uRipple > 0.0) {
                vec2 px = 1.0 / uSize;
                float l0 = luma(texture(uColor, tc(vUv)).rgb);
                float lx = luma(texture(uColor, tc(vUv + vec2(px.x, 0.0))).rgb);
                float ly = luma(texture(uColor, tc(vUv + vec2(0.0, px.y))).rgb);
                muv += vec2(lx - l0, ly - l0) * uRipple * px;
            }
            muv = clamp(muv, vec2(0.0), vec2(1.0));
            vec3 pc = texture(uPlanar, tc(muv)).rgb;
            // Nothing drawn: the capture cleared to black with the far plane, and an
            // opaque surface writes a depth whatever its colour.
            if (texture(uPlanarDepth, tc(muv)).r >= 1.0 && max(pc.r, max(pc.g, pc.b)) <= 0.0) { gPlanarEmpty = true; return false; }
            // Roughness blurs it as it does a march's hit: the image stands as far
            // behind the water as its source stands above it, and the planar depth
            // is the mirrored eye's distance to it.
            c = pc;
            if (rough > 0.0) {
                float zi = texture(uPlanarDepth, tc(muv)).r * FAR;
                float travel = zi < FAR * 0.999 ? max(zi - p.z, 0.0) * length(p) / p.z : 0.0;
                c = blurAt(uPlanar, uPlanarMip, uPlanarMipH, muv, rough * travel * uProjH / (max(zi, 1.0) * uSize.y), true);
            }
            return true;
        }

        // 0072. The retained planar reflection at this pixel: the first of the
        // frame's planes its surface lies on, read unmirrored, bent by the water's
        // own brightness gradient as the old lookup was. gRetEmpty: the surface is
        // on a plane and the mirror drew nothing there, which is exact -- open sky
        // -- and not a cue to march the cubemap, whose answer has the camera's
        // parallax.
        bool gRetEmpty = false;
        bool retPlanarAt(vec3 p, float rough, out vec3 c) {
            c = vec3(0.0);
            if (uRetPlanarN == 0) return false;
            bool on = false;
            for (int k = 0; k < 4; k++) {
                if (k >= uRetPlanarN) break;
                if (abs(dot(uRetPlane[k].xyz, p) + uRetPlane[k].w) <= uPlanarTol) { on = true; break; }
            }
            if (!on) return false;
            vec2 muv = vUv;
            if (uRipple > 0.0) {
                vec2 px = 1.0 / uSize;
                float l0 = luma(texture(uColor, tc(vUv)).rgb);
                float lx = luma(texture(uColor, tc(vUv + vec2(px.x, 0.0))).rgb);
                float ly = luma(texture(uColor, tc(vUv + vec2(0.0, px.y))).rgb);
                muv += vec2(lx - l0, ly - l0) * uRipple * px;
            }
            muv = clamp(muv, vec2(0.0), vec2(1.0));
            vec3 pc = texture(uPlanar, tc(muv)).rgb;
            float pd = texture(uPlanarDepth, tc(muv)).r;
            if (pd >= 1.0 && max(pc.r, max(pc.g, pc.b)) <= 0.0) { gRetEmpty = true; return false; }
            c = pc;
            if (rough > 0.0) {
                float zi = pd * FAR;
                float travel = zi < FAR * 0.999 ? max(zi - p.z, 0.0) * length(p) / p.z : 0.0;
                c = blurAt(uPlanar, uPlanarMip, uPlanarMipH, muv, rough * travel * uProjH / (max(zi, 1.0) * uSize.y), true);
            }
            return true;
        }

        // 0072. The distance from the camera to the first surface along a world
        // direction: the face's depth is its own view depth, so divide by the
        // direction's share along the face's axis. Nothing drawn is past everything.
        float cubeDist(vec3 d) {
            float z = texture(uCubeDepth, d).r;
            if (z >= 1.0) return FAR * 8.0;
            vec3 a = abs(d);
            return z * FAR / max(a.x, max(a.y, a.z));
        }

        // 0072. The reflected ray marched against the cubemap's depth, in world axes
        // about the camera: a step whose point stands further from the camera than
        // the surface the camera sees in its direction has gone behind it. Halved
        // back to the crossing, it is a hit only where the ray is at that surface;
        // one that passed behind an object from the camera's side marches on. No
        // per-pixel jitter: the steps are fine enough, and a pattern is worse.
        bool cubeMarch(vec3 p, vec3 r, float rough, out vec3 c, out float tHit) {
            c = vec3(0.0);
            tHit = 0.0;
            vec3 P = uToWorld * (p - uViewT);
            vec3 D = normalize(uToWorld * r);
            float n1 = float(max(uCubeSteps, 1));
            float tPrev = 0.0;
            for (int i = 0; i < 256; i++) {
                if (i >= uCubeSteps) break;
                float x = (float(i) + 1.0) / n1;
                float t = uMaxDist * x * x + 8.0;
                vec3 q = P + D * t;
                float len = length(q);
                float sd = cubeDist(q / len);
                if (len > sd * 1.002 + 2.0) {
                    float lo = tPrev, hi = t;
                    for (int k = 0; k < 6; k++) {
                        float mid = 0.5 * (lo + hi);
                        vec3 qm = P + D * mid;
                        float lm = length(qm);
                        if (lm > cubeDist(qm / lm) * 1.002 + 2.0) hi = mid; else lo = mid;
                    }
                    vec3 qh = P + D * hi;
                    float lh = length(qh);
                    if (lh - cubeDist(qh / lh) < uThickness + (hi - lo)) {
                        tHit = hi;
                        // The cone's width where it lands, as texels of a face seen
                        // from the camera: a face spans a right angle.
                        float texels = rough * hi / max(lh, 1.0) * uCubeSize / 1.5708;
                        c = textureLod(uCube, qh / lh, log2(max(texels, 1.0))).rgb;
                        return true;
                    }
                }
                tPrev = t;
            }
            return false;
        }

        // Fades a reflection out as its source nears the picture's edge, where the
        // march loses the surface it would have hit a few pixels further on.
        float edgeFade(vec2 uv) {
            vec2 e = smoothstep(vec2(0.0), vec2(0.06), uv) * smoothstep(vec2(0.0), vec2(0.06), 1.0 - uv);
            return e.x * e.y;
        }

        void main() {
            oColor = vec4(0.0);
            oInfo = vec4(0.0);
            vec4 s = texture(uSurface, tc(vUv));
            gShare = veilShare(s.a);
            int m = surfId(s.a);
            // Preserve explicit UI coverage before scene crack repair.
            if (m == 3) return;
            // A texel the water's triangles left uncovered between two that are
            // water is water: the tiles meet with hairline cracks, and the murk
            // made each one a line.
            if (m != 2) {
                vec2 tx = 1.0 / uTexSize;
                vec4 a0 = texture(uSurface, tc(vUv) - vec2(tx.x, 0.0)), a1 = texture(uSurface, tc(vUv) + vec2(tx.x, 0.0));
                vec4 b0 = texture(uSurface, tc(vUv) - vec2(0.0, tx.y)), b1 = texture(uSurface, tc(vUv) + vec2(0.0, tx.y));
                if (surfId(a0.a) == 2 && surfId(a1.a) == 2) { s = a0; s.b = 0.5 * (a0.b + a1.b); m = 2; }
                else if (surfId(b0.a) == 2 && surfId(b1.a) == 2) { s = b0; s.b = 0.5 * (b0.b + b1.b); m = 2; }
            }
            // Red is the material here, for the probe's map.
            oInfo = vec4(float(clamp(m, 0, 255)) / 255.0, depthAt(vUv) >= 1.0 ? 1.0 / 255.0 : 0.0, 0.0, 0.0);
            if (m <= 0 || m >= 256) return;
            vec4 mat = texelFetch(uMatTable, ivec2(m, 0), 0);
            float refl = mat.r;
            bool murky = uMurkDist > 0.0 && m == 2 && abs(dot(octDecode(s.rg), uMurkUp.xyz)) >= uMurkUp.w;
            if (refl <= 0.0 && !murky) return;
            if (refl > 0.0) oInfo.a = 1.0 / 255.0;

            float zs = s.b * FAR;
            if (zs <= 1.0) return;
            // With the Z-buffer on, visibility is the depth test's and not the
            // order's, so a surface redrawn last may still be behind the opaque
            // one the picture shows. The picture is the authority.
            float d = depthAt(vUv);
            if (d > 0.0 && d < 1.0 && d * FAR < zs * 0.99 - 8.0) return;

            vec3 p = viewAt(vUv, zs);
            // 0083. Past the enhancement distance, the game's own look; the planar
            // reflection is left out of it, since what it shows is fogged by the
            // mirror's own depth and meets the black of the distance by itself.
            if (uPlainZ > 0.0 && !onPlanar(p)) {
                gShare *= 1.0 - smoothstep(uPlainZ - 2048.0, uPlainZ, zs);
                if (gShare <= 0.0) return;
            }
            // A translucent surface writes no depth, so the depth buffer holds the
            // floor under it: the ray's run between the two is how much water it
            // crosses. An opaque surface has none; the sky behind is all water.
            if (murky) {
                // The same for a crack in the floor under the water: the nearest
                // depth beside it, rather than the sky's full run.
                if (d <= 0.0 || d >= 1.0) {
                    vec2 tx = 1.0 / uTexSize;
                    float n0 = texture(uDepth, tc(vUv) - vec2(tx.x, 0.0)).r, n1 = texture(uDepth, tc(vUv) + vec2(tx.x, 0.0)).r;
                    float n2 = texture(uDepth, tc(vUv) - vec2(0.0, tx.y)).r, n3 = texture(uDepth, tc(vUv) + vec2(0.0, tx.y)).r;
                    // Only a depth behind the water: beside something standing
                    // in it, the nearest is that thing, and the run came out 0.
                    float nd = 1.0, zw = zs / FAR;
                    if (n0 > zw) nd = min(nd, n0);
                    if (n1 > zw) nd = min(nd, n1);
                    if (n2 > zw) nd = min(nd, n2);
                    if (n3 > zw) nd = min(nd, n3);
                    if (nd < 1.0) d = nd;
                }
                float run = (d <= 0.0 || d >= 1.0) ? FAR : max(d * FAR - zs, 0.0) * length(p) / zs;
                gMurk = 1.0 - exp(-run / uMurkDist);
                gMurkCol = fogTo(uMurkColor, fogKeep(zs));
            }
            if (refl <= 0.0) { emit(vec3(0.0), 0.0); return; }
            vec3 n = octDecode(s.rg);
            vec3 v = normalize(p);
            vec3 r = reflect(v, n);
            float cosv = clamp(dot(-v, n), 0.0, 1.0);
            float f0 = mat.g;
            // Squared, so the slider's lower half is the useful range.
            float rough = mat.b * mat.b;
            vec3 tint = metalTint(mat.a);
            gMetalDark = 0.5 * mat.a;
            // Schlick, running from F0 looking straight down to the material's
            // reflectivity at a grazing angle.
            float w = f0 + (max(refl, f0) - f0) * pow(1.0 - cosv, 5.0);

            vec3 pc;
            bool planarHit = planarAt(p, rough, pc) || retPlanarAt(p, rough, pc);
            if (gRetEmpty && uCompare == 0) { emit(vec3(0.0), 0.0); return; }
            if (planarHit && uCompare == 0) {
                oInfo.a += 4.0 / 255.0;
                emit(pc * tint, w);
                return;
            }
            // 0068. The planar answer is final on its plane: an empty texel is the
            // background (black, or the remaster's sky), which the fogged texels
            // beside it fade into, and never a march.
            if (gPlanarEmpty && uCompare == 0) {
                oInfo.a += 4.0 / 255.0;
                emit((uAtmosOn != 0 ? uAtmosSky : vec3(0.0)) * tint, w);
                return;
            }

            // 0072. The cubemap in place of the screen: nothing it cannot see is
            // borrowed from the picture, and a miss reflects nothing.
            if (uCubeOn != 0) {
                vec3 cc;
                float ct;
                bool cubeHit = cubeMarch(p, r, rough, cc, ct);
                if (planarHit) {
                    // The probe's check on the two: where both found a surface, how
                    // far apart they are, against the planar texture read mirrored
                    // (the wrong way round, for this one) as the control.
                    oInfo.a += 4.0 / 255.0;
                    if (cubeHit) {
                        vec3 sc = fogTo(cc, fogKeep(p.z * (length(p) + ct) / length(p)));
                        oInfo.b = abs(luma(pc) - luma(sc));
                        oInfo.g = abs(luma(texture(uPlanar, tc(vec2(vUv.x, 2.0 * uCentre.y - vUv.y))).rgb) - luma(sc));
                        oInfo.a += 16.0 / 255.0;
                    }
                    emit(pc * tint, w);
                    return;
                }
                if (!cubeHit) {
                    if (uAtmosOn != 0) emit(uAtmosSky * tint, w);
                    else emit(vec3(0.0), 0.0);
                    return;
                }
                float zImage = p.z * (length(p) + ct) / length(p);
                float keep = fogKeep(zImage);
                w *= 1.0 - smoothstep(0.7, 1.0, ct / uMaxDist);
                oInfo.a += 1.0 / 255.0;
                oInfo.b = keep;
                emit(fogTo(cc, keep) * tint, w);
                return;
            }

            // No screen march: a planar pixel the check kept this far takes its
            // lookup, and anything else reflects nothing.
            if (uMarchOn == 0) {
                if (planarHit) { oInfo.a += 4.0 / 255.0; emit(pc * tint, w); }
                else emit(vec3(0.0), 0.0);
                return;
            }

            // The same 4x4 interleaved pattern the occlusion pass uses, as a start
            // offset along the ray, so step banding becomes a fine grain.
            ivec2 px = ivec2(gl_FragCoord.xy) & 3;
            float jitter = (float((px.y << 2) | px.x) + 0.5) / 16.0;

            float n1 = float(max(uSteps, 1));
            float tPrev = 0.0;
            bool hit = false, passed = false;
            vec2 huv = vec2(0.0), bgUv = vec2(-1.0);
            float ht = 0.0;
            for (int i = 0; i < 128; i++) {
                if (i >= uSteps) break;
                float x = (float(i) + jitter) / n1;
                float t = uMaxDist * x * x + 4.0;
                vec3 q = p + r * t;
                if (q.z < 8.0) break;
                vec2 uv = project(q);
                if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) break;
                float sd = depthAt(uv);
                // A background pixel is only the sky if nothing 2D was drawn over it.
                if (sd >= 1.0 || sd <= 0.0) { if (!overlayAt(uv)) bgUv = uv; tPrev = t; continue; }
                float dz = q.z - sd * FAR;
                if (dz > 0.0 && dz < uThickness + (t - tPrev) * abs(r.z)) {
                    // Halve back to where the ray crossed the surface.
                    float lo = tPrev, hi = t;
                    for (int k = 0; k < 5; k++) {
                        float mid = 0.5 * (lo + hi);
                        vec3 qm = p + r * mid;
                        float md = depthAt(project(qm));
                        if (md > 0.0 && md < 1.0 && qm.z > md * FAR) hi = mid; else lo = mid;
                    }
                    // The step's own run lets a coarse step land well behind a
                    // surface, which is also what a ray passing *behind* a thin
                    // object does -- the gem floating over the pool, whose
                    // reflection then trailed down the water. Where the ray crossed
                    // a real surface it is at that surface once halved back; where
                    // it passed behind one, the halving stops at the silhouette
                    // with the ray still far behind. Only the first is a hit.
                    vec3 qh = p + r * hi;
                    vec2 uh = project(qh);
                    float hd = depthAt(uh);
                    if (hd > 0.0 && hd < 1.0 && qh.z - hd * FAR < uThickness) {
                        ht = hi;
                        huv = uh;
                        hit = true;
                        break;
                    }
                    passed = true;
                }
                tPrev = t;
            }
            if (planarHit) {
                oInfo.a += 4.0 / 255.0;
                if (hit && !overlayAt(huv) && edgeFade(huv) > 0.99) {
                    vec3 sc = texture(uColor, tc(huv)).rgb;
                    float zHit = viewAt(huv, depthAt(huv) * FAR).z;
                    float zImage = p.z * (length(p) + ht) / length(p);
                    sc = refog(sc, clamp(fogKeep(zImage) / max(fogKeep(zHit), 1e-3), 0.0, 1.0));
                    oInfo.b = abs(luma(pc) - luma(sc));
                    oInfo.g = abs(luma(texture(uPlanar, tc(vUv)).rgb) - luma(sc));
                    oInfo.a += 16.0 / 255.0;
                }
                emit(pc * tint, w);
                return;
            }
            if (passed) oInfo.a += 8.0 / 255.0;

            vec3 c;
            // A surface under the HUD is hidden by it: its colour there is the HUD's.
            if (hit && overlayAt(huv)) { oInfo.a += 3.0 / 255.0; emit(vec3(0.0), 0.0); return; }
            if (hit) {
                w *= edgeFade(huv) * (1.0 - smoothstep(0.7, 1.0, ht / uMaxDist));
                // The cone's width where it lands, `rough * ht` across, as a share of
                // the picture's height at the hit's depth.
                float hz = max(depthAt(huv) * FAR, 1.0);
                c = blurAt(uColor, uColorMip, uColorMipH, huv, rough * ht * uProjH / (hz * uSize.y), false);
                // The colour there was fogged for its own distance, and the light
                // reaching the water has come further: out to the water and back up
                // to the surface. Its image stands that much further down the mirrored
                // view ray, so it is fogged at that depth. The game's fog is a
                // darkening, so a surface lost in it reflects black -- which is what
                // the void past the draw distance reflects too, so nothing pops at
                // the fog's edge.
                float zHit = viewAt(huv, depthAt(huv) * FAR).z;
                float zImage = p.z * (length(p) + ht) / length(p);
                float keep = clamp(fogKeep(zImage) / max(fogKeep(zHit), 1e-3), 0.0, 1.0);
                c = refog(c, keep);
                oInfo.b = keep;
                oInfo.a += 1.0 / 255.0;
            } else if (bgUv.x >= 0.0 && uSky > 0.0) {
                w *= uSky * edgeFade(bgUv);
                c = texture(uColor, tc(bgUv)).rgb;
                oInfo.a += 2.0 / 255.0;
            } else {
                emit(vec3(0.0), 0.0);
                return;
            }
            emit(c * tint, w);
        }
        """;

    /// <summary>
    /// 0072. The retained scene's vertex shader: a world-space corner through a
    /// camera the backend hands it, to exactly the outputs <c>PrimVs</c> gives
    /// <c>PrimFs</c>, so a reflected texel is decoded, filtered and blended by the
    /// same code as a drawn one. The projection is the GTE's -- the centre plus H
    /// times x/z -- written as a clip-space position with W the view depth, so the
    /// rasterizer interpolates the texture coordinate perspective-correctly and
    /// clips against a near plane, which a packet of already-projected corners
    /// never needed. The depth cue is the one the corner's light record would have
    /// fogged it with, at this camera's depth: through a mirror that is the length
    /// of the path, which is the fog the reflection wants. A cubemap face draws
    /// unfogged, and the reflection pass fogs its hit for the path instead.
    ///
    /// With uMirror set the corner is drawn where its image in the plane
    /// <c>Y = uPlaneY</c> stands, seen from the real camera, so the planar texture
    /// lines up with the picture pixel for pixel; what lies below the plane is
    /// clipped away before it is mirrored.
    /// </summary>
    public static readonly string WorldVs = """
        #version 330 core
        layout(location = 0) in vec3  inWorld;
        layout(location = 1) in vec3  inColorF;
        layout(location = 2) in float inClutF;
        layout(location = 3) in float inTexpageF;
        layout(location = 4) in vec2  inUV;
        layout(location = 5) in vec3  inCue;
        layout(location = 6) in uint  inRect;
        layout(location = 7) in uint  inFlags;
        // The colour the game lit the corner from, which a light and a glow scale; and
        // its mip atlas entry (0060), 0 for none.
        layout(location = 8) in uint  inRgbc;
        layout(location = 9) in uint  inMip;
        // 0085. A static map corner lit here from the light records (bit 31): see
        // RetainedScene.Vertex.Light.
        layout(location = 10) in uint inLight;

        invariant gl_Position;

        noperspective out vec4 vColor;
        out vec2 vUV;
        out float vDepth;
        flat out ivec2 clutBase;
        flat out ivec2 pageBase;
        flat out int   texMode;
        flat out int   vDither;
        flat out int   vRepClut;
        noperspective out vec3 vLit;
        noperspective out float vFog;
        // 0085. The corner's DQA and DQB, for PrimFs to fog at the pixel's own depth.
        out vec2 vCue;
        flat out uint vLight;
        flat out uvec2 vTex;
        flat out uint vMat;
        // 0072, amended. A half's weight in the gate, faded in and out by the port;
        // PrimFs dithers it away. 1 for everything the gate does not weigh.
        flat out float vFade;
        // 0088. A record-lit map corner while NeighbourBlend is on: x bit 31 set, 30
        // the fog on, 29 and 28 the fog and the light blended, bits 14-19 the half's
        // record, 0-13 its half flag; y its RGBC. Its world X and Z, and its own
        // record's light dots. 0 for everything else.
        flat out uvec2 vNb;
        out vec2 vNbXZ;
        noperspective out vec3 vNbDots;
        uniform int uNeighbour;
        // 0089. The corner's world X and Z, for DistanceFade in PrimFs; the camera's
        // (uFade.xy) for the sky and a model placed in view space, which never fade.
        out vec2 vFadeXZ;
        uniform vec4 uFade;

        uniform mat3  uR;
        uniform vec3  uCam;
        uniform vec3  uT;
        uniform float uH;
        uniform vec2  uC;
        uniform vec2  uFb;
        uniform float uNear;
        uniform float uCueH;
        uniform int   uFogOn;
        uniform int   uMirror;
        uniform float uPlaneY;
        uniform float uPlaneBias;
        // The map halves the frame's walk drew, one byte each; 1 draws only those,
        // 2 only the others (the probe's count). Models carry no half.
        uniform usampler2D uHalves;
        uniform int uHalfGate;
        // Authored lights or a glow are drawn: the corner carries its RGBC to them.
        uniform int uWorldLit;
        // 0085. inMip is a texture's index plus one into this table of atlas
        // entries (the static map), not the entry itself (the frame's models).
        uniform usamplerBuffer uMipTable;
        uniform int uMipIndirect;
        // 0085. The main view follows the frame's settings: corner positions on whole
        // pixels (sub-pixel off), the depth cue at the corners (per-pixel lighting
        // off), and the crosshatch. The reflections leave all three at their defaults.
        uniform int uWorldSnap;
        uniform int uWorldPerPixel;
        uniform int uWorldDither;
        // 0085. WaterSwell's field: a corner the port flagged free (bit 27) moves by
        // it, rounded to the whole unit the packets move it by. Per wave: its
        // wavenumber along X and Z, its height, its phase.
        uniform int  uSwellOn;
        uniform vec4 uSwell[3];
        float swellDy(vec3 p) {
            float h = 0.0;
            for (int i = 0; i < 3; i++)
                h += uSwell[i].z * sin(uSwell[i].x * p.x + uSwell[i].y * p.z - uSwell[i].w);
            return floor(-h + 0.5);
        }
        // 0085. A model's gouraud corner (bit 30) carries its normal's three light
        // dots, lit by these as PrimFs's shade8 lights a directional record.
        uniform vec3 uLightBk;
        uniform vec3 uLcmR;
        uniform vec3 uLcmG;
        uniform vec3 uLcmB;
        //@model

        // 0085. The area's light records, 52 ints each in 13 texels (RetainedScene.Records):
        // the light matrix per quarter turn, the colour matrix, the back colour, the fog
        // word, its DQA and DQB, and its curve. A corner is lit as the tile assembler
        // lights its face (NormalColorCol) and blended as EvenFog blends it, in the
        // same integers, so a record the game rewrites is an upload and not a rebuild.
        uniform isampler2D uRecords;
        int recInt(int r, int i) { return texelFetch(uRecords, ivec2(i >> 2, r), 0)[i & 3]; }

        // Round half away from zero of (k . v) / t, exactly, as the CPU's double does:
        // k up to 4096^2 and v a short do not fit an int product, so k is split in two
        // and a float guess is corrected by an exact test of the half-way bounds.
        int mixExact(ivec4 k, ivec4 v, int t) {
            ivec4 kh = k >> 12, kl = k & 4095;
            int hi = kh.x * v.x + kh.y * v.y + kh.z * v.z + kh.w * v.w;
            int lo = kl.x * v.x + kl.y * v.y + kl.z * v.z + kl.w * v.w;
            float q = (float(hi) * 4096.0 + float(lo)) / float(t);
            int sgn = q < 0.0 ? -1 : 1;
            hi *= sgn; lo *= sgn;
            int c = int(floor(abs(q) + 0.5));
            // D = 2S - (2c-1)t, in [0, 2t) when c is right.
            int m = 2 * c - 1;
            int d = (2 * hi - m * (t >> 12)) * 4096 + (2 * lo - m * (t & 4095));
            if (d < 0) c--;
            else if (d >= 2 * t) c++;
            return sgn * c;
        }

        bool sameLight(int a, int b) {
            for (int i = 9; i < 12; i++)
                if (texelFetch(uRecords, ivec2(i, a), 0) != texelFetch(uRecords, ivec2(i, b), 0)) return false;
            return true;
        }

        void recordLit(uint l, vec3 normal, float ax, float az, uint rgbc, out vec3 color, out vec3 cue, out ivec3 dots) {
            int own = int(l & 63u), rot = int((l >> 6) & 3u);
            ivec3 nb = ivec3(int((l >> 8) & 63u), int((l >> 14) & 63u), int((l >> 20) & 63u));
            // TileWeights: own, along X, along Z, the diagonal; 0 where there is no half.
            int iax = int(ax + 0.5), iaz = int(az + 0.5);
            ivec4 k = ivec4((4096 - iax) * (4096 - iaz),
                            (l & (1u << 26)) != 0u ? iax * (4096 - iaz) : 0,
                            (l & (1u << 27)) != 0u ? (4096 - iax) * iaz : 0,
                            (l & (1u << 28)) != 0u ? iax * iaz : 0);
            ivec4 rec = ivec4(own, nb);
            int t = k.x + k.y + k.z + k.w;

            // The light: the own record's light matrix; the colour matrix and back
            // colour blended where a neighbour weighing in lights otherwise.
            bool mixL = (l & 0x40000000u) != 0u
                && ((k.y != 0 && !sameLight(nb.x, own)) || (k.z != 0 && !sameLight(nb.y, own))
                    || (k.w != 0 && !sameLight(nb.z, own)));
            ivec3 n = ivec3(normal);
            ivec3 a;
            for (int j = 0; j < 3; j++) {
                int v = recInt(own, rot * 9 + 3 * j) * n.x + recInt(own, rot * 9 + 3 * j + 1) * n.y
                      + recInt(own, rot * 9 + 3 * j + 2) * n.z;
                a[j] = clamp(v >> 12, 0, 0x7FFF);
            }
            dots = a;
            for (int c = 0; c < 3; c++) {
                int bk = recInt(own, 45 + c);
                if (mixL) bk = mixExact(k, ivec4(bk, recInt(rec.y, 45 + c), recInt(rec.z, 45 + c), recInt(rec.w, 45 + c)), t);
                int v = bk << 12;
                for (int j = 0; j < 3; j++) {
                    int i = 36 + 3 * c + j;
                    int m = recInt(own, i);
                    if (mixL) m = mixExact(k, ivec4(m, recInt(rec.y, i), recInt(rec.z, i), recInt(rec.w, i)), t);
                    v += m * a[j];
                }
                int ir = clamp(v >> 12, 0, 0x7FFF);
                int mac = ((int((rgbc >> uint(8 * c)) & 255u) * ir) << 4) >> 12;
                color[c] = float(clamp(mac >> 4, 0, 255));
            }

            // The fog: the words' DQA and DQB blended where a neighbour weighing in
            // has another word; a word with no fog weighs in as no cue.
            int word = recInt(own, 48);
            cue = vec3(float(recInt(own, 49)), float(recInt(own, 50)), float(recInt(own, 51)));
            bool mixF = (l & 0x20000000u) != 0u
                && ((k.y != 0 && recInt(nb.x, 48) != word) || (k.z != 0 && recInt(nb.y, 48) != word)
                    || (k.w != 0 && recInt(nb.z, 48) != word));
            if (mixF) {
                float qa = 0.0, qb = 0.0, bent = cue.z;
                int most = 0;
                for (int i = 0; i < 4; i++) {
                    if (k[i] == 0) continue;
                    int w = recInt(rec[i], 48);
                    if (recInt(rec[i], 51) != 5 && w >= 32000) continue;
                    float f = float(k[i]) / float(t);
                    qa += f * float(recInt(rec[i], 49));
                    qb += f * float(recInt(rec[i], 50));
                    if (k[i] > most) { most = k[i]; bent = recInt(rec[i], 51) == 5 ? 5.0 : w < 0 ? 1.0 : 2.0; }
                }
                cue = vec3(qa, qb, bent);
            }
        }

        //@linearDepthCue
        float cueKeep(vec3 cue, float z) {
            int curve = int(cue.z + 0.5);
            if (uFogOn == 0 || curve == 0) return 1.0;
            if (curve == 5) return clamp(1.0 - linearDepthCue(z, cue.xy) / 4096.0, 0.0, 1.0);
            float q = min(uCueH * 65536.0 / max(z, 1.0), 131071.0);
            float ir0 = clamp((cue.x * q + cue.y) / 4096.0, 0.0, 4096.0);
            float w = curve == 1 ? max(ir0 - 800.0, 0.0) * 2.0
                    : (ir0 < 2800.0 ? ir0 : 3.0 * ir0 - 5600.0);
            return clamp(1.0 - w / 4096.0, 0.0, 1.0);
        }

        // 0051's depth pass (PrimFs's uDepthOnly): no corner's light is read.
        uniform int uDepthOnly;

        void main() {
            vec3 w = inWorld, color = inColorF, cue = inCue;
            uint flags = inFlags, rgbc = inRgbc;
            ivec3 litDots = ivec3(0);
            vNb = uvec2(0u);
            vNbXZ = vec2(0.0);
            vNbDots = vec3(0.0);
            vFadeXZ = vec2(0.0);
            if (uDepthOnly == 0 && (inLight & 0x80000000u) != 0u) {
                recordLit(inLight, inColorF, inCue.x, inCue.y, inRgbc, color, cue, litDots);
                uint nbHid = (inFlags >> 13) & 0x3FFFu;
                if (uNeighbour != 0 && uModel == 0 && nbHid != 0u) {
                    vNb = uvec2(0x80000000u | (uFogOn != 0 ? 0x40000000u : 0u)
                        | ((uNeighbour & 1) != 0 ? 0x20000000u : 0u) | ((uNeighbour & 2) != 0 ? 0x10000000u : 0u)
                        | (inLight & 63u) << 14 | nbHid, inRgbc);
                    vNbXZ = inWorld.xz;
                    vNbDots = vec3(litDots);
                }
            }
            if (uModel != 0) {
                if (!modelFaceKept(inCue, inRgbc)) {
                    gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
                    gl_ClipDistance[0] = -1.0;
                    return;
                }
                // The corner's normal, lit to its three dots by the instance's LLM.
                w = modelVertex(uint(inWorld.x));
                color = uModelLlm * inColorF;
                cue = uModelCue;
                rgbc = (inLight & 0x40000000u) != 0u ? inLight & 0xFFFFFFu : uModelRgbc;
                flags = (inFlags & ~255u) | uModelMat;
            }
            uint hid = (flags >> 13) & 0x3FFFu;
            vFade = 1.0;
            if (uHalfGate != 0) {
                uint weight = hid == 0u ? 255u
                    : texelFetch(uHalves, ivec2(int((hid - 1u) % 160u), int((hid - 1u) / 160u)), 0).r;
                bool drawn = weight != 0u;
                if (uHalfGate == 1) vFade = float(weight) / 255.0;
                if (drawn != (uHalfGate == 1)) {
                    gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
                    gl_ClipDistance[0] = -1.0;
                    return;
                }
            }
            if (uSwellOn != 0 && (flags & 0x8000000u) != 0u) w.y += swellDy(w);
            vFadeXZ = uModel != 0 && (uModelView != 0 || uModelSky != 0) ? uFade.xy : w.xz;
            gl_ClipDistance[0] = (uPlaneY - uPlaneBias) - w.y;
            if (uMirror != 0) w.y = 2.0 * uPlaneY - w.y;
            vec3 v = uR * (w - uCam) + uT;
            if (uModel != 0 && uModelView != 0) v = modelEye(uint(inWorld.x));
            float z = v.z;
            gl_Position = vec4((uC * z + uH * v.xy) * 2.0 / uFb - z, z - 2.0 * uNear, z);
            if (uWorldSnap != 0 && z > 0.0)
                gl_Position.xy = (floor(uC + uH * v.xy / z) * 2.0 / uFb - 1.0) * z;
            gl_Position = modelPlace(gl_Position, v);
            vDepth = z > 0.0 ? z * (1.0 / 65536.0) : 0.0;

            bool dots = (flags & 0x40000000u) != 0u;
            vec3 lit = color;
            if (dots) {
                vec3 a = clamp(color, 0.0, 32767.0);
                vec3 ir = clamp(uLightBk + vec3(dot(uLcmR, a), dot(uLcmG, a), dot(uLcmB, a)) / 4096.0, 0.0, 32767.0);
                lit = vec3(uvec3(rgbc, rgbc >> 8u, rgbc >> 16u) & uvec3(255u)) * ir / 4096.0;
            }
            vColor = vec4(clamp(lit * cueKeep(cue, z), 0.0, 255.0), 0.0) / 255.0;
            // The sky's packets carry no light record: the corner colour, and nothing
            // an authored light or a glow adds.
            if (uModel != 0 && uModelSky != 0) { dots = false; rgbc = 0u; cue = vec3(0.0); }
            // Fogged per pixel as 0048 fogs the game's own faces: the raw IR0 is
            // affine on screen (it goes as 1/z), so interpolated it is exact, and
            // shade8 puts it through the curve at every pixel.
            int curve = int(cue.z + 0.5);
            if (uFogOn != 0 && curve != 0 && uWorldPerPixel != 0) {
                float q = min(uCueH * 65536.0 / max(z, 1.0), 131071.0);
                vLit = color;
                vFog = curve == 5 ? linearDepthCue(z, cue.xy) : (cue.x * q + cue.y) / 4096.0;
                vCue = cue.xy;
                vLight = uint(curve) << 24;
            } else {
                vLit = vec3(0.0);
                vFog = 0.0;
                vCue = vec2(0.0);
                vLight = 0u;
            }
            if (uWorldLit != 0 && uWorldPerPixel != 0 && rgbc != 0u) {
                if (vLight == 0u) { vLit = color; vFog = 0.0; }
                vLight |= rgbc & 0xFFFFFFu;
            }
            if (dots && uWorldPerPixel != 0) {
                if (vLight == 0u) { vFog = 0.0; vCue = vec2(0.0); }
                vLit = color;
                vLight = (vLight & 0x07000000u) | 0x80000000u | (rgbc & 0xFFFFFFu);
            }
            // 0088. A blended pixel is shaded from the lit colour, per pixel lighting or not.
            if (vNb.x != 0u && vLight == 0u) vLit = color;
            uint mip = inMip;
            if (uMipIndirect != 0) mip = inMip == 0u ? 0u : texelFetch(uMipTable, int(inMip) - 1).r;
            vTex = uvec2(inRect, (flags & 0x80000000u) | mip);
            vMat = flags & 255u;
            vDither = uWorldDither;
            vRepClut = 0;
            vUV = inUV;

            int inTexpage = int(inTexpageF + 0.5);
            int inClut = int(inClutF + 0.5);
            if ((inTexpage & 0x8000) != 0) {
                texMode = 4;
            } else {
                texMode = (inTexpage >> 7) & 3;
                pageBase = ivec2((inTexpage & 0xf) * 64, ((inTexpage >> 4) & 1) * 256);
                clutBase = ivec2((inClut & 0x3f) * 16, (inClut >> 6) & 0x1ff);
            }
        }
        """.Replace("//@model", ModelGlsl).Replace("//@linearDepthCue", LinearDepthCue.Glsl);

    public const string PrimVs = """
        #version 330 core
        layout(location = 0) in vec2  inPos;
        layout(location = 1) in vec3  inColorF;
        layout(location = 2) in float inClutF;
        layout(location = 3) in float inTexpageF;
        layout(location = 4) in vec2  inUV;
        layout(location = 5) in float inW;
        layout(location = 6) in float inZ;
        layout(location = 7) in vec3  inLit;
        layout(location = 8) in float inFog;
        layout(location = 9) in uint  inLight;
        // 0060. The texture rectangle, and the atlas entry with its flags.
        layout(location = 10) in uvec2 inTex;
        // 0071. The packet's material (SurfaceMaterial), beside its light record.
        layout(location = 11) in uint  inMat;

        // 0051 draws an opaque tested batch twice with this program -- depth with
        // colour masked, then colour against it -- and the driver may compile
        // those as two variants. Only `invariant` obliges them to put a vertex in
        // the same place. See "The world lost its textures on NVIDIA" in
        // docs/RENDERING.md.
        invariant gl_Position;

        // vUV is the one thing that wants correcting: handing gl_Position a real W
        // makes the rasterizer interpolate it in 1/W, which is exactly the
        // perspective-correct mapping the PlayStation could not afford. vColor is
        // marked noperspective so that Gouraud shading stays as flat-interpolated
        // as the hardware's, and so an untextured primitive is bit-identical.
        noperspective out vec4 vColor;
        out vec2 vUV;
        out float vDepth;
        flat out ivec2 clutBase;
        flat out ivec2 pageBase;
        flat out int   texMode;
        flat out int   vDither;
        flat out int   vRepClut;
        // 0048. Both are affine across the polygon on screen, as the colour was.
        noperspective out vec3 vLit;
        noperspective out float vFog;
        out vec2 vCue;
        flat out uint vLight;
        flat out uvec2 vTex;
        flat out uint vMat;
        flat out float vFade;
        // 0088. The world program's; no packet is blended.
        flat out uvec2 vNb;
        out vec2 vNbXZ;
        noperspective out vec3 vNbDots;
        // 0089. The world program's; no packet is faded.
        out vec2 vFadeXZ;

        uniform vec2 uVertexOffset;
        uniform vec2 uPosBias;
        uniform vec2 uFbInv;

        void main() {
            vec2 p = (inPos + uVertexOffset + uPosBias) * uFbInv - 1.0;
            // W is 1 where no depth was recovered, and p * 1.0 is p exactly, so
            // such a primitive still lands on the same pixels to the last bit.
            // **No select on inW.** `inW == 1.0 ? vec4(p, 0, 1) : vec4(p * inW, 0, inW)`
            // lost every world texture on NVIDIA with the depth buffer on: the
            // colour-masked pre-pass (0051) is a state-recompiled variant of this
            // program, and some corners came out at W 1 among real depths, so the
            // 1/W weights handed the whole polygon one corner's texel. See "The
            // world lost its textures on NVIDIA" in docs/RENDERING.md.
            // Depth is a fragment value, not clip-space Z: these vertices are
            // already projected, and putting SZ into gl_Position.z lets OpenGL
            // clip them against a far plane the GPU never had — a hard line
            // across the floor where the cave used to continue.
            gl_Position = vec4(p * inW, 0.0, inW);
            vDepth = inZ > 0.0 ? inZ * (1.0/65536.0) : 0.0;

            int inClut = int(inClutF + 0.5);
            int inTexpage = int(inTexpageF + 0.5);

            vColor = vec4(inColorF, 0.0) / 255.0;
            vLit = inLit;
            vFog = inFog;
            vCue = vec2(0.0);
            vLight = inLight;
            vTex = inTex;
            vMat = inMat;
            vFade = 1.0;
            vNb = uvec2(0u);
            vNbXZ = vec2(0.0);
            vNbDots = vec3(0.0);
            vFadeXZ = vec2(0.0);
            vDither = (inTexpage >> 10) & 1;
            vRepClut = (inTexpage >> 12) & 1;

            if ((inTexpage & 0x8000) != 0) {
                texMode = 4;
            } else if ((inTexpage & 0x4000) != 0) {
                texMode = 5;
                vUV = inUV;
            } else if ((inTexpage & 0x2000) != 0) {
                texMode = 6;
                vUV = inUV;
            } else {
                texMode = (inTexpage >> 7) & 3;
                vUV = inUV;
                pageBase = ivec2((inTexpage & 0xf) * 64, ((inTexpage >> 4) & 1) * 256);
                clutBase = ivec2((inClut & 0x3f) * 16, (inClut >> 6) & 0x1ff);
            }
        }
        """;

    public static readonly string PrimFs = """
        #version 330 core
        noperspective in vec4 vColor;
        in vec2 vUV;
        in float vDepth;
        flat in ivec2 clutBase;
        flat in ivec2 pageBase;
        flat in int   texMode;
        flat in int   vDither;
        flat in int   vRepClut;
        noperspective in vec3 vLit;
        noperspective in float vFog;
        // 0085. With uCueFromZ set (the world program's main view, its H), the raw
        // depth cue is DQA * H/z + DQB at this pixel's own depth, from the corners'
        // DQA and DQB; screen-affine vFog is that only while no corner is behind the
        // eye, and a floor clipped at the camera's feet fogged to black.
        in vec2 vCue;
        uniform float uCueFromZ;
        flat in uint vLight;
        flat in uvec2 vTex;
        flat in uint vMat;
        flat in float vFade;
        // 0088. NeighbourBlend's corner word, world X and Z, and own light dots (WorldVs).
        flat in uvec2 vNb;
        in vec2 vNbXZ;
        noperspective in vec3 vNbDots;
        // 0089. DistanceFade's world X and Z (WorldVs).
        in vec2 vFadeXZ;

        layout(location = 0, index = 0) out vec4 FragColor;
        layout(location = 0, index = 1) out vec4 BlendColor;

        uniform sampler2D uVram;
        uniform sampler2D uDest;
        uniform sampler2D uExtTex;
        uniform sampler2D uRepTex;
        uniform sampler2D uRepClut;
        uniform vec4  uRepRect;
        uniform float uRepClutCount;
        uniform float uRepScroll;
        uniform ivec4 uTexWindow;
        uniform vec4  uBlend;
        uniform vec4  uBlendOpaque = vec4(1.0, 1.0, 1.0, 0.0);
        uniform float uSetMask;
        uniform int   uCheckMask;
        // The occlusion pass's depth-only draw: 1 keeps the texels the console does
        // not blend, 2 (a solid packet) every texel.
        uniform int   uOpaqueDepth;
        uniform float uDepthBias;
        uniform float uDepthSlope;
        // The tolerance probe's ceiling on the tolerance (RetainedScene.ToleranceProbe).
        uniform float uDepthCap = 1.0e9;
        // RetainedScene.DepthCapPixels over H: the main view's ceiling on the slope term,
        // per unit of depth. 0 is none.
        uniform float uDepthCapZ;
        uniform int   uScale;
        uniform vec2  uPosBias;
        uniform float uAniso;
        // 0060. The decoded texture atlas and its switch.
        uniform sampler2D uMip;
        uniform float uMipOn;
        uniform vec3  uLightBk;
        uniform vec3  uLcmR;
        uniform vec3  uLcmG;
        uniform vec3  uLcmB;
        // 0053. Scrolling textures: dest RECT in VRAM and a leftover V shift, so
        // water blends between the integer phases func_8002DC78 uploaded.
        uniform vec4  uFluidRect[8];
        uniform float uFluidOff[8];
        uniform float uFluidN;
        // 0078. Ripples on water (WaterWaves): water's VRAM rects; the camera the frame
        // was drawn with (view = R (world - cam) + T) and the projection, to take a
        // fragment to the world; the field's clock; the push in world units, the
        // longest wavelength and the shading.
        uniform int   uWaveOn;
        uniform int   uWaveN;
        uniform vec4  uWaveRect[8];
        uniform mat3  uWaveR;
        uniform vec3  uWaveCam;
        uniform vec3  uWaveT;
        uniform vec2  uWaveCentre;
        uniform float uWaveH;
        uniform float uWaveTime;
        uniform vec4  uWaveParams;
        // 0068. Drawing a planar reflection: the water's plane in the mirrored
        // camera's view space, kept where dot(xyz, p) + w >= 0, and the projection
        // to take a fragment back to that space with (the GTE's centre, in the
        // target's own pixels, and H).
        uniform int   uClipOn;
        uniform vec4  uClipPlane;
        uniform vec2  uClipCentre;
        uniform float uClipH;
        // 0068, amended. The view's level forward in that space, and the frame's
        // DQA and DQB / 4096. See cueWeight().
        uniform vec3  uClipLevel;
        uniform vec2  uClipDq;
        // 0072. Drawing one plane of a retained planar reflection: kept only where
        // the presented frame's own surface (its surface buffer, the target's size
        // in 1x pixels) lies within uMaskTol of the plane, in that frame's view.
        uniform int   uMaskOn;
        uniform sampler2D uMaskSurface;
        uniform vec4  uMaskPlane;
        uniform float uMaskTol;
        uniform vec2  uMaskCentre;
        uniform float uMaskH;
        uniform vec2  uMaskSize;
        // 0071. Authored lights, published by the port in the GTE's view space
        // (RemasterUniforms): position and radius; colour times intensity and the
        // spot's inner cosine; direction and the outer cosine (-2 for a point, and
        // -2 - id for a point a material gives off, which does not light that id).
        uniform int   uLightN;
        uniform vec4  uLightPos[16];
        uniform vec4  uLightCol[16];
        uniform vec4  uLightDir[16];
        uniform vec2  uLightCentre;
        uniform float uLightH;
        // 0077. A light's shadow: the slot of the depth cubemap drawn from it (-1
        // none), each face holding the nearest surface's distance along its axis
        // over 65536; the frame's view-to-world rotation to look it up with; the face
        // size, the normal offset and the spread of the taps in texels, and the bias.
        uniform int   uLightShadow[16];
        uniform mat3  uShadowToWorld;
        uniform float uShadowSize;
        uniform float uShadowOffset;
        uniform float uShadowBias;
        uniform float uShadowSoft;
        uniform samplerCubeShadow uShadow0;
        uniform samplerCubeShadow uShadow1;
        uniform samplerCubeShadow uShadow2;
        uniform samplerCubeShadow uShadow3;
        // 0071. Emissive materials: row 1 of SurfaceMaterial's table is the light an
        // id gives off, in the same units as a light's colour, and in alpha its flags:
        // 1 added after the texture rather than to the lit colour before it, 2 not
        // fogged. Row 0's alpha is metalness and row 2's red the highlight.
        uniform int   uEmitOn;
        uniform sampler2D uMatTable;
        // 0074. The area's fog colour in 8-bit units, and its curve: the depth cue's
        // weight, 0..1, raised to x and capped at y. uAtmosSkip: this batch's blended
        // texels add or subtract, so fog takes them away rather than to its colour.
        uniform int   uAtmosOn;
        uniform vec3  uAtmosColour;
        uniform vec2  uAtmosShape;
        uniform int   uAtmosSkip;
        // 0083. The view depth past which the port's enhancements give way to the
        // game's own look, over the tile before it; 0 is everywhere enhanced.
        uniform float uPlainZ;
        float gPlain = 0.0;
        // The fog's share of the colour, added past the texture: the lit colour was
        // darkened by the same weight, so the two make a mix towards the fog.
        vec3 gFog8 = vec3(0.0);
        ivec3 fogAdd(bool blended) {
            return blended && uAtmosSkip != 0 ? ivec3(0) : ivec3(floor(gFog8));
        }
        // An additive glow, fogged, in 8-bit colour; added to the modulated texel.
        ivec3 gGlow8 = ivec3(0);
        // The authored lights' highlight, fogged, in 8-bit colour before the
        // metal's tint, and the metalness that tints it.
        vec3  gSpec8 = vec3(0.0);
        float gMetal = 0.0;
        // What is added past the texture, given the surface's own colour.
        ivec3 post(vec3 base) {
            return gGlow8 + ivec3(floor(gSpec8 * mix(vec3(1.0), clamp(base, 0.0, 1.0), gMetal)));
        }

        const int ditherTbl[16] = int[16](
            -4,  0, -3,  1,
             2, -2,  3, -1,
            -3,  1, -4,  0,
             3, -1,  2, -2 );

        int u5(float f) { return int(floor(f * 31.0 + 0.5)); }
        // 0054. Sample VRAM is 1x. Multiplying by uScale fetched the scaled atlas
        // the uploads were blitting into, which is what stalled Flush.
        vec4 fetch(ivec2 c) { return texelFetch(uVram, c & ivec2(1023, 511), 0); }
        int fetch16(ivec2 c) {
            vec4 p = fetch(c);
            return u5(p.r) | (u5(p.g) << 5) | (u5(p.b) << 10) | (int(ceil(p.a)) << 15);
        }
        // One texel, exactly as the unfiltered path reads it: texture window, the
        // 8-bit page wrap, the page fetch, the nibble/byte extract and the CLUT
        // lookup. A paletted texel is an *index*, so none of this can be filtered
        // before the lookup -- the average of index 3 and index 4 is index 3.5,
        // a different colour with no relation to either. The kernel below calls
        // this per tap, which is what puts the filter after the palette.
        vec4 decode(ivec2 raw) {
            ivec2 uv = ((raw & uTexWindow.xy) | uTexWindow.zw) & ivec2(0xff);
            if (texMode == 0) {
                int s = fetch16(ivec2(pageBase.x + (uv.x >> 2), pageBase.y + uv.y));
                int idx = (s >> ((uv.x & 3) << 2)) & 0xf;
                return vRepClut != 0
                    ? texture(uRepClut, vec2((float(idx) + 0.5) / uRepClutCount, 0.5))
                    : fetch(ivec2(clutBase.x + idx, clutBase.y));
            } else if (texMode == 1) {
                int s = fetch16(ivec2(pageBase.x + (uv.x >> 1), pageBase.y + uv.y));
                int idx = (s >> ((uv.x & 1) << 3)) & 0xff;
                return vRepClut != 0
                    ? texture(uRepClut, vec2((float(idx) + 0.5) / uRepClutCount, 0.5))
                    : fetch(ivec2(clutBase.x + idx, clutBase.y));
            }
            return fetch(ivec2(pageBase.x + uv.x, pageBase.y + uv.y));
        }

        // 0053. The integer upload sits at this tick's phase. Shift V by the leftover
        // and blend the two wrap-rows, inside the dest rect the upload itself wraps.
        // uFluidN of 0 returns the centre sample unchanged, so a frame with nothing
        // scrolling is bit-identical to before.
        vec4 decodeFluid(ivec2 raw) {
            vec4 a = decode(raw);
            if (uFluidN < 0.5) return a;
            float div = texMode == 0 ? 4.0 : texMode == 1 ? 2.0 : 1.0;
            float vx = float(pageBase.x) + float(raw.x) / div;
            float vy = float(pageBase.y) + float(raw.y);
            for (int i = 0; i < 8; ++i) {
                if (float(i) >= uFluidN) continue;
                vec4 r = uFluidRect[i];
                if (vx < r.x || vx >= r.x + r.z || vy < r.y || vy >= r.y + r.w) continue;
                float origin = r.y - float(pageBase.y);
                float h = r.w;
                if (h < 1.0) continue;
                float local = float(raw.y) - origin + uFluidOff[i];
                local = local - h * floor(local / h);
                int v0 = int(floor(local));
                float fy = fract(local);
                int y0 = int(origin + 0.5) + v0;
                if (fy < 0.001) return y0 == raw.y ? a : decode(ivec2(raw.x, y0));
                int y1 = int(origin + 0.5) + int(mod(float(v0 + 1), h));
                vec4 c0 = y0 == raw.y ? a : decode(ivec2(raw.x, y0));
                vec4 c1 = decode(ivec2(raw.x, y1));
                return vec4(mix(c0.rgb, c1.rgb, fy), c0.a);
            }
            return a;
        }

        // 0078. The water rect this fragment lies in, in the page's texels: origin and
        // size; a size of 0 is not water. A pushed texel wraps inside it, as the upload
        // itself wraps.
        ivec4 gWave = ivec4(0);

        void waveRectOf(vec2 uv) {
            float div = texMode == 0 ? 4.0 : texMode == 1 ? 2.0 : 1.0;
            float vx = float(pageBase.x) + floor(uv.x) / div;
            float vy = float(pageBase.y) + floor(uv.y);
            for (int i = 0; i < 8; ++i) {
                if (i >= uWaveN) break;
                vec4 r = uWaveRect[i];
                if (vx < r.x || vx >= r.x + r.z || vy < r.y || vy >= r.y + r.w) continue;
                gWave = ivec4(int((r.x - float(pageBase.x)) * div + 0.5), int(r.y - float(pageBase.y) + 0.5),
                              int(r.z * div + 0.5), int(r.w + 0.5));
                return;
            }
        }

        ivec2 waveWrap(ivec2 r) {
            if (gWave.z <= 0 || gWave.w <= 0) return r;
            ivec2 d = r - gWave.xy;
            return gWave.xy + d - gWave.zw * ivec2(floor(vec2(d) / vec2(gWave.zw)));
        }

        // One directional wave's slope. Deep water: a wave's period goes as the root
        // of its length.
        vec2 waveTerm(vec2 p, vec2 dir, float len, float amp) {
            float w = 6.2831853 / (2.4 * sqrt(len / 700.0));
            return dir * (amp * cos(6.2831853 / len * dot(dir, p) - w * uWaveTime));
        }

        // Four waves at unrelated lengths and headings, so the sum does not repeat on
        // the tile grid; the slopes' amplitudes add to 1.
        vec2 waveSlope(vec2 p, float len) {
            return waveTerm(p, vec2(0.80, 0.60), len, 0.40)
                 + waveTerm(p, vec2(-0.39, 0.92), len * 0.61, 0.28)
                 + waveTerm(p, vec2(0.97, -0.26), len * 0.37, 0.20)
                 + waveTerm(p, vec2(-0.70, -0.71), len * 0.23, 0.12);
        }

        // The console's truncation: towards the texel the gradient enters from.
        ivec2 truncUV(vec2 t, bool negU, bool negV) {
            return ivec2(negU ? int(ceil(t.x - 0.0001)) : int(floor(t.x + 0.0001)),
                         negV ? int(ceil(t.y - 0.0001)) : int(floor(t.y + 0.0001)));
        }

        // 0060. One level of the decoded texture, bilinear, at a level-0 texel
        // position inside the rectangle. Held half a level texel in from the edge,
        // so the bilinear taps never leave the block.
        vec4 mipAt(vec2 local, float level, vec2 ext, vec2 block) {
            float sz = exp2(level);
            vec2 lo = vec2(0.5 * sz);
            vec2 p = clamp(local, lo, max(ext - lo, lo));
            return textureLod(uMip, (block + p) / 2048.0, level);
        }

        // n taps across the whole long axis, each trilinear at lod, premultiplied by
        // solidity. Level 0 is the exact texel, so the filter meets the console's
        // point sample where the footprint shrinks to one texel.
        vec4 mipFootprint(vec2 axis, float n, float lod, ivec2 rMin, ivec2 rMax, vec2 block, bool negU, bool negV) {
            vec2 ext = vec2(rMax - rMin + 1);
            float k0 = floor(lod), f = lod - k0;
            vec4 acc = vec4(0.0);
            for (int i = 0; i < 16; ++i) {
                if (float(i) >= n) break;
                vec2 t = vUV + axis * ((float(i) + 0.5) / n - 0.5);
                vec2 local = t - vec2(rMin);
                vec4 a;
                if (k0 < 0.5) {
                    vec4 c = decode(clamp(truncUV(t, negU, negV), rMin, rMax));
                    a = (c.rgb == vec3(0.0) && c.a < 0.5) ? vec4(0.0) : vec4(c.rgb, 1.0);
                } else {
                    a = mipAt(local, k0, ext, block);
                }
                vec4 b = f > 0.0 ? mipAt(local, k0 + 1.0, ext, block) : a;
                acc += mix(a, b, f);
            }
            return acc / n;
        }

        // 0048. The vertex colour, made again at this pixel from what made it: a lit
        // colour, or a light colour and the three light dots, then the depth cue's
        // weight from the raw MAC0 through the game's own clamp and curve. Floor,
        // because the GTE truncates.
        // 0071. What the authored lights add at this fragment, in the game's light
        // units: 1.0 adds the packet's own RGBC once. The view position is rebuilt
        // from the recovered depth as NormalFs rebuilds it, and the normal is that
        // position's plane, so a light is placed and faced exactly where the GTE
        // put the polygon. The derivatives are taken before any per-fragment test.
        // 0077. One compare against slot s: sampler arrays may not be indexed by a
        // loop variable in GLSL 3.30, so the four are named.
        float shadowTap(int s, vec4 c) {
            if (s == 0) return texture(uShadow0, c);
            if (s == 1) return texture(uShadow1, c);
            if (s == 2) return texture(uShadow2, c);
            return texture(uShadow3, c);
        }

        // 0077. One compare at q (view space, from the light): its distance along
        // the cubemap face's axis against the nearest surface's.
        float shadowCmp(int s, vec3 q) {
            vec3 d = uShadowToWorld * q;
            vec3 a = abs(d);
            return shadowTap(s, vec4(d, (max(a.x, max(a.y, a.z)) - uShadowBias) / 65536.0));
        }

        // 0077. How much of a light reaches the point rel (view space, from the light)
        // on a surface facing n, dist from it: moved off the surface by a texel and a
        // half at that distance, then five compares (the hardware's own 2x2 each) at
        // fixed offsets along the surface itself, so a flat receiver never shadows
        // itself however it slopes. The same pattern at every pixel: nothing to weave
        // a grid into the picture.
        float shadowAt(int s, vec3 rel, vec3 n, float dist) {
            float texel = 2.0 * dist / uShadowSize;
            vec3 q = rel + n * (uShadowOffset * texel);
            vec3 t = normalize(cross(n, abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
            vec3 b = cross(n, t);
            float k = uShadowSoft * texel;
            float v = shadowCmp(s, q);
            v += shadowCmp(s, q + t * k);
            v += shadowCmp(s, q - t * k);
            v += shadowCmp(s, q + b * k);
            v += shadowCmp(s, q - b * k);
            return v * 0.2;
        }

        vec3 authored(float spec, float rough, out vec3 hi) {
            hi = vec3(0.0);
            float z = vDepth * 65536.0;
            vec3 p = vec3((gl_FragCoord.xy / float(uScale) - uLightCentre) * (z / uLightH), z);
            vec3 n = cross(dFdx(p), dFdy(p));
            if (vDepth <= 0.0 || vLight == 0u || !(dot(n, n) > 1e-12)) return vec3(0.0);
            n = normalize(n);
            if (dot(n, p) > 0.0) n = -n;
            // Normalised Blinn-Phong: the lobe's size from roughness, squared as
            // the reflections take it, and its energy kept as it widens.
            vec3 eye = normalize(-p);
            float a = max(rough, 0.15);
            a *= a;
            float shin = 2.0 / (a * a) - 2.0;
            float norm = (shin + 8.0) / 25.1327;
            vec3 sum = vec3(0.0);
            for (int i = 0; i < 16; ++i) {
                if (i >= uLightN) break;
                // A point below -2 is a material's own light, which leaves that
                // material as its glow drew it.
                if (uLightDir[i].w < -2.5 && vMat != 0u && int(-uLightDir[i].w - 2.0 + 0.5) == int(vMat)) continue;
                vec3 l = uLightPos[i].xyz - p;
                float r2 = uLightPos[i].w * uLightPos[i].w;
                float d2 = dot(l, l);
                if (d2 >= r2) continue;
                vec3 dir = d2 > 0.0 ? l * inversesqrt(d2) : -n;
                float q = 1.0 - d2 / r2;
                float spot = smoothstep(uLightDir[i].w, uLightCol[i].w, dot(-dir, uLightDir[i].xyz));
                float ndl = max(dot(n, dir), 0.0);
                if (ndl > 0.0 && uLightShadow[i] >= 0) ndl *= shadowAt(uLightShadow[i], -l, n, sqrt(d2));
                sum += uLightCol[i].rgb * (ndl * q * q * spot);
                if (spec > 0.0 && ndl > 0.0)
                    hi += uLightCol[i].rgb * (spec * norm * pow(max(dot(n, normalize(dir + eye)), 0.0), shin) * ndl * q * q * spot);
            }
            return sum;
        }

        // 0068, amended. In a planar capture, the fragment's depth over the depth
        // it is fogged at; 1 everywhere else.
        float gCueScale = 1.0;
        float gCueZ = 0.0;

        // The raw depth cue at the depth a level camera would see this fragment at.
        // MAC0 / 4096 is DQA * H/SZ / 4096 + DQB / 4096, so the part past DQB scales
        // as SZ does. A curve-4 corner holds a weight (EvenFog's blend): taken back
        // through the knee, or at the frame's DQA where the knee left it 0.
        float levelCue(float raw, uint curve) {
            if (curve == 4u) {
                float wv = clamp(raw, 0.0, 4096.0);
                float q = min(uClipH * 65536.0 / max(gCueZ, 1.0), 131071.0);
                raw = wv <= 0.0 ? min(uClipDq.x * q / 4096.0 + uClipDq.y, 0.0)
                    : wv < 2800.0 ? wv : (wv + 5600.0) / 3.0;
            }
            return uClipDq.y + (raw - uClipDq.y) * gCueScale;
        }

        // The depth cue's weight, 0..4096, from the raw MAC0 through the curve.
        //@linearDepthCue
        //@neighbourBlend
        //@distanceFade

        // 0088. A pixel some neighbour of whose half lights or fogs otherwise: its lit
        // colour (when the light blends) and its cue weight from the records around
        // it, at its own depth. Every other pixel is drawn as before.
        bool gNbOn = false, gNbLit = false;
        float gNbW = 0.0;
        vec3 gNbColor = vec3(0.0);
        void nbPrepare() {
            if (vNb.x == 0u || vDepth <= 0.0) return;
            ivec4 rec;
            vec4 k;
            nbAround(int((vNb.x >> 14) & 63u), int(vNb.x & 0x3FFFu), vNbXZ, rec, k);
            float z = vDepth * 65536.0, w = 0.0;
            bool fogOn = (vNb.x & 0x40000000u) != 0u;
            bool fog = fogOn && (vNb.x & 0x20000000u) != 0u && nbFog(rec, k, z, w);
            gNbLit = (vNb.x & 0x10000000u) != 0u && nbLight(rec, k, ivec3(round(vNbDots)), vNb.y, gNbColor);
            if (!fog && !gNbLit) return;
            // The light alone blended: the own record's cue, at this pixel too, on a
            // curve this blends.
            if (!fog && fogOn) {
                w = nbRecordFog(rec.x, z);
                if (w < 0.0) { gNbLit = false; return; }
            }
            gNbOn = true;
            gNbW = w;
        }

        float fogRaw() {
            if (((vLight >> 24) & 7u) == 5u) return linearDepthCue(vDepth * 65536.0, vCue);
            if (uCueFromZ <= 0.0 || vDepth <= 0.0) return vFog;
            float q = min(uCueFromZ / max(vDepth, 1.0 / 65536.0), 131071.0);
            return (vCue.x * q + vCue.y) / 4096.0;
        }

        float cueWeight() {
            uint curve = (vLight >> 24) & 7u;
            bool level = gCueScale < 1.0 && curve != 0u && curve != 5u;
            float fog = gNbOn ? gNbW : fogRaw();
            float raw = level ? levelCue(fog, curve) : fog;
            float ir0 = clamp(raw, 0.0, 4096.0);
            float w = gNbOn ? fog
                 : curve == 1u ? max(ir0 - 800.0, 0.0) * 2.0
                 : curve == 2u ? (ir0 < 2800.0 ? ir0 : 3.0 * ir0 - 5600.0)
                 : curve == 3u ? ir0 * 0.5
                 : curve == 4u ? (level ? (ir0 < 2800.0 ? ir0 : 3.0 * ir0 - 5600.0) : fog)
                 : curve == 5u ? fog
                 : 0.0;
            // 0074. The authored curve over the game's.
            if (uAtmosOn != 0 && uAtmosShape != vec2(1.0) && w > 0.0)
                w = 4096.0 * min(pow(min(w / 4096.0, 1.0), uAtmosShape.x), uAtmosShape.y);
            return w;
        }

        // The knee's keep at the frame's DQA, for a colour with no record.
        float frameKeep(float z) {
            float q = min(uClipH * 65536.0 / max(z, 1.0), 131071.0);
            float ir0 = clamp(uClipDq.x * q / 4096.0 + uClipDq.y, 0.0, 4096.0);
            return clamp(1.0 - (ir0 < 2800.0 ? ir0 : 3.0 * ir0 - 5600.0) / 4096.0, 0.0, 1.0);
        }
        // A colour the GTE fogged at this fragment's depth, fogged at the level one.
        vec3 levelColour(vec3 c) {
            if (gCueScale >= 1.0) return c;
            float k0 = frameKeep(gCueZ);
            return k0 > 0.0 ? c * clamp(frameKeep(gCueZ / gCueScale) / k0, 0.0, 1.0) : c;
        }

        ivec3 shade8(vec3 extra) {
            if (vLight == 0u && !gNbOn) return ivec3(levelColour(vColor.rgb) * 255.0 + 0.5);
            uint mode = vLight >> 24;
            vec3 lit = gNbLit ? gNbColor : vLit;
            if ((mode & 0x80u) != 0u) {
                vec3 rgbc = vec3(uvec3(vLight, vLight >> 8u, vLight >> 16u) & uvec3(255u));
                vec3 a = clamp(vLit, 0.0, 32767.0);
                vec3 ir = clamp(uLightBk + vec3(dot(uLcmR, a), dot(uLcmG, a), dot(uLcmB, a)) / 4096.0, 0.0, 32767.0);
                lit = rgbc * ir / 4096.0;
            }
            // 0071. Before the depth cue, so the game's fog darkens it too.
            if (uLightN > 0 || uEmitOn != 0)
                lit += vec3(uvec3(vLight, vLight >> 8u, vLight >> 16u) & uvec3(255u)) * extra;
            float w = cueWeight();
            if (uAtmosOn != 0) gFog8 = uAtmosColour * clamp(w / 4096.0, 0.0, 1.0);
            vec3 pp = clamp(floor(lit * (1.0 - w / 4096.0)), 0.0, 255.0);
            // 0083. Past the distance, the corner colours the game lit, with the
            // glow a material adds before the cue still added.
            if (gPlain > 0.0) {
                vec3 gc = levelColour(vColor.rgb) * 255.0 + 0.5;
                if (uEmitOn != 0)
                    gc += floor(vec3(uvec3(vLight, vLight >> 8u, vLight >> 16u) & uvec3(255u)) * extra * (1.0 - w / 4096.0));
                pp = mix(pp, clamp(floor(gc), 0.0, 255.0), gPlain);
            }
            return ivec3(pp);
        }

        uniform float uTrueColor;
        vec3 quant5(ivec3 c8) {
            // True color: keep all eight bits, so the smooth shaded gradient is not
            // banded down to 32 levels. Dither is pointless here and skipped — the
            // RGBA8 target has nothing to dither into.
            if (uTrueColor > 0.5) return vec3(clamp(c8, 0, 255)) / 255.0;
            if (vDither != 0) {
                ivec2 vp = ivec2(floor(gl_FragCoord.xy / float(uScale) - uPosBias));
                c8 = clamp(c8 + ditherTbl[(vp.y & 3) * 4 + (vp.x & 3)], 0, 255);
            }
            return vec3(min(c8 >> 3, 31)) / 31.0;
        }

        // 0085. The first-person arm, drawn in painter's order: the far plane where it
        // draws, as its unrecorded packets leave it. 0 is off.
        uniform int uFarPlane;

        // 0051, for a draw of the GPU world renderer's: the depth half of the two
        // passes. Only what decides whether the fragment exists, as the colour pass
        // decides it (the centre texel's hole), not the light or the filter, which
        // a pass with colour masked would otherwise run in full. 0 is off.
        uniform int uDepthOnly;

        void main() {
            // Written on every path so a 3D triangle's recovered SZ is the
            // window depth. Everything that recovered none writes the *far*
            // plane rather than the interpolated clip Z it used to, which is
            // the ambient-occlusion pass's whole mask: 2D, and any triangle the
            // vertex map missed, then say "no surface here" and are left alone
            // instead of being shaded against the geometry standing behind
            // them. The Z-buffer never saw the old value either -- a batch with
            // no recovered depth does not test and does not write -- so this
            // costs it nothing. Assigning this also turns off early-Z, so a
            // punch-through discard cannot occlude whatever is behind the hole.
            // 0051. The tolerance is on the test only; GlCore draws the true depth first.
            float dz = uDepthBias + uDepthSlope * max(abs(dFdx(vDepth)), abs(dFdy(vDepth)));
            // The slope term grows without bound on a face seen edge-on, where half a
            // pixel spans the face's whole depth: a model's silhouette then drew over a
            // surface hundreds of units in front of it. A coplanar partner's offset is a
            // unit or two of world, which at depth z moves the depth by at most a few
            // game pixels' width there (z / H each), so the term stops at that.
            if (uDepthCapZ > 0.0) dz = min(dz, uDepthBias + vDepth * uDepthCapZ);
            dz = min(dz, uDepthCap);
            gl_FragDepth = vDepth > 0.0 ? max(vDepth - dz, 0.0) : 1.0;
            if (uFarPlane != 0) gl_FragDepth = 1.0;
            if (uClipOn != 0 && vDepth > 0.0) {
                float cz = vDepth * 65536.0;
                vec3 cp = vec3((gl_FragCoord.xy / float(uScale) - uClipCentre) * (cz / uClipH), cz);
                if (dot(uClipPlane.xyz, cp) + uClipPlane.w < 0.0) discard;
                // The game's map is culled by a level cone and fogged by view depth,
                // so the cone's far edge is black only to a level camera. The
                // mirrored one looks up by the eye's pitch and saw it lit: fog at the
                // larger of the two depths, which is the game's fog looking level.
                float lz = dot(uClipLevel, cp);
                if (lz > cz) { gCueScale = cz / lz; gCueZ = cz; }
            }
            // 0072, amended. A half fading in or out of a reflection: an ordered
            // dither, so it needs no blending and keeps its depth. 0089: times the
            // main view's DistanceFade, 1 where it is off.
            if (fadeDropped(vFade * distanceFade(vFadeXZ, vDepth * 65536.0))) discard;
            if (uPlainZ > 0.0 && vDepth > 0.0)
                gPlain = smoothstep(uPlainZ - 2048.0, uPlainZ, vDepth * 65536.0);
            if (uMaskOn != 0) {
                vec2 mq = gl_FragCoord.xy / float(uScale);
                float mz = texture(uMaskSurface, mq / uMaskSize).b * 65536.0;
                if (mz <= 1.0) discard;
                vec3 mp = vec3((mq - uMaskCentre) * (mz / uMaskH), mz);
                if (abs(dot(uMaskPlane.xyz, mp) + uMaskPlane.w) > uMaskTol) discard;
            }
            if (uDepthOnly != 0) {
                FragColor = vec4(0.0);
                BlendColor = vec4(0.0);
                if (uCheckMask != 0 && texelFetch(uDest, ivec2(gl_FragCoord.xy), 0).a >= 0.5) discard;
                if (texMode == 4) { if (uOpaqueDepth == 1) discard; return; }
                if (texMode == 5) { if (texture(uExtTex, vUV).a < 0.5 || uOpaqueDepth == 1) discard; return; }
                vec2 ddx = dFdx(vUV), ddy = dFdy(vUV);
                if (texMode == 6) {
                    vec2 fuv = mod(vUV, vec2(uTexWindow.xy) + 1.0) + vec2(uTexWindow.zw);
                    vec2 t = (fuv - uRepRect.xy) / uRepRect.zw;
                    if (uRepScroll >= 0.0) t.y = fract(t.y - uRepScroll / uRepRect.w);
                    float a = textureGrad(uRepTex, t, ddx / uRepRect.zw, ddy / uRepRect.zw).a;
                    if (a < 0.5 || uOpaqueDepth == 1 && a < 0.95) discard;
                    return;
                }
                int du = ddx.x < 0.0 ? int(ceil(vUV.x - 0.0001)) : int(floor(vUV.x + 0.0001));
                int dv = ddy.y < 0.0 ? int(ceil(vUV.y - 0.0001)) : int(floor(vUV.y + 0.0001));
                vec4 dt = decodeFluid(waveWrap(ivec2(du, dv)));
                if (vRepClut != 0 && texMode != 2) {
                    if (dt.a < 0.5 || uOpaqueDepth == 1 && dt.a < 0.95) discard;
                    return;
                }
                if (dt.rgb == vec3(0.0) && dt.a < 0.5) discard;
                if (uOpaqueDepth == 1 && dt.a >= 0.5) discard;
                return;
            }
            // 0071. Not into a planar reflection: its view is the mirrored camera's.
            vec3 extra = vec3(0.0);
            nbPrepare();
            bool mat = uEmitOn != 0 && vMat != 0u && vLight != 0u;
            vec4 m0 = mat ? texelFetch(uMatTable, ivec2(int(vMat), 0), 0) : vec4(0.0);
            float spec = mat ? texelFetch(uMatTable, ivec2(int(vMat), 2), 0).r : 0.0;
            vec3 hi = vec3(0.0);
            if (uLightN > 0 && uClipOn == 0 && gPlain < 1.0) {
                extra = authored(spec, m0.b, hi) * (1.0 - gPlain);
                hi *= 1.0 - gPlain;
            }
            // A surface's own glow needs no position, so it is in a planar
            // reflection too. Additive: RGBC times the glow, fogged as the lit
            // colour is, then added past the texture so a dark texel lights too.
            if (mat) {
                vec3 rgbc = vec3(uvec3(vLight, vLight >> 8u, vLight >> 16u) & uvec3(255u));
                float keep = 1.0 - cueWeight() / 4096.0;
                vec4 glow = texelFetch(uMatTable, ivec2(int(vMat), 1), 0);
                int flags = int(glow.a + 0.5);
                if ((flags & 1) != 0)
                    gGlow8 = ivec3(clamp(floor(rgbc * glow.rgb * ((flags & 2) != 0 ? 1.0 : keep)), 0.0, 255.0));
                else extra += glow.rgb;
                // The highlight is the light's, not the surface's: past the texture.
                gSpec8 = clamp(rgbc * hi * keep, 0.0, 255.0);
                gMetal = m0.a;
            }
            ivec3 c8in = shade8(extra);
            if (uCheckMask != 0 && texelFetch(uDest, ivec2(gl_FragCoord.xy), 0).a >= 0.5) discard;

            if (texMode == 4) {
                if (uOpaqueDepth == 1) discard;
                FragColor = vec4(quant5(c8in + post(vec3(c8in) / 255.0) + fogAdd(true)), uSetMask);
                BlendColor = uBlend;
                return;
            }

            if (texMode == 5) {
                vec4 img = texture(uExtTex, vUV);
                if (img.a < 0.5 || uOpaqueDepth == 1) discard;
                ivec3 e8 = ((ivec3(img.rgb * 255.0 + 0.5) * c8in) >> 7) + post(img.rgb) + fogAdd(true);
                FragColor = vec4(quant5(e8), uSetMask);
                BlendColor = uBlend;
                return;
            }

            // The two screen derivatives of the texture coordinate span the
            // pixel's footprint in texture space. They already decided which way
            // the console's truncation rounds; the kernel below reads the same
            // pair as the shape of the area this pixel actually covers.
            vec2 dUVdx = dFdx(vUV);
            vec2 dUVdy = dFdy(vUV);

            // 0078. Water: the texel is read from where the wave field's slope pushes
            // it, a push in the world taken into texture space through the polygon's
            // own mapping, so it agrees across tiles however each is turned; and the
            // slope lightens or darkens it. Faded out where a pixel spans too much of
            // the shortest wave to show it. uWaveOn is uniform, so the derivatives
            // are taken in uniform control flow.
            vec2 uv = vUV;
            float waveLight = 1.0;
            if (uWaveOn != 0 && texMode <= 2) {
                float z = vDepth * 65536.0;
                vec3 vp = vec3((gl_FragCoord.xy / float(uScale) - uWaveCentre) * (z / uWaveH), z);
                vec3 wp = transpose(uWaveR) * (vp - uWaveT) + uWaveCam;
                vec2 wx = dFdx(wp.xz), wy = dFdy(wp.xz);
                float det = wx.x * wy.y - wx.y * wy.x;
                waveRectOf(vUV);
                if (vDepth > 0.0 && gWave.z > 0 && abs(det) > 1e-6) {
                    float len = uWaveParams.y;
                    float fw = max(length(wx), length(wy));
                    float fade = 1.0 - smoothstep(len * 0.03, len * 0.09, fw);
                    vec2 sl = waveSlope(wp.xz, len) * fade * (1.0 - gPlain);
                    // The inverse of [wx wy], then the UV's own derivatives.
                    vec2 push = sl * uWaveParams.x;
                    vec2 sp = vec2(wy.y * push.x - wy.x * push.y, -wx.y * push.x + wx.x * push.y) / det;
                    uv += dUVdx * sp.x + dUVdy * sp.y;
                    waveLight = max(1.0 + uWaveParams.z * dot(sl, vec2(0.6, 0.8)), 0.05);
                } else gWave = ivec4(0);
            }

            int rawU = dUVdx.x < 0.0 ? int(ceil(uv.x - 0.0001)) : int(floor(uv.x + 0.0001));
            int rawV = dUVdy.y < 0.0 ? int(ceil(uv.y - 0.0001)) : int(floor(uv.y + 0.0001));

            if (texMode == 6) {
                vec2 win = vec2(uTexWindow.xy) + 1.0;
                vec2 fuv = mod(vUV, win) + vec2(uTexWindow.zw);
                vec2 t = (fuv - uRepRect.xy) / uRepRect.zw;
                // 0073. A scrolling texture: row d shows the source's row d - phase.
                if (uRepScroll >= 0.0) t.y = fract(t.y - uRepScroll / uRepRect.w);
                // 0073. A replacement is a real texture, so the GL sampler filters it
                // (mipmaps and anisotropy, set by the Texture filtering slider), with the
                // gradients of the unwrapped UV so a texture window's wrap is not a seam.
                vec4 img = textureGrad(uRepTex, t, dUVdx / uRepRect.zw, dUVdy / uRepRect.zw);
                if (img.a < 0.5) discard;
                ivec3 e8 = ((ivec3(img.rgb * 255.0 + 0.5) * c8in) >> 7) + post(img.rgb);
                float stp = img.a < 0.95 ? 1.0 : 0.0;
                if (uOpaqueDepth == 1 && stp > 0.5) discard;
                FragColor = vec4(quant5(e8 + fogAdd(stp > 0.5)), max(stp, uSetMask));
                BlendColor = stp > 0.5 ? uBlend : uBlendOpaque;
                return;
            }

            vec4 texel = decodeFluid(waveWrap(ivec2(rawU, rawV)));

            // Anisotropic filtering and mipmaps. See "Anisotropic filtering" in
            // docs/RENDERING.md. The centre tap above is the console's texel and
            // decides the silhouette and the semi-transparency bit; the filters only
            // replace its colour, and every tap stays inside the polygon's texture
            // rectangle (0060), since past it is other art read through this CLUT.
            // 0073. A replaced CLUT keeps the anisotropic taps (each decodes through it), not
            // the mip atlas, which was decoded through the game's CLUT.
            vec3 rawTexel = texel.rgb;
            if ((uAniso > 1.5 || (uMipOn > 0.5 && vRepClut == 0)) && gPlain < 1.0
                    && !(texel.rgb == vec3(0.0) && texel.a < 0.5)) {
                bool hasRect = (vTex.y & 0x80000000u) != 0u;
                ivec2 rMin = hasRect ? ivec2(int(vTex.x & 255u), int((vTex.x >> 8) & 255u)) : ivec2(0);
                ivec2 rMax = hasRect ? ivec2(int((vTex.x >> 16) & 255u), int(vTex.x >> 24)) : ivec2(255);
                // 0078. A pushed tap wraps in the water's rect instead.
                if (gWave.z > 0) { rMin = ivec2(-4096); rMax = ivec2(4096); }
                float lx = length(dUVdx), ly = length(dUVdy);
                vec2 axis = lx >= ly ? dUVdx : dUVdy;
                float major = max(lx, ly), minor = min(lx, ly);
                bool done = false;
                // `major > 0.0` is false for the NaN a degenerate triangle hands dFdx.
                if (major > 0.0 && uMipOn > 0.5 && vRepClut == 0 && (vTex.y & 0x40000000u) != 0u) {
                    float n = clamp(ceil(major / max(minor, 1e-4)), 1.0, max(uAniso, 1.0));
                    float maxLod = float((vTex.y >> 16) & 15u);
                    float lod = min(log2(max(major / n, minor)), maxLod);
                    if (lod > 0.0) {
                        vec4 acc = mipFootprint(axis, n, lod, rMin, rMax,
                            vec2(float(vTex.y & 255u), float((vTex.y >> 8) & 255u)) * 8.0,
                            dUVdx.x < 0.0, dUVdy.y < 0.0);
                        if (acc.a > 0.0) texel = vec4(acc.rgb / acc.a, texel.a);
                        done = true;
                    }
                }
                // One texel apart, up to uAniso of them: without a mip level under
                // it a tap is a point sample, so the span is what is capped.
                if (!done && uAniso > 1.5) {
                    int taps = int(min(ceil(major), uAniso));
                    if (taps > 1 && major > 0.0) {
                        vec2 stride = axis / major;
                        vec3 sum = vec3(0.0);
                        float solid = 0.0;
                        for (int i = 0; i < 16; ++i) {
                            if (i >= taps) break;
                            vec2 t = uv + stride * (float(i) + 0.5 - 0.5 * float(taps));
                            vec4 c = decodeFluid(waveWrap(clamp(truncUV(t, dUVdx.x < 0.0, dUVdy.y < 0.0), rMin, rMax)));
                            // A transparent texel is black: weigh it out.
                            float w = (c.rgb == vec3(0.0) && c.a < 0.5) ? 0.0 : 1.0;
                            sum += c.rgb * w;
                            solid += w;
                        }
                        if (solid > 0.0) texel = vec4(sum / solid, texel.a);
                    }
                }
                // 0083. Fading to the console's one texel.
                if (gPlain > 0.0) texel.rgb = mix(texel.rgb, rawTexel, gPlain);
            }

            // 0078. A transparent texel is black, and stays so.
            if (waveLight != 1.0 && !(texel.rgb == vec3(0.0) && texel.a < 0.5))
                texel.rgb = clamp(texel.rgb * waveLight, vec3(1.0 / 255.0), vec3(1.0));

            if (vRepClut != 0 && texMode != 2) {
                if (texel.a < 0.5) discard;
                ivec3 e8 = ((ivec3(texel.rgb * 255.0 + 0.5) * c8in) >> 7) + post(texel.rgb);
                float stp = texel.a < 0.95 ? 1.0 : 0.0;
                if (uOpaqueDepth == 1 && stp > 0.5) discard;
                FragColor = vec4(quant5(e8 + fogAdd(stp > 0.5)), max(stp, uSetMask));
                BlendColor = stp > 0.5 ? uBlend : uBlendOpaque;
                return;
            }

            if (texel.rgb == vec3(0.0) && texel.a < 0.5) discard;
            if (uOpaqueDepth == 1 && texel.a >= 0.5) discard;
            // 248 = 31 << 3: exact for a texel, and keeps a filtered colour's fraction.
            ivec3 t8 = ivec3(texel.rgb * 248.0 + 0.5);
            ivec3 c8 = ((t8 * c8in) >> 7) + post(texel.rgb) + fogAdd(texel.a >= 0.5);
            FragColor = vec4(quant5(c8), max(texel.a, uSetMask));
            BlendColor = texel.a >= 0.5 ? uBlend : uBlendOpaque;
        }
        """.Replace("//@linearDepthCue", LinearDepthCue.Glsl).Replace("//@neighbourBlend", NeighbourBlend.Glsl)
            .Replace("//@distanceFade", DistanceFade.Glsl);
    
    public const string FullscreenVs120 = """
        #version 120
        attribute vec2 aPos;
        varying vec2 vUv;
        void main() {
            vUv = aPos * 0.5 + 0.5;
            gl_Position = vec4(aPos, 0.0, 1.0);
        }
        """;

    public const string PresentFs120 = """
        #version 120
        varying vec2 vUv;
        uniform sampler2D uVram;
        uniform vec2 uOrigin;
        uniform vec2 uSize;
        uniform vec2 uTexSize;
        void main() {
            vec2 t = (uOrigin + vUv * uSize) / uTexSize;
            gl_FragColor = vec4(texture2D(uVram, t).rgb, 1.0);
        }
        """;

    public const string Present24Fs120 = """
        #version 120
        varying vec2 vUv;
        uniform sampler2D uVram;
        uniform vec2 uOrigin;
        uniform vec2 uSize;
        uniform vec2 uVramSize;
        uniform float uScale;

        float u5(float f) { return floor(f * 31.0 + 0.5); }

        float texel16(float lin) {
            float x = mod(lin, 1024.0);
            float y = floor(lin / 1024.0);
            vec2 uv = (vec2(x, y) * uScale + 0.5) / uVramSize;
            vec4 p = texture2D(uVram, uv);
            return u5(p.r) + u5(p.g) * 32.0 + u5(p.b) * 1024.0 + ceil(p.a) * 32768.0;
        }

        float byteAt(float b) {
            float t = texel16(floor(b * 0.5));
            return mod(b, 2.0) < 0.5 ? mod(t, 256.0) : floor(t / 256.0);
        }

        void main() {
            float px = floor(vUv.x * uSize.x);
            float py = floor(vUv.y * uSize.y);
            float ty = uOrigin.y + py;
            float base = (ty * 1024.0 + uOrigin.x) * 2.0 + px * 3.0;
            gl_FragColor = vec4(byteAt(base) / 255.0, byteAt(base + 1.0) / 255.0, byteAt(base + 2.0) / 255.0, 1.0);
        }
        """;

    public const string BlitVs120 = """
        #version 120
        attribute vec2 aPos;
        uniform vec4 uDstRect;
        uniform vec4 uSrcRect;
        varying vec2 vSrc;
        void main() {
            vec2 unit = aPos * 0.5 + 0.5;
            vSrc = uSrcRect.xy + unit * uSrcRect.zw;
            vec2 p = uDstRect.xy + unit * uDstRect.zw;
            gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
        }
        """;

    public const string BlitFs120 = """
        #version 120
        varying vec2 vSrc;
        uniform sampler2D uSrc;
        void main() { gl_FragColor = texture2D(uSrc, vSrc); }
        """;

    public const string PrimVs120 = """
        #version 120
        attribute vec2  inPos;
        attribute vec3  inColorF;
        attribute float inClutF;
        attribute float inTexpageF;
        attribute vec2  inUV;
        attribute float inW;
        attribute float inZ;

        // The depth pre-pass and the colour pass must agree; see the core profile.
        invariant gl_Position;

        varying vec4  vColor;
        varying vec2  vUV;
        varying float vDepth;
        varying vec2  vClutBase;
        varying vec2  vPageBase;
        varying float vTexMode;
        varying float vDither;
        varying float vRepClut;

        uniform vec2 uVertexOffset;
        uniform vec2 uPosBias;
        uniform vec2 uFbInv;

        float bitAt(float v, float bit) { return floor(mod(v / bit, 2.0)); }

        void main() {
            vec2 p = (inPos + uVertexOffset + uPosBias) * uFbInv - 1.0;
            // As in the core profile: a real W makes vUV interpolate perspective
            // correctly. GLSL 120 has no `noperspective`, so on this backend alone
            // vColor is corrected along with it -- visible only as a slightly
            // different Gouraud gradient on a steeply angled textured polygon.
            // Depth is a fragment value, not clip-space Z: these vertices are
            // already projected, and putting SZ into gl_Position.z lets OpenGL
            // clip them against a far plane the GPU never had. No select on inW,
            // as in the core profile.
            gl_Position = vec4(p * inW, 0.0, inW);
            vDepth = inZ > 0.0 ? inZ * (1.0/65536.0) : 0.0;

            float tp = floor(inTexpageF + 0.5);
            float clut = floor(inClutF + 0.5);

            vColor = vec4(inColorF / 255.0, 0.0);
            vDither = bitAt(tp, 1024.0);
            vRepClut = bitAt(tp, 4096.0);
            vUV = inUV;
            vClutBase = vec2(0.0);
            vPageBase = vec2(0.0);

            if (bitAt(tp, 32768.0) > 0.5) {
                vTexMode = 4.0;
            } else if (bitAt(tp, 16384.0) > 0.5) {
                vTexMode = 5.0;
            } else if (bitAt(tp, 8192.0) > 0.5) {
                vTexMode = 6.0;
            } else {
                vTexMode = floor(mod(tp / 128.0, 4.0));
                vPageBase = vec2(mod(tp, 16.0) * 64.0, bitAt(tp, 16.0) * 256.0);
                vClutBase = vec2(mod(clut, 64.0) * 16.0, mod(floor(clut / 64.0), 512.0));
            }
        }
        """;

    //gl 2.1 has no dual source blending =/ has to do by hand
    public const string PrimFs120 = """
        #version 120
        varying vec4  vColor;
        varying vec2  vUV;
        varying float vDepth;
        varying vec2  vClutBase;
        varying vec2  vPageBase;
        varying float vTexMode;
        varying float vDither;
        varying float vRepClut;

        uniform sampler2D uVram;
        uniform sampler2D uDest;
        uniform sampler2D uExtTex;
        uniform sampler2D uRepTex;
        uniform sampler2D uRepClut;
        uniform vec4  uRepRect;
        uniform float uRepClutCount;
        uniform float uRepScroll;
        uniform vec4  uTexWindow;
        uniform float uSetMask;
        uniform float uCheckMask;
        uniform float uScale;
        uniform vec2  uPosBias;
        uniform vec2  uVramSize;
        uniform vec2  uDestSize;
        uniform float uSemiTrans;
        uniform float uBlendMode;
        uniform float uAniso;
        uniform float uDepthBias;
        uniform float uDepthSlope;
        // 0053. Same leftover V shift as the core-profile shader.
        uniform vec4  uFluidRect[8];
        uniform float uFluidOff[8];
        uniform float uFluidN;

        float u5(float f) { return floor(f * 31.0 + 0.5); }

        vec4 fetch(vec2 c) {
            vec2 w = vec2(mod(c.x, 1024.0), mod(c.y, 512.0));
            // 0054. Sample VRAM is 1x; see the core-profile fetch.
            return texture2D(uVram, (w + 0.5) / uVramSize);
        }

        float fetch16(vec2 c) {
            vec4 p = fetch(c);
            return u5(p.r) + u5(p.g) * 32.0 + u5(p.b) * 1024.0 + ceil(p.a) * 32768.0;
        }

        // One texel, the whole per-texel job -- see the core-profile shader for
        // why a paletted texel cannot be filtered before the CLUT lookup.
        vec4 decode(vec2 raw) {
            vec2 win = uTexWindow.xy + 1.0;
            vec2 uv = vec2(mod(raw.x, win.x), mod(raw.y, win.y)) + uTexWindow.zw;
            uv = vec2(mod(uv.x, 256.0), mod(uv.y, 256.0));
            if (vTexMode < 0.5) {
                float s = fetch16(vec2(vPageBase.x + floor(uv.x / 4.0), vPageBase.y + uv.y));
                float lane = mod(uv.x, 4.0);
                float div = lane < 0.5 ? 1.0 : (lane < 1.5 ? 16.0 : (lane < 2.5 ? 256.0 : 4096.0));
                float idx = mod(floor(s / div), 16.0);
                return vRepClut > 0.5
                    ? texture2D(uRepClut, vec2((idx + 0.5) / uRepClutCount, 0.5))
                    : fetch(vec2(vClutBase.x + idx, vClutBase.y));
            } else if (vTexMode < 1.5) {
                float s = fetch16(vec2(vPageBase.x + floor(uv.x / 2.0), vPageBase.y + uv.y));
                float div = mod(uv.x, 2.0) < 0.5 ? 1.0 : 256.0;
                float idx = mod(floor(s / div), 256.0);
                return vRepClut > 0.5
                    ? texture2D(uRepClut, vec2((idx + 0.5) / uRepClutCount, 0.5))
                    : fetch(vec2(vClutBase.x + idx, vClutBase.y));
            }
            return fetch(vec2(vPageBase.x + uv.x, vPageBase.y + uv.y));
        }

        // 0053. Same leftover V blend as the core-profile shader; types differ.
        vec4 decodeFluid(vec2 raw) {
            vec4 a = decode(raw);
            if (uFluidN < 0.5) return a;
            float div = vTexMode < 0.5 ? 4.0 : (vTexMode < 1.5 ? 2.0 : 1.0);
            float vx = vPageBase.x + raw.x / div;
            float vy = vPageBase.y + raw.y;
            for (int i = 0; i < 8; ++i) {
                if (float(i) >= uFluidN) continue;
                vec4 r = uFluidRect[i];
                if (vx < r.x || vx >= r.x + r.z || vy < r.y || vy >= r.y + r.w) continue;
                float origin = r.y - vPageBase.y;
                float h = r.w;
                if (h < 1.0) continue;
                float local = raw.y - origin + uFluidOff[i];
                local = local - h * floor(local / h);
                float y0 = origin + floor(local);
                float fy = fract(local);
                if (fy < 0.001) return abs(y0 - raw.y) < 0.001 ? a : decode(vec2(raw.x, y0));
                float y1 = origin + mod(floor(local) + 1.0, h);
                vec4 c0 = abs(y0 - raw.y) < 0.001 ? a : decode(vec2(raw.x, y0));
                vec4 c1 = decode(vec2(raw.x, y1));
                return vec4(mix(c0.rgb, c1.rgb, fy), c0.a);
            }
            return a;
        }

        uniform float uTrueColor;
        vec3 quant5(vec3 c8) {
            if (uTrueColor > 0.5) return clamp(c8, 0.0, 255.0) / 255.0;
            if (vDither > 0.5) {
                vec2 vp = floor(gl_FragCoord.xy / uScale - uPosBias);
                float col = mod(vp.x, 4.0);
                float row = mod(vp.y, 4.0);
                float d = 0.0;
                if (row < 0.5)      d = col < 0.5 ? -4.0 : (col < 1.5 ?  0.0 : (col < 2.5 ? -3.0 :  1.0));
                else if (row < 1.5) d = col < 0.5 ?  2.0 : (col < 1.5 ? -2.0 : (col < 2.5 ?  3.0 : -1.0));
                else if (row < 2.5) d = col < 0.5 ? -3.0 : (col < 1.5 ?  1.0 : (col < 2.5 ? -4.0 :  0.0));
                else                d = col < 0.5 ?  3.0 : (col < 1.5 ? -1.0 : (col < 2.5 ?  2.0 : -2.0));
                c8 = clamp(c8 + d, 0.0, 255.0);
            }
            return min(floor(c8 / 8.0), 31.0) / 31.0;
        }

        vec3 blendWith(vec3 src, vec3 dst) {
            if (uBlendMode < 0.5) return (dst + src) * 0.5;
            if (uBlendMode < 1.5) return dst + src;
            if (uBlendMode < 2.5) return dst - src;
            return dst + src * 0.25;
        }

        void main() {
            // 0051. The tolerance is on the test only; GlCore draws the true depth first.
            float dz = uDepthBias + uDepthSlope * max(abs(dFdx(vDepth)), abs(dFdy(vDepth)));
            gl_FragDepth = vDepth > 0.0 ? max(vDepth - dz, 0.0) : 1.0;
            vec2 destUv = gl_FragCoord.xy / uDestSize;
            vec4 dstTexel = texture2D(uDest, destUv);
            if (uCheckMask > 0.5 && dstTexel.a >= 0.5) discard;

            vec3 rgb;
            float stp;
            float mask;

            if (vTexMode > 3.5 && vTexMode < 4.5) {
                rgb = vColor.rgb * 255.0;
                stp = 1.0;
                mask = uSetMask;
            } else if (vTexMode > 4.5 && vTexMode < 5.5) {
                vec4 img = texture2D(uExtTex, vUV);
                if (img.a < 0.5) discard;
                rgb = floor(img.rgb * 255.0 + 0.5) * floor(vColor.rgb * 255.0 + 0.5) / 128.0;
                stp = 1.0;
                mask = uSetMask;
            } else {
                vec2 win = uTexWindow.xy + 1.0;
                vec2 fuv = vec2(mod(vUV.x, win.x), mod(vUV.y, win.y)) + uTexWindow.zw;

        
                vec2 dUVdx = dFdx(vUV);
                vec2 dUVdy = dFdy(vUV);
                float rawU = dUVdx.x < 0.0 ? ceil(vUV.x - 0.0001) : floor(vUV.x + 0.0001);
                float rawV = dUVdy.y < 0.0 ? ceil(vUV.y - 0.0001) : floor(vUV.y + 0.0001);

                if (vTexMode > 5.5) {
                    vec2 t = (fuv - uRepRect.xy) / uRepRect.zw;
                    if (uRepScroll >= 0.0) t.y = fract(t.y - uRepScroll / uRepRect.w);
                    vec4 img = texture2D(uRepTex, t);
                    if (img.a < 0.5) discard;
                    rgb = floor(img.rgb * 255.0 + 0.5) * floor(vColor.rgb * 255.0 + 0.5) / 128.0;
                    stp = img.a < 0.95 ? 1.0 : 0.0;
                    mask = max(stp, uSetMask);
                } else {
                    vec4 texel = decodeFluid(vec2(rawU, rawV));

                    // Anisotropic filtering -- see the core-profile shader for what
                    // the footprint is, why the taps are one texel apart rather
                    // than spread over the whole span, why the transparent texels
                    // are weighed out and why the centre tap alone decides the
                    // silhouette. Identical arithmetic; only the types differ.
                    if (uAniso > 1.5 && vRepClut < 0.5
                            && !(texel.r == 0.0 && texel.g == 0.0
                                 && texel.b == 0.0 && texel.a < 0.5)) {
                        vec2 axis = dot(dUVdx, dUVdx) >= dot(dUVdy, dUVdy) ? dUVdx : dUVdy;
                        float len = length(axis);
                        float taps = min(ceil(len), uAniso);
                        if (taps > 1.5 && len > 0.0) {
                            vec2 stride = axis / len;
                            vec3 sum = vec3(0.0);
                            float solid = 0.0;
                            for (int i = 0; i < 16; ++i) {
                                if (float(i) >= taps) break;
                                vec2 t = vUV + stride * (float(i) + 0.5 - 0.5 * taps);
                                vec4 c = decodeFluid(vec2(
                                    dUVdx.x < 0.0 ? ceil(t.x - 0.0001) : floor(t.x + 0.0001),
                                    dUVdy.y < 0.0 ? ceil(t.y - 0.0001) : floor(t.y + 0.0001)));
                                float w = (c.r == 0.0 && c.g == 0.0 && c.b == 0.0 && c.a < 0.5)
                                    ? 0.0 : 1.0;
                                sum += c.rgb * w;
                                solid += w;
                            }
                            if (solid > 0.0) texel = vec4(sum / solid, texel.a);
                        }
                    }

                    if (vRepClut > 0.5 && vTexMode < 1.5) {
                        if (texel.a < 0.5) discard;
                        rgb = floor(texel.rgb * 255.0 + 0.5) * floor(vColor.rgb * 255.0 + 0.5) / 128.0;
                        stp = texel.a < 0.95 ? 1.0 : 0.0;
                    } else {
                        if (texel.r == 0.0 && texel.g == 0.0 && texel.b == 0.0 && texel.a < 0.5) discard;
                        vec3 t8 = floor(texel.rgb * 31.0 + 0.5) * 8.0;
                        rgb = t8 * floor(vColor.rgb * 255.0 + 0.5) / 128.0;
                        stp = texel.a >= 0.5 ? 1.0 : 0.0;
                    }
                    mask = max(stp, uSetMask);
                }
            }

            vec3 outRgb = quant5(floor(rgb));
            if (uSemiTrans * stp > 0.5) outRgb = clamp(blendWith(outRgb, dstTexel.rgb), 0.0, 1.0);

            gl_FragColor = vec4(outRgb, mask);
        }
        """;

    // 0060. The texture atlas's two passes: decode a texture rectangle through its
    // CLUT into its block, and build one level of a block from the level above.
    public const string MipVs = """
        #version 330 core
        layout(location = 0) in vec2  aPos;
        layout(location = 1) in ivec4 aRect;
        layout(location = 2) in ivec4 aSrc;
        layout(location = 3) in ivec4 aInfo;
        flat out ivec4 vRect;
        flat out ivec4 vSrc;
        flat out ivec4 vInfo;
        void main() {
            gl_Position = vec4(aPos, 0.0, 1.0);
            vRect = aRect; vSrc = aSrc; vInfo = aInfo;
        }
        """;

    public const string MipDecodeFs = """
        #version 330 core
        flat in ivec4 vRect;   // u0, v0, w, h
        flat in ivec4 vSrc;    // page x, page y, clut x, clut y
        flat in ivec4 vInfo;   // mode, block x, block y
        uniform sampler2D uVram;
        out vec4 oColor;

        int u5(float f) { return int(floor(f * 31.0 + 0.5)); }
        vec4 fetch(ivec2 c) { return texelFetch(uVram, c & ivec2(1023, 511), 0); }
        int fetch16(ivec2 c) {
            vec4 p = fetch(c);
            return u5(p.r) | (u5(p.g) << 5) | (u5(p.b) << 10) | (int(ceil(p.a)) << 15);
        }

        void main() {
            // Past the rectangle the block repeats its edge.
            ivec2 local = clamp(ivec2(gl_FragCoord.xy) - vInfo.yz, ivec2(0), vRect.zw - 1);
            ivec2 uv = (vRect.xy + local) & ivec2(0xff);
            vec4 c;
            if (vInfo.x == 0) {
                int s = fetch16(ivec2(vSrc.x + (uv.x >> 2), vSrc.y + uv.y));
                c = fetch(ivec2(vSrc.z + ((s >> ((uv.x & 3) << 2)) & 0xf), vSrc.w));
            } else if (vInfo.x == 1) {
                int s = fetch16(ivec2(vSrc.x + (uv.x >> 1), vSrc.y + uv.y));
                c = fetch(ivec2(vSrc.z + ((s >> ((uv.x & 1) << 3)) & 0xff), vSrc.w));
            } else {
                c = fetch(ivec2(vSrc.x + uv.x, vSrc.y + uv.y));
            }
            // Premultiplied by solidity; a transparent texel is already black.
            oColor = (c.rgb == vec3(0.0) && c.a < 0.5) ? vec4(0.0) : vec4(c.rgb, 1.0);
        }
        """;

    // The level above is the texture's base level while this runs, so it is lod 0.
    public const string MipDownFs = """
        #version 330 core
        uniform sampler2D uAtlas;
        out vec4 oColor;
        void main() {
            ivec2 p = ivec2(gl_FragCoord.xy) * 2;
            oColor = 0.25 * (texelFetch(uAtlas, p, 0) + texelFetch(uAtlas, p + ivec2(1, 0), 0)
                           + texelFetch(uAtlas, p + ivec2(0, 1), 0) + texelFetch(uAtlas, p + ivec2(1, 1), 0));
        }
        """;

    static readonly (uint Index, string Name)[] PrimAttribs =
    [
        (0, "inPos"), (1, "inColorF"), (2, "inClutF"), (3, "inTexpageF"), (4, "inUV"), (5, "inW"), (6, "inZ"),
        (7, "inLit"), (8, "inFog"), (9, "inLight"), (10, "inTex"),
    ];

    public static uint BuildPrim(GL gl, string vsSrc, string fsSrc, string name)
        => Build(gl, vsSrc, fsSrc, name, PrimAttribs);

    public static uint BuildFullscreen(GL gl, string vsSrc, string fsSrc, string name)
        => Build(gl, vsSrc, fsSrc, name, [(0, "aPos")]);

    public static uint Build(GL gl, string vsSrc, string fsSrc, string name, out string? error)
    {
        error = null;
        uint vs = CompileStage(gl, ShaderType.VertexShader, vsSrc, name, out string? vsLog);
        uint fs = CompileStage(gl, ShaderType.FragmentShader, fsSrc, name, out string? fsLog);
        if (vs == 0 || fs == 0)
        {
            error = vsLog ?? fsLog;
            if (vs != 0) gl.DeleteShader(vs);
            if (fs != 0) gl.DeleteShader(fs);
            return 0;
        }

        uint prog = gl.CreateProgram();
        gl.AttachShader(prog, vs);
        gl.AttachShader(prog, fs);
        gl.LinkProgram(prog);
        gl.GetProgram(prog, ProgramPropertyARB.LinkStatus, out int ok);
        if (ok == 0)
        {
            error = gl.GetProgramInfoLog(prog);
            gl.DeleteProgram(prog);
            prog = 0;
        }
        gl.DeleteShader(vs);
        gl.DeleteShader(fs);
        return prog;
    }

    static uint CompileStage(GL gl, ShaderType type, string src, string name, out string? log)
    {
        log = null;
        uint sh = gl.CreateShader(type);
        gl.ShaderSource(sh, Ascii(src));
        gl.CompileShader(sh);
        gl.GetShader(sh, ShaderParameterName.CompileStatus, out int ok);
        if (ok == 0)
        {
            log = $"{type}: {gl.GetShaderInfoLog(sh)}";
            gl.DeleteShader(sh);
            return 0;
        }
        return sh;
    }

    public static uint Build(GL gl, string vsSrc, string fsSrc, string name, (uint Index, string Name)[]? attribs = null)
    {
        uint vs = CompileStage(gl, ShaderType.VertexShader, vsSrc, name);
        uint fs = CompileStage(gl, ShaderType.FragmentShader, fsSrc, name);
        if (vs == 0 || fs == 0) return 0;

        uint prog = gl.CreateProgram();
        gl.AttachShader(prog, vs);
        gl.AttachShader(prog, fs);
        if (attribs != null)
            foreach (var (index, attrib) in attribs)
                gl.BindAttribLocation(prog, index, attrib);
        gl.LinkProgram(prog);
        gl.GetProgram(prog, ProgramPropertyARB.LinkStatus, out int ok);
        if (ok == 0)
        {
            Console.WriteLine($"[GlBackend] link failed ({name}): {gl.GetProgramInfoLog(prog)}");
            gl.DeleteProgram(prog);
            prog = 0;
        }
        gl.DeleteShader(vs);
        gl.DeleteShader(fs);
        return prog;
    }

    static string Ascii(string s)
    {
        var a = s.ToCharArray();
        for (int i = 0; i < a.Length; i++) if (a[i] > 0x7F) a[i] = ' ';
        return new string(a);
    }

    static uint CompileStage(GL gl, ShaderType type, string src, string name)
    {
        uint sh = gl.CreateShader(type);
        gl.ShaderSource(sh, Ascii(src));
        gl.CompileShader(sh);
        gl.GetShader(sh, ShaderParameterName.CompileStatus, out int ok);
        if (ok == 0)
        {
            Console.WriteLine($"[GlBackend] compile failed ({name} {type}) {gl.GetShaderInfoLog(sh)}");
            gl.DeleteShader(sh);
            return 0;
        }
        return sh;
    }
}
