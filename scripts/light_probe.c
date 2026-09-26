// light_probe -- run the port's real prim fragment shader over strips whose
// per-pixel lighting varyings sweep known values, headless, and compare every
// pixel with the formula (0048).
//
//     gcc -O0 -o /tmp/light_probe scripts/light_probe.c -lEGL -lGL -lm
//     /tmp/light_probe PrimFs.frag
//
// Extract the shader the way scripts/shader_probe.c's header says. The strips are
// untextured and true colour, so the pixel read back is the shader's 8-bit colour
// with nothing downstream of it. Each line reports the worst difference from the
// formula and how many distinct colours the strip holds; the exit code is the
// number of cases more than 1 out. KF2_PERPIXEL_PROBE=2 checks the same formula
// against the GTE in play, so between them the chain is closed. See "Per-pixel
// lighting" in docs/RENDERING.md.
//
// 0071. The same strips again with the authored light list: every case above with
// a light uploaded but no recovered depth (it must be untouched, since only a
// packet with a depth is lit), then a wall facing the camera at a known depth, lit
// by point and spot lights, against the shader's formula in C. With no light
// uploaded the program is the one above, which is the "off is bit-identical"
// case. See "Phase 2, the first slice" in docs/REMASTER.md.
//
// 0071, amended. Emissive materials: with the material table bound and the switch
// on, a packet whose material is 0 is the program above to the bit; one whose
// material glows adds its row-1 colour times RGBC before the depth cue, with no
// depth needed; with the switch off the same glowing material changes nothing. See
// "Phase 3, the first slice" in docs/REMASTER.md.
//
// 0071, amended again. The additive glow: the same glowing material with the
// table's alpha set adds RGBC times its colour, fogged on the packet's own curve,
// after the texture is modulated. Four passes on a strip textured with a known
// 15-bit texel: no glow (the texture path's formula), the lit mode, the additive
// mode, and the additive mode untextured. See "The glow is a light source" in
// docs/REMASTER.md.
//
// Then the material's other terms. The authored lights' highlight on the wall at
// depth, untextured, and textured on a metal (tinted by the texel); a point light
// that names a material in its outer cosine (-2 - id), which must leave that
// material unlit by it; and an additive glow flagged unfogged. See "Phase 3, the
// second slice" in docs/REMASTER.md.
//
// 0077. Shadows: the shadow samplers are put on their own units and every light
// unshadowed, as GlCore does (a cube sampler left on unit 0 beside uVram fails every
// draw). Then pass 2's wall with light 0 sampling a depth cubemap: one holding the
// far plane everywhere (the wall exactly as unshadowed), and one whose +Z face has
// an occluder 250 in front of the light over its left half, so the wall left of
// centre is lit as if light 0 were gone and right of centre as if nothing were
// there. The bands where the filter crosses the occluder's edge and the face's are
// not checked. See "Shadows, the first slice" in docs/REMASTER.md.
//
// 0074. The area's fog: with the switch on, a black colour and the game's curve
// must be pass 0 to the bit; a fog colour adds its colour times the cue's weight
// after the texture (untextured and textured); a curve raises the weight to a power
// and caps it, darkening and colouring by the same weight; and a batch whose blended
// texels add skips the colour. See "Phase 5, the second slice" in docs/REMASTER.md.
#define GL_GLES_PROTOTYPES 0
#include <EGL/egl.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

#define F(r,n,a) typedef r(*P_##n)a; P_##n n##_;
F(GLuint,glCreateShader,(GLenum)) F(void,glShaderSource,(GLuint,GLsizei,const char*const*,const GLint*))
F(void,glCompileShader,(GLuint)) F(void,glGetShaderiv,(GLuint,GLenum,GLint*))
F(void,glGetShaderInfoLog,(GLuint,GLsizei,GLsizei*,char*)) F(GLuint,glCreateProgram,(void))
F(void,glAttachShader,(GLuint,GLuint)) F(void,glLinkProgram,(GLuint))
F(void,glGetProgramiv,(GLuint,GLenum,GLint*)) F(void,glGetProgramInfoLog,(GLuint,GLsizei,GLsizei*,char*))
F(void,glUseProgram,(GLuint)) F(GLint,glGetUniformLocation,(GLuint,const char*))
F(void,glUniform1i,(GLint,GLint)) F(void,glUniform1f,(GLint,GLfloat)) F(void,glUniform1ui,(GLint,GLuint))
F(void,glUniform3f,(GLint,GLfloat,GLfloat,GLfloat)) F(void,glUniform2f,(GLint,GLfloat,GLfloat))
F(void,glUniform4fv,(GLint,GLsizei,const GLfloat*))
F(void,glGenBuffers,(GLsizei,GLuint*)) F(void,glBindBuffer,(GLenum,GLuint))
F(void,glBufferData,(GLenum,GLsizeiptr,const void*,GLenum))
F(void,glGenVertexArrays,(GLsizei,GLuint*)) F(void,glBindVertexArray,(GLuint))
F(void,glEnableVertexAttribArray,(GLuint))
F(void,glVertexAttribPointer,(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*))
F(void,glGenFramebuffers,(GLsizei,GLuint*)) F(void,glBindFramebuffer,(GLenum,GLuint))
F(void,glFramebufferTexture2D,(GLenum,GLenum,GLenum,GLuint,GLint))
F(GLenum,glCheckFramebufferStatus,(GLenum)) F(void,glBindAttribLocation,(GLuint,GLuint,const char*))
F(void,glActiveTexture,(GLenum)) F(void,glUniform4i,(GLint,GLint,GLint,GLint,GLint))
F(void,glUniform1iv,(GLint,GLsizei,const GLint*)) F(void,glUniformMatrix3fv,(GLint,GLsizei,GLboolean,const GLfloat*))

static void load(void){
#define L(n) n##_=(P_##n)eglGetProcAddress(#n); if(!n##_){printf("missing %s\n",#n);exit(2);}
 L(glCreateShader)L(glShaderSource)L(glCompileShader)L(glGetShaderiv)L(glGetShaderInfoLog)
 L(glCreateProgram)L(glAttachShader)L(glLinkProgram)L(glGetProgramiv)L(glGetProgramInfoLog)
 L(glUseProgram)L(glGetUniformLocation)L(glUniform1i)L(glUniform1f)L(glUniform1ui)L(glUniform3f)L(glUniform2f)L(glUniform4fv)
 L(glGenBuffers)L(glBindBuffer)L(glBufferData)L(glGenVertexArrays)L(glBindVertexArray)
 L(glEnableVertexAttribArray)L(glVertexAttribPointer)L(glGenFramebuffers)L(glBindFramebuffer)
 L(glFramebufferTexture2D)L(glCheckFramebufferStatus)L(glBindAttribLocation)L(glActiveTexture)L(glUniform4i)
 L(glUniform1iv)L(glUniformMatrix3fv)
}

static char *slurp(const char*p){FILE*f=fopen(p,"rb");if(!f){perror(p);exit(2);}
 fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);char*b=malloc(n+1);
 if(fread(b,1,n,f)!=(size_t)n)exit(2);b[n]=0;fclose(f);return b;}

#define VW 256
static const char *VS =
"#version 330 core\n"
"layout(location=0) in vec2 aPos;\n"
"noperspective out vec4 vColor; out vec2 vUV; out float vDepth;\n"
"flat out ivec2 clutBase; flat out ivec2 pageBase; flat out int texMode;\n"
"flat out int vDither; flat out int vRepClut;\n"
"noperspective out vec3 vLit; noperspective out float vFog; flat out uint vLight; flat out uint vMat; flat out uvec2 vTex;\n"
"uniform uint uTestLight; uniform vec3 uLit0, uLit1; uniform float uFog0, uFog1; uniform float uTestDepth; uniform uint uTestMat; uniform int uTestTex;\n"
"void main(){\n"
"  gl_Position = vec4(aPos,0.0,1.0);\n"
"  float t = aPos.x*0.5+0.5;\n"
"  vLit = mix(uLit0, uLit1, t); vFog = mix(uFog0, uFog1, t); vLight = uTestLight;\n"
"  vColor = vec4(1.0); vUV = vec2(3.0, 5.0); vDepth = uTestDepth; clutBase = ivec2(0); pageBase = ivec2(0);\n"
"  texMode = uTestTex != 0 ? 2 : 4; vDither = 0; vRepClut = 0; vMat = uTestMat; vTex = uvec2(0u);\n"
"}\n";

#define VH 2
// The light list, as RemasterUniforms lays it out, and the view it is in.
#define NL 3
static float LPOS[NL*4], LCOL[NL*4], LDIR[NL*4];
static const float H=200.f, CX=128.f, CY=1.f, DEPTH=2000.f;

static float sstep(float e0,float e1,float x){float t=(x-e0)/(e1-e0);t=t<0?0:t>1?1:t;return t*t*(3-2*t);}

// authored() in C, at pixel (x,y), for a wall at DEPTH facing the camera; `hi` the
// highlight at `spec` and `rough`, and `mat` the packet's material, which a light
// naming it in its outer cosine does not reach.
static void authored2(int n,int x,int y,float out[3],float spec,float rough,float hi[3],int mat){
 float z=DEPTH, p[3]={((x+0.5f)-CX)*(z/H),((y+0.5f)-CY)*(z/H),z}, nn[3]={0,0,-1};
 float pl=sqrtf(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]), eye[3]={-p[0]/pl,-p[1]/pl,-p[2]/pl};
 float a=rough>0.15f?rough:0.15f; a*=a; float shin=2.f/(a*a)-2.f, norm=(shin+8.f)/25.1327f;
 out[0]=out[1]=out[2]=0; hi[0]=hi[1]=hi[2]=0;
 for(int i=0;i<n;i++){
   if(LDIR[i*4+3]< -2.5f && mat!=0 && (int)(-LDIR[i*4+3]-2.f+0.5f)==mat) continue;
   float l[3]={LPOS[i*4]-p[0],LPOS[i*4+1]-p[1],LPOS[i*4+2]-p[2]};
   float r2=LPOS[i*4+3]*LPOS[i*4+3], d2=l[0]*l[0]+l[1]*l[1]+l[2]*l[2]; if(d2>=r2) continue;
   float inv=1.f/sqrtf(d2), dir[3]={l[0]*inv,l[1]*inv,l[2]*inv};
   float q=1.f-d2/r2, ndl=nn[0]*dir[0]+nn[1]*dir[1]+nn[2]*dir[2]; if(ndl<0)ndl=0;
   float spot=sstep(LDIR[i*4+3],LCOL[i*4+3],-(dir[0]*LDIR[i*4]+dir[1]*LDIR[i*4+1]+dir[2]*LDIR[i*4+2]));
   for(int c=0;c<3;c++) out[c]+=LCOL[i*4+c]*(ndl*q*q*spot);
   if(spec>0&&ndl>0){ float h[3]={dir[0]+eye[0],dir[1]+eye[1],dir[2]+eye[2]}; float hl=sqrtf(h[0]*h[0]+h[1]*h[1]+h[2]*h[2]);
     float nh=(nn[0]*h[0]+nn[1]*h[1]+nn[2]*h[2])/hl; if(nh<0)nh=0; float k=spec*norm*powf(nh,shin)*ndl*q*q*spot;
     for(int c=0;c<3;c++) hi[c]+=LCOL[i*4+c]*k; } } }
static void authored(int n,int x,int y,float out[3]){ float hi[3]; authored2(n,x,y,out,0,0,hi,0); }
// The same with light 0 gone.
static void authoredNo0(int n,int x,int y,float out[3]){ float r=LPOS[3]; LPOS[3]=0; authored(n,x,y,out); LPOS[3]=r; }

// 0074. The fog as the passes set it.
static int FOGON=0, FOGSKIP=0; static float FOGC[3]={0,0,0}, FPOW=1.f, FMAX=1.f;
static float shapew(float w){ if(!FOGON||(FPOW==1.f&&FMAX==1.f)||w<=0) return w;
 float x=w/4096.f; if(x>1)x=1; x=powf(x,FPOW); if(x>FMAX)x=FMAX; return 4096.f*x; }

static const float BK[3]={1920,1920,1920};
static const float LCM[9]={2662,2662,3328, 2662,2662,3328, 2662,2662,3328};

static float cue(unsigned light, float fog){
 unsigned c=(light>>24)&7; float ir0=fog<0?0:fog>4096?4096:fog;
 return shapew(c==1 ? (ir0-800>0?(ir0-800)*2:0) : c==2 ? (ir0<2800?ir0:3*ir0-5600) : c==3 ? ir0*0.5f : c==4 ? fog : 0); }
// The fog's colour at a pixel, 8-bit, added past the texture.
static int fog8(unsigned light, float fog, int ch){
 if(!FOGON||FOGSKIP) return 0; float w=cue(light,fog)/4096.f; w=w<0?0:w>1?1:w; return (int)floorf(FOGC[ch]*w); }

static int expect(unsigned light, float lit[3], float fog, int ch, float extra){
 unsigned mode=light>>24; float l=lit[ch];
 if(mode&0x80){ float a[3]; for(int i=0;i<3;i++){a[i]=lit[i]<0?0:lit[i]>32767?32767:lit[i];}
   float ir=BK[ch]+(LCM[ch*3]*a[0]+LCM[ch*3+1]*a[1]+LCM[ch*3+2]*a[2])/4096.f;
   ir=ir<0?0:ir>32767?32767:ir; l=((light>>(8*ch))&255)*ir/4096.f; }
 l+=((light>>(8*ch))&255)*extra;
 float ir0=fog<0?0:fog>4096?4096:fog; float w; unsigned c=mode&7;
 w = c==1 ? (ir0-800>0?(ir0-800)*2:0) : c==2 ? (ir0<2800?ir0:3*ir0-5600) : c==3 ? ir0*0.5f : c==4 ? fog : 0;
 w=shapew(w);
 float v=floor(l*(1-w/4096.f)); return v<0?0:v>255?255:(int)v; }

// The additive glow at a pixel, 8-bit, and the texel the textured strip reads (a
// 15-bit colour as RGBA8, which is what the VRAM texture holds).
static int glow8(unsigned light, float fog, int ch, float e){
 float v=floor(((light>>(8*ch))&255)*e*(1-cue(light,fog)/4096.f)); return v<0?0:v>255?255:(int)v; }
static const unsigned char TEXEL[3]={66,165,214};

int main(int argc,char**argv){
 EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);
 if(!eglInitialize(d,NULL,NULL)){printf("egl init\n");return 2;}
 eglBindAPI(EGL_OPENGL_API);
 EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_NONE};
 EGLConfig cfg;EGLint nc=0;eglChooseConfig(d,ca,&cfg,1,&nc);
 EGLint ctxa[]={EGL_CONTEXT_MAJOR_VERSION,3,EGL_CONTEXT_MINOR_VERSION,3,EGL_NONE};
 EGLContext ctx=eglCreateContext(d,cfg,EGL_NO_CONTEXT,ctxa);
 eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx);
 load();
 char*fs=slurp(argv[1]);
 GLuint v=glCreateShader_(GL_VERTEX_SHADER),f=glCreateShader_(GL_FRAGMENT_SHADER);
 glShaderSource_(v,1,&VS,NULL);glCompileShader_(v);
 glShaderSource_(f,1,(const char*const*)&fs,NULL);glCompileShader_(f);
 GLint ok; char log[8192]; GLsizei ln;
 glGetShaderiv_(v,GL_COMPILE_STATUS,&ok); if(!ok){glGetShaderInfoLog_(v,8192,&ln,log);printf("VS:%s\n",log);return 2;}
 glGetShaderiv_(f,GL_COMPILE_STATUS,&ok); if(!ok){glGetShaderInfoLog_(f,8192,&ln,log);printf("FS:%s\n",log);return 2;}
 GLuint p=glCreateProgram_();glAttachShader_(p,v);glAttachShader_(p,f);
 glBindAttribLocation_(p,0,"aPos"); glLinkProgram_(p);
 glGetProgramiv_(p,GL_LINK_STATUS,&ok); if(!ok){glGetProgramInfoLog_(p,8192,&ln,log);printf("LINK:%s\n",log);return 2;}
 glUseProgram_(p);
 GLint l;
 if((l=glGetUniformLocation_(p,"uTrueColor"))>=0) glUniform1f_(l,1.f); else {printf("no uTrueColor\n");return 2;}
 if((l=glGetUniformLocation_(p,"uSetMask"))>=0) glUniform1f_(l,0.f);
 if((l=glGetUniformLocation_(p,"uCheckMask"))>=0) glUniform1i_(l,0);
 if((l=glGetUniformLocation_(p,"uOpaqueDepth"))>=0) glUniform1i_(l,0);
 // 0077. As GlCore sets them up.
 for(int i=0;i<4;i++){ char nm[16]; snprintf(nm,sizeof nm,"uShadow%d",i); if((l=glGetUniformLocation_(p,nm))>=0) glUniform1i_(l,12+i); else {printf("no %s\n",nm);return 2;} }
 GLint uLS=glGetUniformLocation_(p,"uLightShadow"); if(uLS<0){printf("no uLightShadow\n");return 2;}
 static const GLint NOSH[16]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
 glUniform1iv_(uLS,16,NOSH);
 glUniform3f_(glGetUniformLocation_(p,"uLightBk"),BK[0],BK[1],BK[2]);
 glUniform3f_(glGetUniformLocation_(p,"uLcmR"),LCM[0],LCM[1],LCM[2]);
 glUniform3f_(glGetUniformLocation_(p,"uLcmG"),LCM[3],LCM[4],LCM[5]);
 glUniform3f_(glGetUniformLocation_(p,"uLcmB"),LCM[6],LCM[7],LCM[8]);

 GLuint rt; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D,rt);
 glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,VW,VH,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
 GLuint fbo; glGenFramebuffers_(1,&fbo); glBindFramebuffer_(GL_FRAMEBUFFER,fbo);
 glFramebufferTexture2D_(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
 if(glCheckFramebufferStatus_(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){printf("fbo\n");return 2;}
 glViewport(0,0,VW,VH);
 float quad[]={-1,-1, 3,-1, -1,3};
 GLuint vao,vbo; glGenVertexArrays_(1,&vao); glBindVertexArray_(vao);
 glGenBuffers_(1,&vbo); glBindBuffer_(GL_ARRAY_BUFFER,vbo);
 glBufferData_(GL_ARRAY_BUFFER,sizeof quad,quad,GL_STATIC_DRAW);
 glEnableVertexAttribArray_(0); glVertexAttribPointer_(0,2,GL_FLOAT,GL_FALSE,8,0);

 struct { const char*name; unsigned light; float l0[3], l1[3]; float f0, f1; } cases[] = {
  {"tile, knee",      (0x40u|2)<<24,            {200,120,60},{200,120,60}, -1500, 5500},
  {"tile, half",      (0x40u|3)<<24,            {255,255,255},{255,255,255}, -500, 5000},
  {"flat, offset",    (0x40u|1)<<24,            {400,300,90},{400,300,90}, 0, 4096},
  {"word",            (0x40u|4)<<24,            {180,180,180},{180,180,180}, 0, 6688},
  {"none",            (0x40u|0)<<24,            {300,20,255},{300,20,255}, 0, 4096},
  {"lit, knee",       ((0xC0u|2)<<24)|0x806040, {-4000,1500,3000},{4000,-2000,500}, 500, 3500},
  {"lit, bright",     ((0xC0u|0)<<24)|0xFFFFFF, {-8000,8000,-100},{9000,-500,12000}, 0, 0},
  {"tile rgbc, knee",  ((0x40u|2)<<24)|0x808080, {60,50,40},{60,50,40}, -500, 3500},
  {"tile rgbc, none",  ((0x40u|0)<<24)|0x6080A0, {30,30,30},{90,90,90}, 0, 0},
 };
 GLint uL=glGetUniformLocation_(p,"uTestLight"), uL0=glGetUniformLocation_(p,"uLit0"), uL1=glGetUniformLocation_(p,"uLit1");
 GLint uF0=glGetUniformLocation_(p,"uFog0"), uF1=glGetUniformLocation_(p,"uFog1");
 unsigned char px[VW*VH*4]; int fails=0;
 GLint uD=glGetUniformLocation_(p,"uTestDepth"), uN=glGetUniformLocation_(p,"uLightN");
 if(uN<0){printf("no uLightN\n");return 2;}
 glUniform1i_(glGetUniformLocation_(p,"uScale"),1);
 glUniform2f_(glGetUniformLocation_(p,"uLightCentre"),CX,CY);
 glUniform1f_(glGetUniformLocation_(p,"uLightH"),H);
 // A warm point light 500 in front of the wall's centre, a cool one to the right,
 // and a spot pointing into the wall from the left.
 float lp[NL*4]={0,0,DEPTH-500,1500, 900,0,DEPTH-300,1200, -800,0,DEPTH-600,2500};
 float lc[NL*4]={0.9f,0.5f,0.2f,-1, 0.2f,0.4f,1.2f,-1, 1.0f,1.0f,0.8f,0.9848f};
 float ld[NL*4]={0,0,0,-2, 0,0,0,-2, 0.3f,0,0.954f,0.8192f};
 memcpy(LPOS,lp,sizeof lp); memcpy(LCOL,lc,sizeof lc); memcpy(LDIR,ld,sizeof ld);
 glUniform4fv_(glGetUniformLocation_(p,"uLightPos"),NL,LPOS);
 glUniform4fv_(glGetUniformLocation_(p,"uLightCol"),NL,LCOL);
 glUniform4fv_(glGetUniformLocation_(p,"uLightDir"),NL,LDIR);
 // The material table: id 5 glows, every other id is dark.
 #define MAT 5
 static const float EMIT[3]={1.2f,0.6f,0.15f};
 static float table[256*3*4];
 table[(256+MAT)*4]=EMIT[0]; table[(256+MAT)*4+1]=EMIT[1]; table[(256+MAT)*4+2]=EMIT[2];
 #define MATADD 6
 table[(256+MATADD)*4]=EMIT[0]; table[(256+MATADD)*4+1]=EMIT[1]; table[(256+MATADD)*4+2]=EMIT[2]; table[(256+MATADD)*4+3]=1.f;
 // 7 has a highlight (roughness 0.4); 8 is the same on a metal; 9 an unfogged
 // additive glow.
 #define MATSPEC 7
 #define MATMETAL 8
 #define MATNOFOG 9
 #define MATSKIP 10
 static const float SPEC=0.8f, ROUGH=0.4f;
 table[MATSPEC*4+2]=ROUGH; table[(512+MATSPEC)*4]=SPEC;
 table[MATMETAL*4+2]=ROUGH; table[MATMETAL*4+3]=1.f; table[(512+MATMETAL)*4]=SPEC;
 table[(256+MATNOFOG)*4]=EMIT[0]; table[(256+MATNOFOG)*4+1]=EMIT[1]; table[(256+MATNOFOG)*4+2]=EMIT[2]; table[(256+MATNOFOG)*4+3]=3.f;
 GLuint mt; glGenTextures(1,&mt); glActiveTexture_(0x84C0+6); glBindTexture(GL_TEXTURE_2D,mt);
 glTexImage2D(GL_TEXTURE_2D,0,0x8814 /*RGBA32F*/,256,3,0,GL_RGBA,GL_FLOAT,table);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 glActiveTexture_(0x84C0);
 GLint uEmit=glGetUniformLocation_(p,"uEmitOn"), uMatT=glGetUniformLocation_(p,"uMatTable"), uM=glGetUniformLocation_(p,"uTestMat");
 if(uEmit<0||uMatT<0){printf("no uEmitOn/uMatTable\n");return 2;}
 glUniform1i_(uMatT,6);
 // VRAM for the textured passes: one texel everywhere, direct 15-bit, opaque.
 static unsigned char vram[1024*512*4];
 for(int i=0;i<1024*512;i++){ vram[i*4]=TEXEL[0]; vram[i*4+1]=TEXEL[1]; vram[i*4+2]=TEXEL[2]; vram[i*4+3]=0; }
 GLuint vt; glGenTextures(1,&vt); glActiveTexture_(0x84C0+7); glBindTexture(GL_TEXTURE_2D,vt);
 glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1024,512,0,GL_RGBA,GL_UNSIGNED_BYTE,vram);
 glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
 glActiveTexture_(0x84C0);
 glUniform1i_(glGetUniformLocation_(p,"uVram"),7);
 glUniform4i_(glGetUniformLocation_(p,"uTexWindow"),255,255,0,0);
 GLint uT=glGetUniformLocation_(p,"uTestTex");
 // pass 0: no light list, no depth (the program as 0048 had it); pass 1: the list
 // uploaded, no depth (nothing may change); pass 2: the list and a wall at DEPTH.
 // pass 3: emissive on, material 0 (as pass 0); pass 4: emissive on, material 5,
 // no depth; pass 5: emissive off, material 5 (as pass 0). Textured: pass 6 no glow,
 // pass 7 material 5 (lit), pass 8 material 6 (additive); pass 9 material 6 untextured.
 // pass 10: the highlight, untextured; 11: textured, on a metal; 12: light 0 names
 // material 10 (nothing else set), drawn as 10; 13: the unfogged glow.
 const char*names[]={"no lights","lights, no depth","lights on a wall at depth 2000",
   "emissive on, material 0","emissive on, material 5 glowing","emissive off, material 5",
   "textured, no glow","textured, lit glow","textured, additive glow","untextured, additive glow",
   "highlight, untextured","highlight, textured metal","own light skipped","unfogged glow",
   "light 0 shadowed, nothing in the cubemap","light 0 shadowed, left half occluded",
   "fog on, black, the game's curve","fog colour, untextured","fog colour, textured",
   "fog colour and curve","fog colour, additive batch"};
 GLint uAO=glGetUniformLocation_(p,"uAtmosOn"), uAC=glGetUniformLocation_(p,"uAtmosColour");
 GLint uAS=glGetUniformLocation_(p,"uAtmosShape"), uAK=glGetUniformLocation_(p,"uAtmosSkip");
 if(uAO<0||uAC<0||uAS<0||uAK<0){printf("no uAtmos*\n");return 2;}
 // 0077. A depth cubemap on unit 12 for light 0: identity view-to-world, and the
 // backend's tuning.
 #define SN 64
 static float face[SN*SN];
 GLuint cube; glGenTextures(1,&cube); glActiveTexture_(0x84C0+12); glBindTexture(0x8513,cube);
 glTexParameteri(0x8513,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(0x8513,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
 glTexParameteri(0x8513,0x884C,0x884E); glTexParameteri(0x8513,0x884D,GL_LEQUAL);
 glTexParameteri(0x8513,GL_TEXTURE_WRAP_S,0x812F); glTexParameteri(0x8513,GL_TEXTURE_WRAP_T,0x812F); glTexParameteri(0x8513,0x8072,0x812F);
 glEnable(0x884F);
 glActiveTexture_(0x84C0);
 static const float ID3[9]={1,0,0,0,1,0,0,0,1};
 glUniformMatrix3fv_(glGetUniformLocation_(p,"uShadowToWorld"),1,GL_FALSE,ID3);
 glUniform1f_(glGetUniformLocation_(p,"uShadowSize"),SN);
 glUniform1f_(glGetUniformLocation_(p,"uShadowOffset"),1.5f);
 glUniform1f_(glGetUniformLocation_(p,"uShadowBias"),6.f);
 glUniform1f_(glGetUniformLocation_(p,"uShadowSoft"),1.25f);
 for(int pass=0;pass<21;pass++){
 // 0074. Passes 16-20.
 FOGON=pass>=16; FOGSKIP=pass==20;
 FOGC[0]=pass==16?0:90; FOGC[1]=pass==16?0:140; FOGC[2]=pass==16?0:200;
 FPOW=pass==19?0.5f:1.f; FMAX=pass==19?0.7f:1.f;
 glUniform1i_(uAO,FOGON); glUniform3f_(uAC,FOGC[0],FOGC[1],FOGC[2]); glUniform2f_(uAS,FPOW,FMAX); glUniform1i_(uAK,FOGSKIP);
 if(pass==14||pass==15){
   glActiveTexture_(0x84C0+12);
   for(int fc=0;fc<6;fc++){
     for(int t=0;t<SN;t++) for(int s2=0;s2<SN;s2++) face[t*SN+s2]=(pass==15&&fc==4&&s2<SN/2)?250.f/65536.f:1.f;
     glTexImage2D(0x8515+fc,0,0x81A6,SN,SN,0,GL_DEPTH_COMPONENT,GL_FLOAT,face); }
   glActiveTexture_(0x84C0);
   GLint sh[16]; for(int i=0;i<16;i++) sh[i]=i==0?0:-1; glUniform1iv_(uLS,16,sh);
 }
 int tex=(pass>=6&&pass<=8)||pass==11||pass==18, add=pass==8||pass==9, lit=pass==2||(pass>=10&&pass<=12)||pass==14||pass==15;
 int mat=add?MATADD:pass==10?MATSPEC:pass==11?MATMETAL:pass==12?MATSKIP:pass==13?MATNOFOG:pass>=14?0:pass>=4?MAT:0;
 // Light 0 names material 10 only in pass 12.
 LDIR[3]=pass==12?-2.f-MATSKIP:-2.f;
 glUniform4fv_(glGetUniformLocation_(p,"uLightDir"),NL,LDIR);
 glUniform1i_(uN,pass==1||lit?NL:0);
 glUniform1f_(uD,lit?DEPTH/65536.f:0.f);
 glUniform1i_(uEmit,pass==3||pass==4||(pass>=7&&pass<16)?1:0);
 glUniform1ui_(uM,mat);
 glUniform1i_(uT,tex);
 printf("-- %s\n", names[pass]);
 for(unsigned k=0;k<sizeof cases/sizeof cases[0];k++){
   glUniform1ui_(uL,cases[k].light);
   glUniform3f_(uL0,cases[k].l0[0],cases[k].l0[1],cases[k].l0[2]);
   glUniform3f_(uL1,cases[k].l1[0],cases[k].l1[1],cases[k].l1[2]);
   glUniform1f_(uF0,cases[k].f0); glUniform1f_(uF1,cases[k].f1);
   glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
   glDrawArrays(GL_TRIANGLES,0,3);
   glReadPixels(0,0,VW,VH,GL_RGBA,GL_UNSIGNED_BYTE,px);
   int worst=0, off1=0, distinct=0, last=-1, lit_px=0;
   for(int y=0;y<VH;y++) for(int x=0;x<VW;x++){
     float t=(x+0.5f)/VW; float lit[3]; for(int i=0;i<3;i++) lit[i]=cases[k].l0[i]+(cases[k].l1[i]-cases[k].l0[i])*t;
     float fog=cases[k].f0+(cases[k].f1-cases[k].f0)*t;
     float ex[3]={0,0,0}, hi[3]={0,0,0};
     if(pass==2||pass==14) authored(NL,x,y,ex);
     // Left of centre the occluder hides light 0; right of it nothing does; the
     // filter's band across the edge is left out.
     if(pass==15){ float px_=((x+0.5f)-CX)*(DEPTH/H); if(px_>-400.f&&px_<-50.f) authoredNo0(NL,x,y,ex);
       else if(px_>50.f&&px_<400.f) authored(NL,x,y,ex); else continue; }
     if(pass>=10&&pass<=12) authored2(NL,x,y,ex,pass==12?0.f:SPEC,ROUGH,hi,mat);
     if(pass==4||pass==7) for(int c=0;c<3;c++) ex[c]=EMIT[c];
     if(ex[0]+ex[1]+ex[2]>0.01f) lit_px++;
     const unsigned char*q=px+(y*VW+x)*4;
     if(add||pass==13) lit_px++;
     for(int ch=0;ch<3;ch++){ int e=expect(cases[k].light,lit,fog,ch,ex[ch]), g=q[ch];
       // The texture path: 248 = 31 << 3, the texel times the shaded colour over 128.
       if(tex){ int t8=(int)(TEXEL[ch]/255.f*248.f+0.5f); e=(t8*e)>>7; }
       if(add) e+=glow8(cases[k].light,fog,ch,EMIT[ch]);
       unsigned rgbc=(cases[k].light>>(8*ch))&255;
       if(pass==13){ float v=floorf(rgbc*EMIT[ch]); e+=(int)(v>255?255:v); }
       if(pass==10||pass==11){ float s8=rgbc*hi[ch]*(1-cue(cases[k].light,fog)/4096.f); s8=s8<0?0:s8>255?255:s8;
         float tint=pass==11?TEXEL[ch]/255.f:1.f; e+=(int)floorf(s8*tint); }
       e+=fog8(cases[k].light,fog,ch);
       if(e>255) e=255;
       int dd=abs(e-g); if(dd>worst)worst=dd; if(dd==1)off1++; }
     int g=q[0]|(q[1]<<8)|(q[2]<<16); if(y==0&&g!=last){distinct++; last=g;}
   }
   printf("%-14s worst |shader - formula| %d, off by 1 in %d channel(s), %d distinct colours across %d px, %d px lit; first %d,%d,%d last %d,%d,%d\n",
     cases[k].name, worst, off1, distinct, VW, lit_px, px[0],px[1],px[2], px[(VW-1)*4],px[(VW-1)*4+1],px[(VW-1)*4+2]);
   if(worst>1) fails++;
 }
 }
 glUniform1iv_(uLS,16,NOSH);
 return fails;}
