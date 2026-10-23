#include <kf/renderer/renderer.h>

#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace kf {
namespace {
// Uniform encoding shared with the shader-local names below.
constexpr GLint shader_texture_solid = 0;
constexpr GLint shader_texture_modulated = 1;
constexpr GLint shader_texture_raw = 2;
}

// Triangles are backend-private: original producers submit whole faces, which
// are sorted before this representation is constructed.
struct DrawTriangle {
    std::array<Vertex, 3> vertices;
    u32 material;
    BlendMode blend;
    SurfaceKind surface;
    TextureColorMode color_mode;
    bool dither;
    float uv_bounds[4];   // the whole face's uv rectangle (texture filtering)
    bool casts_shadow;
};
struct DrawList {
    const DrawTriangle *triangles;
    std::size_t count;
    const TextureId *textures;
    std::size_t texture_count;
    FrameStyle style{};
};
static GLuint shader(GLenum type, const char *source, char *error, std::size_t size) {
    const auto object = glCreateShader(type);
    glShaderSource(object, 1, &source, nullptr);
    glCompileShader(object);
    GLint good = 0;
    glGetShaderiv(object, GL_COMPILE_STATUS, &good);
    if (!good) {
        glGetShaderInfoLog(object, static_cast<GLsizei>(size), nullptr, error);
        glDeleteShader(object);
        return 0;
    }
    return object;
}
static const char *post_vertex_source = R"(#version 300 es
out vec2 screen_uv;
void main() {
    // One triangle covering the target: (0,0) (2,0) (0,2) in uv.
    vec2 corner=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));
    screen_uv=corner;
    gl_Position=vec4(corner*2.0-1.0,0.0,1.0);
}
)";

// Screen-space ambient occlusion over the packed camera depth. Reconstructs
// camera-space positions with the game's projection (320x240 logical grid with
// extra widescreen columns), samples a hemisphere around each surface point.
static const char *ao_fragment_source = R"(#version 300 es
precision highp float;
in vec2 screen_uv;
out vec4 result;
uniform sampler2D view_depth;
uniform vec2 depth_size;      // pixels of the depth target
uniform float pixel_scale;
uniform float extra_columns;
uniform float projection_distance;
uniform float radius;
float depth_at(vec2 uv) {
    vec4 stored=texture(view_depth,uv);
    return dot(stored.rgb,vec3(1.0,1.0/255.0,1.0/65025.0))*65536.0;
}
vec3 camera_at(vec2 uv,float z) {
    vec2 pixel=uv*depth_size;
    float x=pixel.x/pixel_scale-extra_columns;
    float y=240.0-pixel.y/pixel_scale;
    return vec3((x-160.0)*z/projection_distance,(y-120.0)*z/projection_distance,z);
}
vec2 uv_of(vec3 p) {
    float x=p.x*projection_distance/p.z+160.0;
    float y=p.y*projection_distance/p.z+120.0;
    return vec2((x+extra_columns)*pixel_scale,(240.0-y)*pixel_scale)/depth_size;
}
void main() {
    float z=depth_at(screen_uv);
    if(z<=1.0) { result=vec4(1.0); return; }
    vec3 p=camera_at(screen_uv,z);
    vec2 texel=1.0/depth_size;
    vec3 px=camera_at(screen_uv+vec2(texel.x,0.0),depth_at(screen_uv+vec2(texel.x,0.0)))-p;
    vec3 py=camera_at(screen_uv+vec2(0.0,texel.y),depth_at(screen_uv+vec2(0.0,texel.y)))-p;
    vec3 n=cross(px,py);
    if(dot(n,n)<1e-6) { result=vec4(1.0); return; }
    n=normalize(n);
    if(dot(n,p)>0.0) n=-n;
    vec3 t=normalize(abs(n.y)<0.9 ? cross(n,vec3(0.0,1.0,0.0)) : cross(n,vec3(1.0,0.0,0.0)));
    vec3 b=cross(n,t);
    // Interleaved gradient noise rotates the pattern per pixel; the blur removes it.
    float noise=fract(52.9829189*fract(dot(gl_FragCoord.xy,vec2(0.06711056,0.00583715))));
    float occlusion=0.0;
    const int samples=12;
    for(int i=0;i<samples;i++) {
        float f=(float(i)+0.5)/float(samples);
        float angle=(float(i)*2.39996+noise*6.28318);
        float r=sqrt(f);
        vec3 dir=t*(cos(angle)*r)+b*(sin(angle)*r)+n*sqrt(max(0.0,1.0-f));
        float scale_i=mix(0.2,1.0,fract(f*7.0+noise));
        vec3 s=p+dir*radius*scale_i;
        if(s.z<16.0) continue;
        vec2 suv=uv_of(s);
        if(suv.x<0.0 || suv.y<0.0 || suv.x>1.0 || suv.y>1.0) continue;
        float scene=depth_at(suv);
        if(scene<=1.0) continue;
        float range=smoothstep(0.0,1.0,radius/max(abs(z-scene),1.0));
        occlusion+=(scene<s.z-radius*0.08 ? 1.0 : 0.0)*range;
    }
    result=vec4(vec3(1.0-occlusion/float(samples)),1.0);
}
)";

// Final picture: supersample averaging or FXAA, then blurred AO.
static const char *composite_fragment_source = R"(#version 300 es
precision highp float;
in vec2 screen_uv;
out vec4 result;
uniform sampler2D color_image;
uniform sampler2D ao_image;
uniform sampler2D depth_image;   // packed camera depth; alpha 1 on world pixels
uniform vec2 ao_texel;
uniform float strength;
uniform int ao_enabled;
uniform vec2 source_size;
uniform int supersample;
uniform int fxaa;
// Flame glow: lights in camera space (xyz, radius) and colour, plus what is
// needed to project them onto the picture.
const int light_capacity=32;
uniform int glow_count;
uniform float glow_strength;
uniform vec4 glow_position[light_capacity];
uniform vec4 glow_color[light_capacity];
uniform float pixel_scale;
uniform float extra_columns;
uniform float projection_distance;
float luma(vec3 c) { return dot(c,vec3(0.299,0.587,0.114)); }
float stored_depth(vec2 uv) {
    vec4 stored=texture(depth_image,uv);
    return stored.a>0.75 ? dot(stored.rgb,vec3(1.0,1.0/255.0,1.0/65025.0))*65536.0 : 0.0;
}
vec3 flame_glow(vec2 uv) {
    vec3 sum=vec3(0.0);
    vec2 aspect=vec2(source_size.x/source_size.y,1.0);
    for(int i=0;i<light_capacity;i++) {
        if(i>=glow_count) break;
        vec3 p=glow_position[i].xyz;
        if(p.z<500.0) continue;   // too close to the camera to glow sensibly
        vec2 logical=vec2(p.x*projection_distance/p.z+160.0,p.y*projection_distance/p.z+120.0);
        vec2 centre=vec2((logical.x+extra_columns)*pixel_scale,(240.0-logical.y)*pixel_scale)/source_size;
        // Size of the halo on screen: ~350 world units around the flame.
        float size=350.0*projection_distance/p.z*pixel_scale/source_size.y;
        vec2 offset=(uv-centre)*aspect;
        float d2=dot(offset,offset)/(size*size);
        if(d2>9.0) continue;
        // Hidden when something nearer covers the flame's centre.
        float covering=stored_depth(centre);
        if(covering>0.0 && covering<p.z-400.0) continue;
        float core=exp(-d2*2.5);
        float bloom=exp(-d2*0.45)*0.25;
        sum+=glow_color[i].rgb*(core+bloom);
    }
    return sum*glow_strength;
}
// FXAA (after Timothy Lottes' public-domain FXAA 3.11 "PC console" variant).
vec3 fxaa_color(vec2 uv,vec2 texel) {
    vec3 nw=texture(color_image,uv+vec2(-1.0,-1.0)*texel).rgb;
    vec3 ne=texture(color_image,uv+vec2(1.0,-1.0)*texel).rgb;
    vec3 sw=texture(color_image,uv+vec2(-1.0,1.0)*texel).rgb;
    vec3 se=texture(color_image,uv+vec2(1.0,1.0)*texel).rgb;
    vec3 m=texture(color_image,uv).rgb;
    float l_nw=luma(nw),l_ne=luma(ne),l_sw=luma(sw),l_se=luma(se),l_m=luma(m);
    float l_min=min(l_m,min(min(l_nw,l_ne),min(l_sw,l_se)));
    float l_max=max(l_m,max(max(l_nw,l_ne),max(l_sw,l_se)));
    vec2 dir=vec2(-((l_nw+l_ne)-(l_sw+l_se)),(l_nw+l_sw)-(l_ne+l_se));
    float reduce=max((l_nw+l_ne+l_sw+l_se)*0.03125,1.0/128.0);
    float scale=1.0/(min(abs(dir.x),abs(dir.y))+reduce);
    dir=clamp(dir*scale,vec2(-8.0),vec2(8.0))*texel;
    vec3 a=0.5*(texture(color_image,uv+dir*(1.0/3.0-0.5)).rgb+texture(color_image,uv+dir*(2.0/3.0-0.5)).rgb);
    vec3 b=a*0.5+0.25*(texture(color_image,uv-dir*0.5).rgb+texture(color_image,uv+dir*0.5).rgb);
    float l_b=luma(b);
    return (l_b<l_min || l_b>l_max) ? a : b;
}
void main() {
    vec3 color;
    if(supersample>1) {
        // Box filter over the N x N source pixels behind this output pixel.
        vec2 centre=screen_uv*source_size;
        float n=float(supersample);
        vec3 sum=vec3(0.0);
        for(int y=0;y<4;y++)
            for(int x=0;x<4;x++) {
                if(x>=supersample || y>=supersample) continue;
                vec2 p=centre+vec2(float(x),float(y))-(n-1.0)*0.5;
                sum+=texelFetch(color_image,ivec2(clamp(p,vec2(0.0),source_size-1.0)),0).rgb;
            }
        color=sum/(n*n);
    } else if(fxaa!=0 && texture(depth_image,screen_uv).a>0.75) {
        color=fxaa_color(screen_uv,1.0/source_size);
    } else {
        color=texture(color_image,screen_uv).rgb;
    }
    // Glow spills over the world and empty darkness, never over the HUD or menus.
    float coverage=texture(depth_image,screen_uv).a;
    if(glow_count>0 && (coverage>0.75 || coverage<0.25))
        color+=flame_glow(screen_uv);
    if(ao_enabled!=0) {
        float ao=0.0;
        for(int y=-1;y<=1;y++)
            for(int x=-1;x<=1;x++)
                ao+=texture(ao_image,screen_uv+vec2(float(x),float(y))*ao_texel*1.5).r;
        ao/=9.0;
        color*=mix(1.0,ao,strength);
    }
    result=vec4(color,1.0);
}
)";

// Shadow pass: world triangles as seen from a light along one cube face, with a
// linear depth (distance along the face axis / far) so the lookup is simple.
static const char *shadow_vertex_source = R"(#version 300 es
layout(location=0) in vec3 camera_position;
uniform vec3 light;
uniform vec3 right; uniform vec3 up; uniform vec3 forward;
uniform float near_exclusion;
const float shadow_far=12000.0;
out float light_distance;
out float face_depth;
void main() {
    vec3 v=camera_position-light;
    vec3 local=vec3(dot(right,v),dot(up,v),dot(forward,v));
    light_distance=length(v);
    // Written per fragment: a varying is interpolated perspective-correctly,
    // whereas a linear depth in gl_Position.z is not (it bent across big faces
    // and shadowed whole triangles next to the light).
    face_depth=local.z/shadow_far;
    // z_ndc = 2*local.z/far - 1 after the divide by w = local.z: linear depth.
    gl_Position=vec4(local.x,local.y,(2.0*local.z/shadow_far-1.0)*local.z,local.z);
}
)";
static const char *shadow_fragment_source = R"(#version 300 es
precision highp float;
in float light_distance;
in float face_depth;
uniform float near_exclusion;
void main() {
    // The light's own fitting (a torch's niche) must not shadow it.
    if(light_distance<near_exclusion) discard;
    gl_FragDepth=clamp(face_depth,0.0,1.0);
}
)";

static GLuint link_program(GLuint vertex, GLuint fragment) {
    if (!vertex || !fragment)
        return 0;
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint good = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &good);
    if (!good) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static void create_post_programs(Renderer *renderer) {
    char log[1024] {};
    const GLuint vertex = shader(GL_VERTEX_SHADER, post_vertex_source, log, sizeof log);
    const GLuint ao = shader(GL_FRAGMENT_SHADER, ao_fragment_source, log, sizeof log);
    const GLuint composite = shader(GL_FRAGMENT_SHADER, composite_fragment_source, log, sizeof log);
    renderer->ao_program = link_program(vertex, ao);
    renderer->composite_program = link_program(vertex, composite);
    if (!renderer->ao_program || !renderer->composite_program)
        std::fprintf(stderr, "Ambient occlusion unavailable: %s\n", log);
    glDeleteShader(vertex);
    glDeleteShader(ao);
    glDeleteShader(composite);
    glGenVertexArrays(1, &renderer->post_vao);
    const GLuint shadow_vertex = shader(GL_VERTEX_SHADER, shadow_vertex_source, log, sizeof log);
    const GLuint shadow_fragment = shader(GL_FRAGMENT_SHADER, shadow_fragment_source, log, sizeof log);
    renderer->shadow_program = link_program(shadow_vertex, shadow_fragment);
    if (!renderer->shadow_program)
        std::fprintf(stderr, "Shadows unavailable: %s\n", log);
    glDeleteShader(shadow_vertex);
    glDeleteShader(shadow_fragment);
    glGenVertexArrays(1, &renderer->shadow_vao);
    glGenBuffers(1, &renderer->shadow_buffer);
    glBindVertexArray(renderer->shadow_vao);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->shadow_buffer);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindVertexArray(0);
}

// Renders the shadow atlas for the first lights from this frame's opaque world
// triangles. Returns how many lights have shadow maps.
static int render_shadow_maps(Renderer *renderer, const DrawList *draws) {
    // Leading lights that cast shadows (shadow_near < 0 marks one that does not).
    int lights = 0;
    while (lights < std::min(renderer->light_count, render_shadow_lights) && renderer->lights[lights].shadow_near >= 0)
        ++lights;
    if (render_shadows <= 0 || !render_dynamic_lights || lights == 0 || !renderer->shadow_program)
        return 0;
    const int face = render_shadow_face_size;
    if (!renderer->shadow_texture) {
        glGenTextures(1, &renderer->shadow_texture);
        glBindTexture(GL_TEXTURE_2D, renderer->shadow_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, face * 6, face * render_shadow_lights, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &renderer->shadow_framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, renderer->shadow_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, renderer->shadow_texture, 0);
        const GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "Shadow atlas unavailable\n");
            glDeleteFramebuffers(1, &renderer->shadow_framebuffer);
            glDeleteTextures(1, &renderer->shadow_texture);
            renderer->shadow_framebuffer = renderer->shadow_texture = 0;
            glDeleteProgram(renderer->shadow_program);
            renderer->shadow_program = 0;
            return 0;
        }
    }
    // Opaque world triangles in camera space.
    static std::vector<float> positions;
    positions.clear();
    for (std::size_t i = 0; i < draws->count; ++i) {
        const auto &triangle = draws->triangles[i];
        if (triangle.blend != BlendMode::opaque || !triangle.casts_shadow)
            continue;
        const auto &v = triangle.vertices;
        if (v[0].cz <= 0 || v[1].cz <= 0 || v[2].cz <= 0)
            continue;
        for (const auto &vertex : v) {
            positions.push_back(vertex.cx);
            positions.push_back(vertex.cy);
            positions.push_back(vertex.cz);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, renderer->shadow_framebuffer);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glViewport(0, 0, face * 6, face * render_shadow_lights);
    glClearDepthf(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glUseProgram(renderer->shadow_program);
    glBindVertexArray(renderer->shadow_vao);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->shadow_buffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(positions.size() * sizeof(float)), positions.data(), GL_STREAM_DRAW);
    static const float right[6][3] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    static const float up[6][3] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    static const float forward[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const GLint light_location = glGetUniformLocation(renderer->shadow_program, "light");
    const GLint right_location = glGetUniformLocation(renderer->shadow_program, "right");
    const GLint up_location = glGetUniformLocation(renderer->shadow_program, "up");
    const GLint forward_location = glGetUniformLocation(renderer->shadow_program, "forward");
    const GLint near_location = glGetUniformLocation(renderer->shadow_program, "near_exclusion");
    const GLsizei vertex_count = static_cast<GLsizei>(positions.size() / 3);
    for (int l = 0; l < lights; ++l) {
        const auto &light = renderer->lights[l];
        glUniform3f(light_location, light.x, light.y, light.z);
        glUniform1f(near_location, light.shadow_near);
        for (int f = 0; f < 6; ++f) {
            glViewport(f * face, l * face, face, face);
            glUniform3fv(right_location, 1, right[f]);
            glUniform3fv(up_location, 1, up[f]);
            glUniform3fv(forward_location, 1, forward[f]);
            glDrawArrays(GL_TRIANGLES, 0, vertex_count);
        }
    }
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(0);
    return lights;
}

bool renderer_init(Renderer *renderer, char *error, std::size_t size) try {
    const auto vertex = shader(GL_VERTEX_SHADER, R"(#version 300 es
layout(location=0) in vec2 position;
layout(location=1) in vec2 texcoord;
layout(location=2) in vec4 color;
layout(location=3) in float depth_w;
layout(location=4) in vec3 camera_position;
out vec3 view_position;
out float world_surface;
uniform int texture_mode;
// x: columns added left of the original 320, y: half the logical width.
uniform vec2 view_columns;
out vec2 uv; out vec4 tint;
const int texture_solid=0;
const float rgb8_max=255.0;
const float modulation_unity=128.0;
void main() {
    vec4 clip=vec4((position.x+view_columns.x)/view_columns.y-1.0,1.0-position.y/120.0,0.0,1.0);
    // Scaling by w leaves the position unchanged but makes the rasterizer
    // interpolate uv and colour perspective-correctly.
    // z = A + B/w in NDC, the standard perspective mapping (near 16, far 65536),
    // which the rasterizer interpolates correctly for the depth test.
    const float near_w=16.0, far_w=65536.0;
    float clip_z=(far_w+near_w)/(far_w-near_w)*depth_w-2.0*far_w*near_w/(far_w-near_w);
    gl_Position=depth_w>0.0 ? vec4(clip.xy*depth_w,clip_z,depth_w) : clip;
    uv=texcoord;
    view_position=camera_position;
    world_surface=camera_position.z>0.0 ? 1.0 : 0.0;
    tint=vec4(floor(color.rgb*(texture_mode==texture_solid ? rgb8_max : modulation_unity)+0.5),color.a);
}
)",
                               error, size);
    if (!vertex)
        return false;
    const auto fragment = shader(GL_FRAGMENT_SHADER, R"(#version 300 es
precision highp float;
precision highp int;
uniform sampler2D image;
uniform sampler2D backdrop;
uniform int texture_mode;
uniform int dither_enabled;
uniform int blend_mode;
uniform int pixel_scale;
uniform int true_color;
uniform int texture_filter;   // 0 nearest (original), 1 bilinear, 2 three-point (N64)
uniform vec4 uv_bounds;       // uv rectangle this face samples: min.xy, max.xy
const int light_capacity=32;
uniform int light_count;
uniform vec4 light_position[light_capacity];  // camera space xyz, radius
uniform vec4 light_color[light_capacity];
in vec3 view_position;
in float world_surface;
uniform int shadow_count;        // lights [0, shadow_count) have shadow maps
uniform float shadow_strength;
uniform highp sampler2D shadow_atlas;   // depth: 6 faces across, one light per row
const float shadow_far=12000.0;
// Cube face bases (right, up, forward); must match the shadow pass.
const vec3 face_right[6]=vec3[6](vec3(0,0,-1),vec3(0,0,1),vec3(1,0,0),vec3(1,0,0),vec3(1,0,0),vec3(-1,0,0));
const vec3 face_up[6]=vec3[6](vec3(0,1,0),vec3(0,1,0),vec3(0,0,-1),vec3(0,0,1),vec3(0,1,0),vec3(0,1,0));
const vec3 face_forward[6]=vec3[6](vec3(1,0,0),vec3(-1,0,0),vec3(0,1,0),vec3(0,-1,0),vec3(0,0,1),vec3(0,0,-1));
// Fraction of light i reaching a point (1 lit, 0 shadowed), 2x2 filtered.
float shadow_visibility(int i,vec3 from_light) {
    vec3 a=abs(from_light);
    int face=a.x>=a.y && a.x>=a.z ? (from_light.x>0.0 ? 0 : 1) : (a.y>=a.z ? (from_light.y>0.0 ? 2 : 3) : (from_light.z>0.0 ? 4 : 5));
    vec3 local=vec3(dot(face_right[face],from_light),dot(face_up[face],from_light),dot(face_forward[face],from_light));
    if(local.z<=1.0) return 1.0;
    vec2 face_uv=clamp(local.xy/local.z*0.5+0.5,vec2(0.002),vec2(0.998));
    ivec2 atlas=textureSize(shadow_atlas,0);
    float face_size=float(atlas.x)/6.0;
    vec2 texel=(vec2(float(face),float(i))+face_uv)*face_size-0.5;
    float depth=local.z/shadow_far;
    float bias=(6.0+local.z*0.0015)/shadow_far;
    ivec2 base=ivec2(floor(texel));
    vec2 f=fract(texel);
    ivec2 low=ivec2(face,i)*int(face_size), high=low+int(face_size)-1;
    float s00=texelFetch(shadow_atlas,clamp(base,low,high),0).r<depth-bias ? 0.0 : 1.0;
    float s10=texelFetch(shadow_atlas,clamp(base+ivec2(1,0),low,high),0).r<depth-bias ? 0.0 : 1.0;
    float s01=texelFetch(shadow_atlas,clamp(base+ivec2(0,1),low,high),0).r<depth-bias ? 0.0 : 1.0;
    float s11=texelFetch(shadow_atlas,clamp(base+ivec2(1,1),low,high),0).r<depth-bias ? 0.0 : 1.0;
    return mix(mix(s00,s10,f.x),mix(s01,s11,f.x),f.y);
}
uniform int light_occlusion;
uniform highp sampler2D occluders;   // R8: 1 = solid map cell
uniform vec2 occluder_size;          // columns, rows
uniform float occluder_tile;
uniform mat3 camera_to_world;
uniform vec3 camera_world;
bool cell_blocks(ivec2 cell) {
    if(cell.x<0 || cell.y<0 || cell.x>=int(occluder_size.x) || cell.y>=int(occluder_size.y)) return true;
    return texelFetch(occluders,cell,0).r>0.5;
}
// Does the segment from a to b (world units) pass through the cell's box shrunk
// by `inset` on every side? Walls and pilasters are thinner than a map cell, so
// grazing a solid cell's corner must not count as hitting it.
bool segment_hits_core(vec2 a,vec2 b,ivec2 cell,float inset) {
    vec2 low=vec2(cell)*occluder_tile+inset, high=vec2(cell+1)*occluder_tile-inset;
    vec2 d=b-a;
    float t0=0.0,t1=1.0;
    for(int axis=0;axis<2;axis++) {
        float da=axis==0 ? d.x : d.y, pa=axis==0 ? a.x : a.y;
        float lo=axis==0 ? low.x : low.y, hi=axis==0 ? high.x : high.y;
        if(abs(da)<1e-4) {
            if(pa<lo || pa>hi) return false;
        } else {
            float ta=(lo-pa)/da, tb=(hi-pa)/da;
            t0=max(t0,min(ta,tb));
            t1=min(t1,max(ta,tb));
        }
    }
    return t0<=t1;
}
// 2D walk through the map grid from the light to the surface point: a solid
// cell strictly between them blocks the light if the ray passes through its
// core. The start cell (a torch can sit in its wall) and the end cell are not tested.
bool light_blocked(vec2 from, vec2 to) {
    vec2 a=from/occluder_tile, b=to/occluder_tile;
    ivec2 cell=ivec2(floor(a)), goal=ivec2(floor(b)), start=cell;
    vec2 delta=b-a;
    ivec2 stride=ivec2(sign(delta));
    vec2 step_t=vec2(delta.x!=0.0 ? abs(1.0/delta.x) : 1e9, delta.y!=0.0 ? abs(1.0/delta.y) : 1e9);
    vec2 next_t=vec2(
        delta.x>0.0 ? (floor(a.x)+1.0-a.x)*step_t.x : (a.x-floor(a.x))*step_t.x,
        delta.y>0.0 ? (floor(a.y)+1.0-a.y)*step_t.y : (a.y-floor(a.y))*step_t.y);
    for(int i=0;i<32;i++) {
        if(cell==goal) return false;
        if(next_t.x<next_t.y) { next_t.x+=step_t.x; cell.x+=stride.x; }
        else { next_t.y+=step_t.y; cell.y+=stride.y; }
        if(cell==goal) return false;
        // Cells next to the light's own cell are the wall a torch is mounted in:
        // they must not block its light (that cut wedges out of nearby ceilings).
        bool beside_light=max(abs(cell.x-start.x),abs(cell.y-start.y))<=1;
        if(!beside_light && cell_blocks(cell) && segment_hits_core(from,to,cell,250.0)) return true;
    }
    return false;
}
// PC extension: point lights (torches, spells) on world surfaces. The face
// normal comes from screen-space derivatives; facing is two-sided because the
// game's winding is not a reliable outward direction for every mesh.
vec3 dynamic_light() {
    if(light_count==0 || world_surface<0.5) return vec3(0.0);
    // Degenerate derivatives (sprite edges, tiny faces) have no usable normal:
    // light those pixels without the facing term instead of producing NaN.
    vec3 cross_product=cross(dFdx(view_position),dFdy(view_position));
    float cross_length=length(cross_product);
    bool has_normal=cross_length>1e-4;
    vec3 normal=has_normal ? cross_product/cross_length : vec3(0.0,0.0,-1.0);
    // The game only draws surfaces facing the camera, so the side toward the
    // camera is the lit side: orient the normal that way. Lights on the far side
    // of a wall then contribute nothing instead of shining through it.
    if(dot(normal,view_position)>0.0) normal=-normal;
    // A surface point lies on a cell boundary; test from just off the surface,
    // on the side the camera sees, so a wall's own cell does not shade it.
    vec2 surface_xz=vec2(0.0);
    if(light_occlusion!=0) surface_xz=(camera_to_world*(view_position+normal*120.0)+camera_world).xz;
    vec3 sum=vec3(0.0);
    for(int i=0;i<light_capacity;i++) {
        if(i>=light_count) break;
        vec3 to_light=light_position[i].xyz-view_position;
        float distance=length(to_light);
        float radius=light_position[i].w;
        if(distance>=radius) continue;
        // Shadow-mapped lights are blocked by real geometry; the coarse map-grid
        // test (whole 2000-unit cells) is only for lights without a shadow map,
        // where it cut false wedges beside niches and pilasters.
        if(light_occlusion!=0 && i>=shadow_count &&
           light_blocked((camera_to_world*light_position[i].xyz+camera_world).xz,surface_xz)) continue;
        float falloff=1.0-distance/radius;
        falloff*=falloff;
        // One-sided: nothing from behind the surface; a slight wrap keeps the wall a
        // torch is mounted on (lit at a grazing angle) from going dark.
        float incidence=dot(normal,to_light/max(distance,1.0));
        float facing=has_normal ? (incidence>0.0 ? incidence*0.75+0.25 : 0.0) : 1.0;
        // Shadows only take away this light's own addition: never darker than the game's lighting.
        float shadow=1.0;
        if(i<shadow_count && facing>0.0)
        {
            // Offset along the surface normal by ~1.5 shadow texels at this distance:
            // enough to avoid self-shadowing speckle without detaching contact shadows.
            float texel_world=distance*2.0/1024.0;
            shadow=mix(1.0,shadow_visibility(i,view_position+normal*max(4.0,texel_world*1.5)-light_position[i].xyz),shadow_strength);
        }
        sum+=light_color[i].rgb*falloff*facing*shadow;
    }
    // Let the game's black fog swallow distant light pools, as it does the walls.
    return sum*(1.0-smoothstep(24000.0,38000.0,view_position.z));
}
in vec2 uv; in vec4 tint;
layout(location=0) out vec4 pixel;
layout(location=1) out vec4 view_depth;   // PC extension: packed camera depth (AO)
vec4 pack_depth(float z) {
    vec3 encoded=fract(clamp(z/65536.0,0.0,0.9999)*vec3(1.0,255.0,65025.0));
    encoded-=encoded.yzz*vec3(1.0/255.0,1.0/255.0,0.0);
    return vec4(encoded,1.0);
}
// BlendMode and the texture-mode uniform use these integer encodings.
const int texture_solid=0, texture_modulated=1, texture_raw=2;
const int blend_opaque=0, blend_average=1, blend_add=2,
    blend_subtract=3, blend_add_quarter=4;
const float rgb8_max=255.0;
const int rgb5_max=31, rgb_expansion_shift=3, rgb_replication_shift=2;
const int modulation_shift=4;
const int dither_period=4, dither_mask=dither_period-1;
const int last_framebuffer_row=239;
// Texture uploads mark STP texels with alpha 128 and opaque texels with 255.
const float stp_alpha_threshold=0.75;
const int dither_offsets[dither_period*dither_period]=int[dither_period*dither_period](
    -4,0,-3,1, 2,-2,3,-1, -3,1,-4,0, 3,-1,2,-2);
// PC extension: smoothed texture sampling for world surfaces. Taps stay inside
// the face's own texel rectangle (texture pages pack unrelated images side by
// side), transparent texels are left out of the average, and transparency and
// the STP flag come from the nearest texel so cut-outs keep their original edge.
vec4 filtered_texture(vec2 coordinate, vec4 nearest) {
    vec2 size=vec2(textureSize(image,0));
    vec2 position=coordinate*size-0.5;
    vec2 f=fract(position);
    ivec2 base=ivec2(floor(position));
    ivec2 low=ivec2(floor(uv_bounds.xy*size+0.001));
    ivec2 high=max(low,ivec2(ceil(uv_bounds.zw*size-0.001))-1);
    vec4 t00=texelFetch(image,clamp(base,low,high),0);
    vec4 t10=texelFetch(image,clamp(base+ivec2(1,0),low,high),0);
    vec4 t01=texelFetch(image,clamp(base+ivec2(0,1),low,high),0);
    vec4 t11=texelFetch(image,clamp(base+ivec2(1,1),low,high),0);
    vec4 w;   // weights for t00, t10, t01, t11
    if(texture_filter==2) {
        w=f.x+f.y<1.0 ? vec4(1.0-f.x-f.y,f.x,f.y,0.0) : vec4(0.0,1.0-f.y,1.0-f.x,f.x+f.y-1.0);
    } else {
        w=vec4((1.0-f.x)*(1.0-f.y),f.x*(1.0-f.y),(1.0-f.x)*f.y,f.x*f.y);
    }
    w*=vec4(t00.a>0.0 ? 1.0 : 0.0,t10.a>0.0 ? 1.0 : 0.0,t01.a>0.0 ? 1.0 : 0.0,t11.a>0.0 ? 1.0 : 0.0);
    float total=w.x+w.y+w.z+w.w;
    if(total<=0.0) return nearest;
    vec3 rgb=(t00.rgb*w.x+t10.rgb*w.y+t01.rgb*w.z+t11.rgb*w.w)/total;
    return vec4(rgb,nearest.a);
}

void main() {
    vec4 sample_color=texture(image,uv);
    if(sample_color.a==0.0) discard;
    if(texture_filter!=0 && texture_mode!=texture_solid && world_surface>0.5)
        sample_color=filtered_texture(uv,sample_color);
    // Alpha: 1 world surface, 0.5 flat 2D (HUD, menus), 0 nothing drawn.
    view_depth=world_surface>0.5 ? pack_depth(view_position.z) : vec4(0.0,0.0,0.0,0.5);
    ivec3 shade=ivec3(clamp(floor(tint.rgb),0.0,rgb8_max));
    ivec3 texel=ivec3(floor(sample_color.rgb*float(rgb5_max)+0.5));
    // PC extension: full-precision colour. Same shading and blend equations,
    // without the 15-bit truncation and the dither that hid its banding.
    vec3 light=dynamic_light();
    vec3 albedo=texture_mode==texture_solid ? vec3(shade) : sample_color.rgb*float(rgb5_max<<rgb_expansion_shift);
    if(true_color!=0) {
        vec3 base=vec3(shade);
        if(texture_mode==texture_modulated) base=sample_color.rgb*float(rgb5_max)*vec3(shade)/16.0;
        if(texture_mode==texture_raw) base=sample_color.rgb*float(rgb5_max<<rgb_expansion_shift);
        base=clamp(base+albedo*light,0.0,rgb8_max);
        if(blend_mode!=blend_opaque && (texture_mode==texture_solid || sample_color.a<stp_alpha_threshold)) {
            vec3 background=texelFetch(backdrop,ivec2(gl_FragCoord.xy),0).rgb*rgb8_max;
            if(blend_mode==blend_average) base=(background+base)*0.5;
            if(blend_mode==blend_add) base=background+base;
            if(blend_mode==blend_subtract) base=background-base;
            if(blend_mode==blend_add_quarter) base=background+base*0.25;
            base=clamp(base,0.0,rgb8_max);
        }
        pixel=vec4(base/rgb8_max,1.0);
        return;
    }
    ivec3 foreground=shade;
    if(texture_mode==texture_modulated) foreground=(texel*shade)>>modulation_shift;
    if(texture_mode==texture_raw) foreground=texel<<rgb_expansion_shift;
    foreground+=ivec3(albedo*light);
    ivec2 destination=ivec2(gl_FragCoord.xy);
    // Dither on the original pixel grid whatever the output scale.
    ivec2 logical=destination/pixel_scale;
    int offset=0;
    if(dither_enabled!=0)
        offset=dither_offsets[((last_framebuffer_row-logical.y)&dither_mask)*dither_period+(logical.x&dither_mask)];
    foreground=clamp((foreground+ivec3(offset))>>rgb_expansion_shift,ivec3(0),ivec3(rgb5_max));
    // A textured face blends only its STP texels. Black texels with STP are
    // visible; only a zero texture word was discarded above.
    if(blend_mode!=blend_opaque && (texture_mode==texture_solid || sample_color.a<stp_alpha_threshold)) {
        ivec3 background=ivec3(floor(texelFetch(backdrop,destination,0).rgb*float(rgb5_max)+0.5));
        if(blend_mode==blend_average) foreground=(background+foreground)>>1;
        if(blend_mode==blend_add) foreground=background+foreground;
        if(blend_mode==blend_subtract) foreground=background-foreground;
        if(blend_mode==blend_add_quarter) foreground=background+(foreground>>2);
        foreground=clamp(foreground,ivec3(0),ivec3(rgb5_max));
    }
    ivec3 expanded=(foreground<<rgb_expansion_shift)|(foreground>>rgb_replication_shift);
    pixel=vec4(vec3(expanded)/rgb8_max,1.0);
}
)",
                                 error, size);
    if (!fragment) {
        glDeleteShader(vertex);
        return false;
    }
    renderer->program = glCreateProgram();
    glAttachShader(renderer->program, vertex);
    glAttachShader(renderer->program, fragment);
    glLinkProgram(renderer->program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint good = 0;
    glGetProgramiv(renderer->program, GL_LINK_STATUS, &good);
    if (!good) {
        glGetProgramInfoLog(renderer->program, static_cast<GLsizei>(size), nullptr, error);
        renderer_release(renderer);
        return false;
    }
    glGenVertexArrays(1, &renderer->vao);
    glBindVertexArray(renderer->vao);
    glGenBuffers(1, &renderer->buffer);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->buffer);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, r)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, w)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void *>(offsetof(Vertex, cx)));
    if (!renderer_configure(renderer, render_width, 1)) {
        std::snprintf(error, size, "Cannot create 320x240 render target.");
        renderer_release(renderer);
        return false;
    }
    const Image solid{1, 1, {color8_max, color8_max, color8_max, color8_max}};
    renderer->white_texture = renderer_upload(&solid);
    if (!renderer->white_texture) {
        std::snprintf(error, size, "Cannot create solid-color material.");
        renderer_release(renderer);
        return false;
    }
    create_post_programs(renderer);
    glUseProgram(renderer->program);
    glUniform1i(glGetUniformLocation(renderer->program, "image"), 0);
    glUniform1i(glGetUniformLocation(renderer->program, "backdrop"), 1);
    return glGetError() == GL_NO_ERROR;
} catch (const std::bad_alloc &) {
    std::snprintf(error, size, "Cannot allocate renderer resources.");
    renderer_release(renderer);
    return false;
}
static GLuint create_target_texture(int width, int height) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
}

bool renderer_configure(Renderer *renderer, int logical_width, int scale) {
    logical_width = std::max(render_width, logical_width + (logical_width & 1));
    scale = std::clamp(scale, 1, 16);
    if (renderer->framebuffer && logical_width == renderer->logical_width && scale == renderer->scale)
        return true;
    const int width = logical_width * scale, height = render_height * scale;
    const GLuint color = create_target_texture(width, height);
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    GLuint depth = 0;
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    const GLuint view_depth = create_target_texture(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, view_depth, 0);
    const GLenum attachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, attachments);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteRenderbuffers(1, &depth);
        glDeleteTextures(1, &color);
        glDeleteTextures(1, &view_depth);
        return false;
    }
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (renderer->framebuffer) {
        // Carry the retained picture over, so a paused menu does not go black.
        // Copy the logical columns both targets share, rescaled.
        const int old_extra = renderer_extra_columns(renderer), new_extra = (logical_width - render_width) / 2;
        const int shared = std::min(old_extra, new_extra);
        const int old_scale = renderer->scale;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, renderer->framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        // Blit into the colour attachment only: with both draw buffers bound the
        // picture also landed in the packed-depth attachment and scrambled AO/FXAA.
        const GLenum colour_only[] = {GL_COLOR_ATTACHMENT0, GL_NONE};
        glDrawBuffers(2, colour_only);
        glBlitFramebuffer((old_extra - shared) * old_scale, 0, (old_extra + render_width + shared) * old_scale,
                          render_height * old_scale, (new_extra - shared) * scale, 0,
                          (new_extra + render_width + shared) * scale, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glDrawBuffers(2, attachments);
        const float no_depth[] = {0, 0, 0, 0};
        glClearBufferfv(GL_COLOR, 1, no_depth);
        glDeleteFramebuffers(1, &renderer->framebuffer);
        glDeleteRenderbuffers(1, &renderer->depth_buffer);
        glDeleteTextures(1, &renderer->color_texture);
        glDeleteTextures(1, &renderer->blend_texture);
        glDeleteTextures(1, &renderer->view_depth_texture);
        glDeleteTextures(1, &renderer->ao_texture);
        glDeleteFramebuffers(1, &renderer->ao_framebuffer);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    renderer->framebuffer = framebuffer;
    renderer->depth_buffer = depth;
    renderer->view_depth_texture = view_depth;
    // AO is computed at half resolution (at least the logical grid) and blurred.
    renderer->ao_texture = create_target_texture(std::max(width / 2, logical_width), std::max(height / 2, render_height));
    glBindTexture(GL_TEXTURE_2D, renderer->ao_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &renderer->ao_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer->ao_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderer->ao_texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    renderer->color_texture = color;
    renderer->blend_texture = create_target_texture(width, height);
    renderer->logical_width = logical_width;
    renderer->scale = scale;
    return true;
}

void renderer_release(Renderer *renderer) {
    texture_store_release(&renderer->textures);
    renderer_delete_texture(renderer->white_texture);
    glDeleteProgram(renderer->program);
    glDeleteBuffers(1, &renderer->buffer);
    glDeleteVertexArrays(1, &renderer->vao);
    glDeleteTextures(1, &renderer->color_texture);
    glDeleteTextures(1, &renderer->blend_texture);
    glDeleteFramebuffers(1, &renderer->framebuffer);
    glDeleteRenderbuffers(1, &renderer->depth_buffer);
    glDeleteTextures(1, &renderer->occluder_texture);
    glDeleteTextures(1, &renderer->view_depth_texture);
    glDeleteTextures(1, &renderer->ao_texture);
    glDeleteFramebuffers(1, &renderer->ao_framebuffer);
    glDeleteProgram(renderer->ao_program);
    glDeleteProgram(renderer->composite_program);
    glDeleteVertexArrays(1, &renderer->post_vao);
    glDeleteTextures(1, &renderer->shadow_texture);
    glDeleteFramebuffers(1, &renderer->shadow_framebuffer);
    glDeleteProgram(renderer->shadow_program);
    glDeleteVertexArrays(1, &renderer->shadow_vao);
    glDeleteBuffers(1, &renderer->shadow_buffer);
    *renderer = {};
}
TextureId renderer_upload(const Image *image) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(image->width),
                 static_cast<GLsizei>(image->height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 image->rgba.data());
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        return 0;
    }
    return texture;
}
void renderer_delete_texture(TextureId texture) {
    glDeleteTextures(1, &texture);
}
static float clear_channel(float value) {
    const auto channel = static_cast<u32>(std::clamp(std::round(value * color8_max), 0.0f, color8_scale)) >> color5_color8_shift;
    return static_cast<float>((channel << color5_color8_shift) | (channel >> color5_replication_shift)) / color8_scale;
}
static void renderer_draw(Renderer *renderer, const DrawList *draws, int width, int height) {
    const int shadowed_lights = render_shadow_maps(renderer, draws);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer->framebuffer);
    const int scale = renderer->scale, extra = renderer_extra_columns(renderer);
    glViewport(0, 0, renderer->logical_width * scale, render_height * scale);
    const auto &style = draws->style;
    // A full-screen clip covers the widened frame; smaller clips (menus, the
    // opening's panels) stay where they were, relative to the original columns.
    const bool full_width = style.clip_x <= 0 && style.clip_x + style.clip_width >= render_width;
    const std::int64_t clip_left = full_width ? -extra : style.clip_x;
    const std::int64_t clip_right = full_width ? render_width + extra : std::int64_t(style.clip_x) + style.clip_width;
    const auto left = std::clamp<std::int64_t>(clip_left, -extra, render_width + extra);
    const auto top = std::clamp<std::int64_t>(style.clip_y, 0, render_height);
    const auto right = std::clamp<std::int64_t>(clip_right, left, render_width + extra);
    const auto bottom = std::clamp<std::int64_t>(std::int64_t(style.clip_y) + style.clip_height, top, render_height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<GLint>((left + extra) * scale), static_cast<GLint>((render_height - bottom) * scale),
              static_cast<GLsizei>((right - left) * scale), static_cast<GLsizei>((bottom - top) * scale));
    glDisable(GL_DITHER);
    glDisable(GL_BLEND);
    if (style.clear == FrameClear::Clear) {
        const float clear_color[] = {clear_channel(style.red), clear_channel(style.green), clear_channel(style.blue), 1};
        const float no_depth[] = {0, 0, 0, 0};
        glClearBufferfv(GL_COLOR, 0, clear_color);
        glClearBufferfv(GL_COLOR, 1, no_depth);
    }
    // Each submission is a complete face list, so depth starts empty. Faces are
    // still drawn in the game's sorted order; the test only removes the
    // see-through cases painter's sorting gets wrong.
    glDepthMask(GL_TRUE);
    glClearDepthf(1.0f);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_DEPTH_TEST);
    bool depth_state = false;
    glDisable(GL_CULL_FACE);
    glUseProgram(renderer->program);
    glBindVertexArray(renderer->vao);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->buffer);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, renderer->blend_texture);
    const auto texture_mode = glGetUniformLocation(renderer->program, "texture_mode");
    const auto dither_enabled = glGetUniformLocation(renderer->program, "dither_enabled");
    const auto blend_mode = glGetUniformLocation(renderer->program, "blend_mode");
    glUniform2f(glGetUniformLocation(renderer->program, "view_columns"), static_cast<float>(extra),
                static_cast<float>(renderer->logical_width) / 2.0f);
    glUniform1i(glGetUniformLocation(renderer->program, "pixel_scale"), scale);
    glUniform1i(glGetUniformLocation(renderer->program, "true_color"), render_true_color ? 1 : 0);
    glUniform1i(glGetUniformLocation(renderer->program, "texture_filter"), std::clamp(render_texture_filter, 0, 2));
    const auto uv_bounds = glGetUniformLocation(renderer->program, "uv_bounds");
    {
        float positions[render_light_capacity * 4], colors[render_light_capacity * 4];
        const int count = render_dynamic_lights ? renderer->light_count : 0;
        for (int i = 0; i < count; ++i) {
            const auto &light = renderer->lights[i];
            positions[i * 4] = light.x; positions[i * 4 + 1] = light.y;
            positions[i * 4 + 2] = light.z; positions[i * 4 + 3] = light.radius;
            colors[i * 4] = light.r; colors[i * 4 + 1] = light.g; colors[i * 4 + 2] = light.b; colors[i * 4 + 3] = 1;
        }
        glUniform1i(glGetUniformLocation(renderer->program, "light_count"), count);
        glUniform1i(glGetUniformLocation(renderer->program, "shadow_count"), count ? shadowed_lights : 0);
        glUniform1f(glGetUniformLocation(renderer->program, "shadow_strength"), std::clamp(render_shadows, 0.0f, 1.0f));
        if (shadowed_lights) {
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, renderer->shadow_texture);
            glUniform1i(glGetUniformLocation(renderer->program, "shadow_atlas"), 3);
            glActiveTexture(GL_TEXTURE0);
        }
        const bool occlusion = count && render_light_occlusion && renderer->occluder_texture;
        glUniform1i(glGetUniformLocation(renderer->program, "light_occlusion"), occlusion ? 1 : 0);
        if (occlusion) {
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, renderer->occluder_texture);
            glUniform1i(glGetUniformLocation(renderer->program, "occluders"), 2);
            glUniform2f(glGetUniformLocation(renderer->program, "occluder_size"),
                static_cast<float>(renderer->occluder_columns), static_cast<float>(renderer->occluder_rows));
            glUniform1f(glGetUniformLocation(renderer->program, "occluder_tile"), renderer->occluder_tile);
            // GL matrices are column-major; transpose flag false with a column-major copy.
            const float *m = renderer->camera_to_world;
            const float columns[9] = {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
            glUniformMatrix3fv(glGetUniformLocation(renderer->program, "camera_to_world"), 1, GL_FALSE, columns);
            glUniform3fv(glGetUniformLocation(renderer->program, "camera_world"), 1, renderer->camera_world);
        }
        if (count) {
            glUniform4fv(glGetUniformLocation(renderer->program, "light_position"), count, positions);
            glUniform4fv(glGetUniformLocation(renderer->program, "light_color"), count, colors);
        }
    }
    for (std::size_t i = 0; i < draws->count; ++i) {
        const auto *draw = &draws->triangles[i];
        if (draw->material >= draws->texture_count || !draws->textures[draw->material])
            continue;
        if (draw->blend != BlendMode::opaque) {
            const auto &a = draw->vertices[0];
            const auto &b = draw->vertices[1];
            const auto &c = draw->vertices[2];
            const auto x0 = static_cast<GLint>(std::clamp(std::floor(std::min({a.x, b.x, c.x})),
                static_cast<float>(left), static_cast<float>(right)));
            const auto x1 = static_cast<GLint>(std::clamp(std::ceil(std::max({a.x, b.x, c.x})),
                static_cast<float>(left), static_cast<float>(right)));
            const auto y0 = static_cast<GLint>(std::clamp(std::floor(std::min({a.y, b.y, c.y})),
                static_cast<float>(top), static_cast<float>(bottom)));
            const auto y1 = static_cast<GLint>(std::clamp(std::ceil(std::max({a.y, b.y, c.y})),
                static_cast<float>(top), static_cast<float>(bottom)));
            if (x1 <= x0 || y1 <= y0)
                continue;
            // Snapshot only this triangle's clipped bounds. Reading the attached
            // color target directly in a shader is undefined on GLES/WebGL.
            glActiveTexture(GL_TEXTURE1);
            const GLint px = static_cast<GLint>((x0 + extra) * scale), py = static_cast<GLint>((render_height - y1) * scale);
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, px, py, px, py, (x1 - x0) * scale, (y1 - y0) * scale);
        }
        // Only world geometry is depth tested. 2D faces (w = 0) and camera-attached
        // models (cz = 0: the held weapon, notifications) keep the original draw
        // order: the weapon sits as deep as nearby walls and relies on its
        // sort-depth trick to be drawn on top.
        const auto world = [](const Vertex &vertex) { return vertex.w > 0 && vertex.cz > 0; };
        const bool use_depth = render_depth_buffer && world(draw->vertices[0]) &&
            world(draw->vertices[1]) && world(draw->vertices[2]);
        if (use_depth != depth_state) {
            if (use_depth)
                glEnable(GL_DEPTH_TEST);
            else
                glDisable(GL_DEPTH_TEST);
            depth_state = use_depth;
        }
        // Translucent faces are tested but never hide what is drawn after them.
        glDepthMask(draw->blend == BlendMode::opaque ? GL_TRUE : GL_FALSE);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, draws->textures[draw->material]);
        if (render_texture_filter != 0) {
            glUniform4fv(uv_bounds, 1, draw->uv_bounds);
        }
        glBufferData(GL_ARRAY_BUFFER, sizeof draw->vertices, draw->vertices.data(), GL_STREAM_DRAW);
        glUniform1i(texture_mode, draw->surface == SurfaceKind::Solid ? shader_texture_solid :
            draw->color_mode == TextureColorMode::Modulated ? shader_texture_modulated : shader_texture_raw);
        glUniform1i(dither_enabled, draw->dither ? 1 : 0);
        glUniform1i(blend_mode, static_cast<GLint>(draw->blend));
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    renderer_present_retained(renderer, width, height);
}

void renderer_present_retained(const Renderer *renderer, int width, int height) {
    if (width <= 0 || height <= 0)
        return;
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    const auto rect = renderer_present_rect(renderer, width, height);
    const int source_width = renderer->logical_width * renderer->scale, source_height = render_height * renderer->scale;
    // Nearest keeps pixels crisp at whole multiples; otherwise smooth the resample.
    const bool whole = rect.width % source_width == 0 && rect.height % source_height == 0;
    const bool ambient_occlusion = render_ambient_occlusion && renderer->ao_program;
    const int supersample = std::clamp(render_supersample, 1, 4);
    const bool glow = render_glow > 0 && render_dynamic_lights && renderer->light_count > 0;
    if (renderer->composite_program && (ambient_occlusion || render_fxaa || supersample > 1 || glow)) {
        GLint ao_size[2] = {std::max(source_width / 2, renderer->logical_width), std::max(source_height / 2, render_height)};
        glBindVertexArray(renderer->post_vao);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        // 1. AO at half resolution.
        glBindFramebuffer(GL_FRAMEBUFFER, renderer->ao_framebuffer);
        glViewport(0, 0, ao_size[0], ao_size[1]);
        if (ambient_occlusion) {
        glUseProgram(renderer->ao_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, renderer->view_depth_texture);
        glUniform1i(glGetUniformLocation(renderer->ao_program, "view_depth"), 0);
        glUniform2f(glGetUniformLocation(renderer->ao_program, "depth_size"), static_cast<float>(source_width), static_cast<float>(source_height));
        glUniform1f(glGetUniformLocation(renderer->ao_program, "pixel_scale"), static_cast<float>(renderer->scale));
        glUniform1f(glGetUniformLocation(renderer->ao_program, "extra_columns"), static_cast<float>(renderer_extra_columns(renderer)));
        glUniform1f(glGetUniformLocation(renderer->ao_program, "projection_distance"), renderer->projection_distance);
        glUniform1f(glGetUniformLocation(renderer->ao_program, "radius"), 700.0f);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        // 2. Composite into the window.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(rect.x, rect.y, rect.width, rect.height);
        glUseProgram(renderer->composite_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, renderer->color_texture);
        const bool smooth_sampling = !whole || render_fxaa;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth_sampling ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth_sampling ? GL_LINEAR : GL_NEAREST);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, renderer->ao_texture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, renderer->view_depth_texture);
        glUniform1i(glGetUniformLocation(renderer->composite_program, "color_image"), 0);
        glUniform1i(glGetUniformLocation(renderer->composite_program, "ao_image"), 1);
        glUniform1i(glGetUniformLocation(renderer->composite_program, "depth_image"), 2);
        glUniform1i(glGetUniformLocation(renderer->composite_program, "ao_enabled"), ambient_occlusion ? 1 : 0);
        glUniform2f(glGetUniformLocation(renderer->composite_program, "source_size"), static_cast<float>(source_width), static_cast<float>(source_height));
        glUniform1i(glGetUniformLocation(renderer->composite_program, "supersample"), supersample);
        glUniform1i(glGetUniformLocation(renderer->composite_program, "fxaa"), render_fxaa ? 1 : 0);
        {
            const GLuint program = renderer->composite_program;
            const int count = glow ? renderer->light_count : 0;
            glUniform1i(glGetUniformLocation(program, "glow_count"), count);
            if (count) {
                float positions[render_light_capacity * 4], colors[render_light_capacity * 4];
                for (int i = 0; i < count; ++i) {
                    const auto &light = renderer->lights[i];
                    positions[i * 4] = light.x; positions[i * 4 + 1] = light.y;
                    positions[i * 4 + 2] = light.z; positions[i * 4 + 3] = light.radius;
                    // Light colours carry strength and flicker; normalise to a glow tint.
                    colors[i * 4] = light.r / 255.0f * 160.0f; colors[i * 4 + 1] = light.g / 255.0f * 160.0f;
                    colors[i * 4 + 2] = light.b / 255.0f * 160.0f; colors[i * 4 + 3] = 1;
                }
                glUniform4fv(glGetUniformLocation(program, "glow_position"), count, positions);
                glUniform4fv(glGetUniformLocation(program, "glow_color"), count, colors);
                glUniform1f(glGetUniformLocation(program, "glow_strength"), render_glow);
                glUniform1f(glGetUniformLocation(program, "pixel_scale"), static_cast<float>(renderer->scale));
                glUniform1f(glGetUniformLocation(program, "extra_columns"), static_cast<float>(renderer_extra_columns(renderer)));
                glUniform1f(glGetUniformLocation(program, "projection_distance"), renderer->projection_distance);
            }
        }
        glUniform2f(glGetUniformLocation(renderer->composite_program, "ao_texel"), 1.0f / ao_size[0], 1.0f / ao_size[1]);
        glUniform1f(glGetUniformLocation(renderer->composite_program, "strength"), std::clamp(render_ao_strength, 0.0f, 2.0f));
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, renderer->color_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glViewport(0, 0, width, height);
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, renderer->framebuffer);
    glBlitFramebuffer(0, 0, source_width, source_height, rect.x, rect.y, rect.x + rect.width, rect.y + rect.height,
                      GL_COLOR_BUFFER_BIT, whole ? GL_NEAREST : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void renderer_set_occluders(Renderer *renderer, const u8 *solid, int columns, int rows, float tile_size) {
    static std::vector<u8> uploaded;
    const std::size_t size = static_cast<std::size_t>(columns) * rows;
    renderer->occluder_tile = tile_size;
    if (renderer->occluder_texture && columns == renderer->occluder_columns && rows == renderer->occluder_rows &&
        uploaded.size() == size && std::equal(uploaded.begin(), uploaded.end(), solid))
        return;
    uploaded.assign(solid, solid + size);
    std::vector<u8> texels(size);
    for (std::size_t i = 0; i < size; ++i)
        texels[i] = solid[i] ? 255 : 0;
    if (!renderer->occluder_texture)
        glGenTextures(1, &renderer->occluder_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, renderer->occluder_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, columns, rows, 0, GL_RED, GL_UNSIGNED_BYTE, texels.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glActiveTexture(GL_TEXTURE0);
    renderer->occluder_columns = columns;
    renderer->occluder_rows = rows;
}

void renderer_set_projection_distance(Renderer *renderer, float distance) {
    renderer->projection_distance = distance > 0 ? distance : 200.0f;
}

void renderer_set_camera(Renderer *renderer, const float rotation[9], const float position[3]) {
    std::copy_n(rotation, 9, renderer->camera_to_world);
    std::copy_n(position, 3, renderer->camera_world);
}

void renderer_set_lights(Renderer *renderer, const RenderLight *lights, int count) {
    renderer->light_count = std::clamp(count, 0, render_light_capacity);
    std::copy_n(lights, renderer->light_count, renderer->lights);
}

PresentRect renderer_present_rect(const Renderer *renderer, int width, int height) {
    // Letterbox to the target's aspect (4:3 stretched pixels, like a TV).
    const double aspect = static_cast<double>(renderer->logical_width) / render_width * 4.0 / 3.0;
    int target_width = width, target_height = static_cast<int>(width / aspect + 0.5);
    if (target_height > height) {
        target_height = height;
        target_width = static_cast<int>(height * aspect + 0.5);
    }
    return {(width - target_width) / 2, (height - target_height) / 2, target_width, target_height};
}

struct FaceOrder {
    std::size_t index;
    s32 depth;
};
static bool compare_faces(const FaceOrder &a, const FaceOrder &b) {
    if (a.depth != b.depth)
        return a.depth > b.depth;
    // Original equal-depth insertion was LIFO. Order whole faces before splitting
    // quads, so another face can never be interleaved between their triangles.
    return a.index > b.index;
}

static unsigned append_face_triangle(DrawTriangle *triangles, const DrawFace &face,
    unsigned a, unsigned b, unsigned c, u32 material, BlendMode blend) {
    const auto &va = face.vertices[a];
    const auto &vb = face.vertices[b];
    const auto &vc = face.vertices[c];
    const float width = std::max({va.x, vb.x, vc.x}) - std::min({va.x, vb.x, vc.x});
    const float height = std::max({va.y, vb.y, vc.y}) - std::min({va.y, vb.y, vc.y});
    // Preserve the original rasterizer's per-triangle size rejection before
    // viewport clipping. Saturated behind-camera faces can otherwise fill the
    // view. Check each quad half independently without changing face order.
    // World faces are exempt: the host clips them at the near plane and
    // re-projects them, so a large one is a near floor or wall, not garbage.
    constexpr float maximum_triangle_width = 1023;
    constexpr float maximum_triangle_height = 511;
    const bool clipped_world = va.cz >= render_near_depth && vb.cz >= render_near_depth && vc.cz >= render_near_depth;
    if (!clipped_world && (width > maximum_triangle_width || height > maximum_triangle_height))
        return 0;
    const bool dither = face.material.kind == SurfaceKind::Texture
        ? face.material.color_mode == TextureColorMode::Modulated : face.shading == FaceShading::Gouraud;
    triangles[0] = {{va, vb, vc}, material, blend, face.material.kind, face.material.color_mode, dither, {1, 1, 0, 0},
                    face.casts_shadow};
    // Clamp filtering to the whole face, so both halves of a quad agree along the split.
    for (unsigned i = 0; i < static_cast<unsigned>(face.shape); ++i) {
        triangles[0].uv_bounds[0] = std::min(triangles[0].uv_bounds[0], face.vertices[i].u);
        triangles[0].uv_bounds[1] = std::min(triangles[0].uv_bounds[1], face.vertices[i].v);
        triangles[0].uv_bounds[2] = std::max(triangles[0].uv_bounds[2], face.vertices[i].u);
        triangles[0].uv_bounds[3] = std::max(triangles[0].uv_bounds[3], face.vertices[i].v);
    }
    return 1;
}

// KF_DUMP_FACES=<n>: print the textured faces of the n-th submitted frame (debugging).
static void dump_faces(const FaceList *faces) {
    static const long target = std::getenv("KF_DUMP_FACES") ? std::atol(std::getenv("KF_DUMP_FACES")) : -1;
    static long submitted;
    if (target < 0 || submitted++ != target)
        return;
    for (std::size_t i = 0; i < faces->count; ++i) {
        const auto &face = faces->faces[i];
        if (face.material.kind != SurfaceKind::Texture)
            continue;
        float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f, min_u = 1e9f, max_u = -1e9f, min_v = 1e9f, max_v = -1e9f;
        for (unsigned v = 0; v < static_cast<unsigned>(face.shape); ++v) {
            const auto &vertex = face.vertices[v];
            min_x = std::min(min_x, vertex.x); max_x = std::max(max_x, vertex.x);
            min_y = std::min(min_y, vertex.y); max_y = std::max(max_y, vertex.y);
            min_u = std::min(min_u, vertex.u); max_u = std::max(max_u, vertex.u);
            min_v = std::min(min_v, vertex.v); max_v = std::max(max_v, vertex.v);
        }
        const auto &source = face.material.source;
        std::printf("[faces] %zu page %u,%u clut %u,%u fmt %d uv %.0f-%.0f,%.0f-%.0f screen %.0f-%.0f,%.0f-%.0f depth %d blend %d cz %.0f\n",
            i, source.x, source.y, source.palette_x, source.palette_y, static_cast<int>(source.format),
            min_u * 256, max_u * 256, min_v * 256, max_v * 256, min_x, max_x, min_y, max_y, face.depth,
            face.transparency == FaceTransparency::Blend ? static_cast<int>(face.material.blend) : -1, face.vertices[0].cz);
    }
}

bool renderer_draw_faces(Renderer *renderer, const FaceList *faces, int width, int height) try {
    dump_faces(faces);
    const auto count = faces->count;
    if ((count && !faces->faces) || count > std::numeric_limits<u32>::max() ||
        count > std::numeric_limits<std::size_t>::max() / 2 / sizeof(DrawTriangle))
        return false;
    std::vector<FaceOrder> order;
    std::vector<DrawTriangle> triangles;
    std::vector<TextureId> textures;
    if (count > order.max_size() || count > triangles.max_size() / 2 || count > textures.max_size())
        return false;
    order.resize(count);
    triangles.resize(count * 2);
    textures.resize(count);
    bool good = true;
    std::size_t triangle_count = 0;
    // The frame starts with an explicit blend policy; previous-frame texture
    // selection must not change a leading untextured translucent face.
    auto preceding_blend = faces->style.initial_blend;
    if (good && count) {
        for (std::size_t i = 0; i < count; ++i)
            order[i] = {i, faces->faces[i].depth};
        std::sort(order.begin(), order.end(), compare_faces);
        for (std::size_t i = 0; i < count; ++i) {
            const auto &face = faces->faces[order[i].index];
            if (face.shape != FaceShape::Triangle && face.shape != FaceShape::Quad) {
                good = false;
                break;
            }
            const bool textured = face.material.kind == SurfaceKind::Texture;
            if (textured)
                preceding_blend = face.material.blend;
            textures[i] = textured ? texture_store_resolve(&renderer->textures, face.material.source)
                                   : renderer->white_texture;
            if (!textures[i]) {
                good = false;
                break;
            }
            const auto blend = face.transparency == FaceTransparency::Blend ? preceding_blend : BlendMode::opaque;
            // Sort original whole faces first. All output from each face
            // stays adjacent, preserving equal-depth LIFO and blending order.
            triangle_count += append_face_triangle(triangles.data() + triangle_count, face,
                0, 1, 2, static_cast<u32>(i), blend);
            if (face.shape == FaceShape::Quad)
                triangle_count += append_face_triangle(triangles.data() + triangle_count, face,
                    1, 3, 2, static_cast<u32>(i), blend);
        }
    }
    if (good) {
        const DrawList draws{triangles.data(), triangle_count, textures.data(), count, faces->style};
        renderer_draw(renderer, &draws, width, height);
    }
    return good;
} catch (const std::bad_alloc &) {
    return false;
}
} // namespace kf
