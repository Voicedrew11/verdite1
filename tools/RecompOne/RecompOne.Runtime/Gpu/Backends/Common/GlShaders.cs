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
        // How much the occlusion reaches each material: the surface buffer's id and
        // row 2 of SurfaceMaterial's table, whose green is the share taken off.
        uniform sampler2D uSurface;
        uniform sampler2D uMatTable;
        uniform float uAoMatOn;
        out vec4 oColor;
        void main() {
            vec2 t = (uOrigin + vUv * uSize) / uTexSize;
            vec3 c = texture(uVram, t).rgb;
            // The occlusion texture is rendered at exactly this framebuffer's
            // size, so it is indexed by the present's own uv and needs no
            // geometry of its own.
            if (uAoOn > 0.5) {
                float ao = texture(uAo, vUv).r;
                if (uAoMatOn > 0.5) {
                    int m = int(texture(uSurface, t).a + 0.5);
                    if (m > 0 && m < 256) ao = mix(ao, 1.0, texelFetch(uMatTable, ivec2(m, 2), 0).g);
                }
                c *= ao;
            }
            if (uSsrOn > 0.5) {
                vec4 r = texture(uSsr, vUv);
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

        uniform vec2 uPosBias;
        uniform vec2 uFbInv;

        out float vDepth;
        flat out float vM;

        void main() {
            vec2 p = (inPos + uPosBias) * uFbInv - 1.0;
            float w = max(inZ, 1.0);
            gl_Position = vec4(p * w, 0.0, w);
            vDepth = inZ * (1.0/65536.0);
            vM = inM;
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
    public const string NormalFs = """
        #version 330 core
        in float vDepth;
        flat in float vM;
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
            // A blended triangle arrives with 256 added to its material, so
            // opacity is the draw's and not a guess from the id.
            bool opaque = vM < 255.5;
            float id = opaque ? vM : vM - 256.0;
            // 0067. The HUD and anything else 2D: no normal and no depth, only the
            // fact that it covers what is under it.
            if (id > 2.5 && id < 3.5) { oColor = vec4(0.0); oSurface = vec4(0.0, 0.0, 0.0, id); return; }
            if (z <= 0.0) { oColor = vec4(0.0); oSurface = vec4(0.0); return; }
            vec2 s = gl_FragCoord.xy / uScale;
            vec3 p = vec3((s - uCentre) * (z / uProjH), z);
            vec3 n = cross(dFdx(p), dFdy(p));
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
        """;

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
        // SurfaceMaterial's table, by id: row 0 is reflectivity, F0 and roughness.
        uniform sampler2D uMatTable;
        // The game's depth cue, off the GTE: IR0 = (DQA * H/SZ + DQB) / 4096, and
        // which of its curves turns that into a darkening (GteLightMap's numbering).
        uniform float uDqa;
        uniform float uDqb;
        uniform int   uFogCurve;
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
            return clamp(1.0 - w / 4096.0, 0.0, 1.0);
        }

        vec2 tc(vec2 uv) { return (uOrigin + uv * uSize) / uTexSize; }
        float depthAt(vec2 uv) { return texture(uDepth, tc(uv)).r; }
        bool overlayAt(vec2 uv) { return abs(texture(uSurface, tc(uv)).a - OVERLAY) < 0.5; }
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
        // so a dark bronze tints without darkening what it reflects.
        vec3 metalTint(float metal) {
            if (metal <= 0.0) return vec3(1.0);
            vec3 sc = texture(uColor, tc(vUv)).rgb;
            return mix(vec3(1.0), sc / max(max(sc.r, max(sc.g, sc.b)), 1e-3), metal);
        }

        // 0068. The planar reflection at this pixel, when the surface lies on the
        // mirrored plane and something above the water was drawn where it looks:
        // the mirrored image of display row y is row 2*centre - y. The water's own
        // brightness gradient bends the lookup, so the reflection moves with the
        // scrolling texture rather than lying on it like glass. The colour was
        // fogged by the mirrored camera, whose distance to it is the length of the
        // path through the mirror, so it needs no correction here.
        bool planarAt(vec3 p, float rough, out vec3 c) {
            c = vec3(0.0);
            if (uPlanarOn == 0) return false;
            if (abs(dot(uPlanarPlane.xyz, p) + uPlanarPlane.w) > uPlanarTol) return false;
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
            if (texture(uPlanarDepth, tc(muv)).r >= 1.0 && max(pc.r, max(pc.g, pc.b)) <= 0.0) return false;
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
        // own brightness gradient as the old lookup was.
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
            if (pd >= 1.0 && max(pc.r, max(pc.g, pc.b)) <= 0.0) return false;
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
            int m = int(s.a + 0.5);
            // Red is the material here, for the probe's map.
            oInfo = vec4(float(clamp(m, 0, 255)) / 255.0, depthAt(vUv) >= 1.0 ? 1.0 / 255.0 : 0.0, 0.0, 0.0);
            if (m <= 0 || m >= 256) return;
            vec4 mat = texelFetch(uMatTable, ivec2(m, 0), 0);
            float refl = mat.r;
            if (refl <= 0.0) return;
            oInfo.a = 1.0 / 255.0;

            float zs = s.b * FAR;
            if (zs <= 1.0) return;
            // With the Z-buffer on, visibility is the depth test's and not the
            // order's, so a surface redrawn last may still be behind the opaque
            // one the picture shows. The picture is the authority.
            float d = depthAt(vUv);
            if (d > 0.0 && d < 1.0 && d * FAR < zs * 0.99 - 8.0) return;

            vec3 p = viewAt(vUv, zs);
            vec3 n = octDecode(s.rg);
            vec3 v = normalize(p);
            vec3 r = reflect(v, n);
            float cosv = clamp(dot(-v, n), 0.0, 1.0);
            float f0 = mat.g;
            // Squared, so the slider's lower half is the useful range.
            float rough = mat.b * mat.b;
            vec3 tint = metalTint(mat.a);
            // Schlick, running from F0 looking straight down to the material's
            // reflectivity at a grazing angle.
            float w = f0 + (max(refl, f0) - f0) * pow(1.0 - cosv, 5.0);

            vec3 pc;
            bool planarHit = planarAt(p, rough, pc) || retPlanarAt(p, rough, pc);
            if (planarHit && uCompare == 0) {
                oInfo.a += 4.0 / 255.0;
                w = clamp(w, 0.0, 1.0);
                oColor = vec4(pc * tint * w, w);
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
                        vec3 sc = cc * fogKeep(p.z * (length(p) + ct) / length(p));
                        oInfo.b = abs(luma(pc) - luma(sc));
                        oInfo.g = abs(luma(texture(uPlanar, tc(vec2(vUv.x, 2.0 * uCentre.y - vUv.y))).rgb) - luma(sc));
                        oInfo.a += 16.0 / 255.0;
                    }
                    w = clamp(w, 0.0, 1.0);
                    oColor = vec4(pc * tint * w, w);
                    return;
                }
                if (!cubeHit) return;
                float zImage = p.z * (length(p) + ct) / length(p);
                float keep = fogKeep(zImage);
                w *= 1.0 - smoothstep(0.7, 1.0, ct / uMaxDist);
                w = clamp(w, 0.0, 1.0);
                oInfo.a += 1.0 / 255.0;
                oInfo.b = keep;
                oColor = vec4(cc * keep * tint * w, w);
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
                    sc *= clamp(fogKeep(zImage) / max(fogKeep(zHit), 1e-3), 0.0, 1.0);
                    oInfo.b = abs(luma(pc) - luma(sc));
                    oInfo.g = abs(luma(texture(uPlanar, tc(vUv)).rgb) - luma(sc));
                    oInfo.a += 16.0 / 255.0;
                }
                w = clamp(w, 0.0, 1.0);
                oColor = vec4(pc * tint * w, w);
                return;
            }
            if (passed) oInfo.a += 8.0 / 255.0;

            vec3 c;
            // A surface under the HUD is hidden by it: its colour there is the HUD's.
            if (hit && overlayAt(huv)) { oInfo.a += 3.0 / 255.0; return; }
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
                c *= keep;
                oInfo.b = keep;
                oInfo.a += 1.0 / 255.0;
            } else if (bgUv.x >= 0.0 && uSky > 0.0) {
                w *= uSky * edgeFade(bgUv);
                c = texture(uColor, tc(bgUv)).rgb;
                oInfo.a += 2.0 / 255.0;
            } else {
                return;
            }
            w = clamp(w, 0.0, 1.0);
            oColor = vec4(c * tint * w, w);
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
    public const string WorldVs = """
        #version 330 core
        layout(location = 0) in vec3  inWorld;
        layout(location = 1) in vec3  inColorF;
        layout(location = 2) in float inClutF;
        layout(location = 3) in float inTexpageF;
        layout(location = 4) in vec2  inUV;
        layout(location = 5) in vec3  inCue;
        layout(location = 6) in uint  inRect;
        layout(location = 7) in uint  inFlags;

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
        flat out uint vLight;
        flat out uvec2 vTex;
        flat out uint vMat;

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

        float cueKeep(float z) {
            int curve = int(inCue.z + 0.5);
            if (uFogOn == 0 || curve == 0) return 1.0;
            float q = min(uCueH * 65536.0 / max(z, 1.0), 131071.0);
            float ir0 = clamp((inCue.x * q + inCue.y) / 4096.0, 0.0, 4096.0);
            float w = curve == 1 ? max(ir0 - 800.0, 0.0) * 2.0
                    : (ir0 < 2800.0 ? ir0 : 3.0 * ir0 - 5600.0);
            return clamp(1.0 - w / 4096.0, 0.0, 1.0);
        }

        void main() {
            vec3 w = inWorld;
            gl_ClipDistance[0] = (uPlaneY - uPlaneBias) - w.y;
            if (uMirror != 0) w.y = 2.0 * uPlaneY - w.y;
            vec3 v = uR * (w - uCam) + uT;
            float z = v.z;
            gl_Position = vec4((uC * z + uH * v.xy) * 2.0 / uFb - z, z - 2.0 * uNear, z);
            vDepth = z > 0.0 ? z * (1.0 / 65536.0) : 0.0;

            vColor = vec4(inColorF * cueKeep(z), 0.0) / 255.0;
            vLit = vec3(0.0);
            vFog = 0.0;
            vLight = 0u;
            vTex = uvec2(inRect, inFlags & 0x80000000u);
            vMat = inFlags & 255u;
            vDither = 0;
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
        """;

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
        flat out uint vLight;
        flat out uvec2 vTex;
        flat out uint vMat;

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
            vLight = inLight;
            vTex = inTex;
            vMat = inMat;
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

    public const string PrimFs = """
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
        flat in uint vLight;
        flat in uvec2 vTex;
        flat in uint vMat;

        layout(location = 0, index = 0) out vec4 FragColor;
        layout(location = 0, index = 1) out vec4 BlendColor;

        uniform sampler2D uVram;
        uniform sampler2D uDest;
        uniform sampler2D uExtTex;
        uniform sampler2D uRepTex;
        uniform sampler2D uRepClut;
        uniform vec4  uRepRect;
        uniform float uRepClutCount;
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
        // 0068. Drawing a planar reflection: the water's plane in the mirrored
        // camera's view space, kept where dot(xyz, p) + w >= 0, and the projection
        // to take a fragment back to that space with (the GTE's centre, in the
        // target's own pixels, and H).
        uniform int   uClipOn;
        uniform vec4  uClipPlane;
        uniform vec2  uClipCentre;
        uniform float uClipH;
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
        // 0071. Emissive materials: row 1 of SurfaceMaterial's table is the light an
        // id gives off, in the same units as a light's colour, and in alpha its flags:
        // 1 added after the texture rather than to the lit colour before it, 2 not
        // fogged. Row 0's alpha is metalness and row 2's red the highlight.
        uniform int   uEmitOn;
        uniform sampler2D uMatTable;
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
                sum += uLightCol[i].rgb * (ndl * q * q * spot);
                if (spec > 0.0 && ndl > 0.0)
                    hi += uLightCol[i].rgb * (spec * norm * pow(max(dot(n, normalize(dir + eye)), 0.0), shin) * ndl * q * q * spot);
            }
            return sum;
        }

        // The depth cue's weight, 0..4096, from the raw MAC0 through the curve.
        float cueWeight() {
            uint curve = (vLight >> 24) & 7u;
            float ir0 = clamp(vFog, 0.0, 4096.0);
            return curve == 1u ? max(ir0 - 800.0, 0.0) * 2.0
                 : curve == 2u ? (ir0 < 2800.0 ? ir0 : 3.0 * ir0 - 5600.0)
                 : curve == 3u ? ir0 * 0.5
                 : curve == 4u ? vFog
                 : 0.0;
        }

        ivec3 shade8(vec3 extra) {
            if (vLight == 0u) return ivec3(vColor.rgb * 255.0 + 0.5);
            uint mode = vLight >> 24;
            vec3 lit = vLit;
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
            return ivec3(clamp(floor(lit * (1.0 - w / 4096.0)), 0.0, 255.0));
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
            gl_FragDepth = vDepth > 0.0 ? max(vDepth - dz, 0.0) : 1.0;
            if (uClipOn != 0 && vDepth > 0.0) {
                float cz = vDepth * 65536.0;
                vec3 cp = vec3((gl_FragCoord.xy / float(uScale) - uClipCentre) * (cz / uClipH), cz);
                if (dot(uClipPlane.xyz, cp) + uClipPlane.w < 0.0) discard;
            }
            if (uMaskOn != 0) {
                vec2 mq = gl_FragCoord.xy / float(uScale);
                float mz = texture(uMaskSurface, mq / uMaskSize).b * 65536.0;
                if (mz <= 1.0) discard;
                vec3 mp = vec3((mq - uMaskCentre) * (mz / uMaskH), mz);
                if (abs(dot(uMaskPlane.xyz, mp) + uMaskPlane.w) > uMaskTol) discard;
            }
            // 0071. Not into a planar reflection: its view is the mirrored camera's.
            vec3 extra = vec3(0.0);
            bool mat = uEmitOn != 0 && vMat != 0u && vLight != 0u;
            vec4 m0 = mat ? texelFetch(uMatTable, ivec2(int(vMat), 0), 0) : vec4(0.0);
            float spec = mat ? texelFetch(uMatTable, ivec2(int(vMat), 2), 0).r : 0.0;
            vec3 hi = vec3(0.0);
            if (uLightN > 0 && uClipOn == 0) extra = authored(spec, m0.b, hi);
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
                FragColor = vec4(quant5(c8in + post(vec3(c8in) / 255.0)), uSetMask);
                BlendColor = uBlend;
                return;
            }

            if (texMode == 5) {
                vec4 img = texture(uExtTex, vUV);
                if (img.a < 0.5 || uOpaqueDepth == 1) discard;
                ivec3 e8 = ((ivec3(img.rgb * 255.0 + 0.5) * c8in) >> 7) + post(img.rgb);
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
            int rawU = dUVdx.x < 0.0 ? int(ceil(vUV.x - 0.0001)) : int(floor(vUV.x + 0.0001));
            int rawV = dUVdy.y < 0.0 ? int(ceil(vUV.y - 0.0001)) : int(floor(vUV.y + 0.0001));

            if (texMode == 6) {
                vec2 win = vec2(uTexWindow.xy) + 1.0;
                vec2 fuv = mod(vUV, win) + vec2(uTexWindow.zw);
                vec2 t = (fuv - uRepRect.xy) / uRepRect.zw;
                // 0073. A replacement is a real texture, so the GL sampler filters it
                // (mipmaps and anisotropy, set by the Texture filtering slider), with the
                // gradients of the unwrapped UV so a texture window's wrap is not a seam.
                vec4 img = textureGrad(uRepTex, t, dUVdx / uRepRect.zw, dUVdy / uRepRect.zw);
                if (img.a < 0.5) discard;
                ivec3 e8 = ((ivec3(img.rgb * 255.0 + 0.5) * c8in) >> 7) + post(img.rgb);
                float stp = img.a < 0.95 ? 1.0 : 0.0;
                if (uOpaqueDepth == 1 && stp > 0.5) discard;
                FragColor = vec4(quant5(e8), max(stp, uSetMask));
                BlendColor = stp > 0.5 ? uBlend : uBlendOpaque;
                return;
            }

            vec4 texel = decodeFluid(ivec2(rawU, rawV));

            // Anisotropic filtering and mipmaps. See "Anisotropic filtering" in
            // docs/RENDERING.md. The centre tap above is the console's texel and
            // decides the silhouette and the semi-transparency bit; the filters only
            // replace its colour, and every tap stays inside the polygon's texture
            // rectangle (0060), since past it is other art read through this CLUT.
            // 0073. A replaced CLUT keeps the anisotropic taps (each decodes through it), not
            // the mip atlas, which was decoded through the game's CLUT.
            if ((uAniso > 1.5 || (uMipOn > 0.5 && vRepClut == 0))
                    && !(texel.rgb == vec3(0.0) && texel.a < 0.5)) {
                bool hasRect = (vTex.y & 0x80000000u) != 0u;
                ivec2 rMin = hasRect ? ivec2(int(vTex.x & 255u), int((vTex.x >> 8) & 255u)) : ivec2(0);
                ivec2 rMax = hasRect ? ivec2(int((vTex.x >> 16) & 255u), int(vTex.x >> 24)) : ivec2(255);
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
                            vec2 t = vUV + stride * (float(i) + 0.5 - 0.5 * float(taps));
                            vec4 c = decodeFluid(clamp(truncUV(t, dUVdx.x < 0.0, dUVdy.y < 0.0), rMin, rMax));
                            // A transparent texel is black: weigh it out.
                            float w = (c.rgb == vec3(0.0) && c.a < 0.5) ? 0.0 : 1.0;
                            sum += c.rgb * w;
                            solid += w;
                        }
                        if (solid > 0.0) texel = vec4(sum / solid, texel.a);
                    }
                }
            }

            if (vRepClut != 0 && texMode != 2) {
                if (texel.a < 0.5) discard;
                ivec3 e8 = ((ivec3(texel.rgb * 255.0 + 0.5) * c8in) >> 7) + post(texel.rgb);
                float stp = texel.a < 0.95 ? 1.0 : 0.0;
                if (uOpaqueDepth == 1 && stp > 0.5) discard;
                FragColor = vec4(quant5(e8), max(stp, uSetMask));
                BlendColor = stp > 0.5 ? uBlend : uBlendOpaque;
                return;
            }

            if (texel.rgb == vec3(0.0) && texel.a < 0.5) discard;
            if (uOpaqueDepth == 1 && texel.a >= 0.5) discard;
            // 248 = 31 << 3: exact for a texel, and keeps a filtered colour's fraction.
            ivec3 t8 = ivec3(texel.rgb * 248.0 + 0.5);
            ivec3 c8 = ((t8 * c8in) >> 7) + post(texel.rgb);
            FragColor = vec4(quant5(c8), max(texel.a, uSetMask));
            BlendColor = texel.a >= 0.5 ? uBlend : uBlendOpaque;
        }
        """;
    
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
