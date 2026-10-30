/*
 * nv2a_gpu_gl.c - OpenGL back end for the pushbuffer executor (NV2A GPU).
 *
 * The POSIX sibling of nv2a_gpu.c's D3D11 branch, same contract (see
 * nv2a_gpu.h). The executor (nv2a_pb_exec.c) decodes the NV2A command stream
 * and hands over finished vertices plus the batch state; this rasterises them
 * on the host GPU into one OpenGL render target per guest colour surface, with
 * a depth renderbuffer per surface, and an uber fragment shader that evaluates
 * the NV2A register combiners exactly as nv2a_combiner.c does.
 *
 * Stage 1, as the D3D11 branch describes: rasterisation, combiners, blending,
 * depth and alpha test on the GPU; vertex programs still run in the executor's
 * interpreter (nv2a_gpu_vsh_enabled() returns 0, so nv2a_pb_exec interprets
 * them and calls nv2a_gpu_begin/nv2a_gpu_tri).
 *
 * Guest memory stays the source of truth: a surface is uploaded when first
 * drawn to (or when the CPU changed it), written back at every flip and
 * whenever the CPU is about to read it. Off unless RECOMP_GPU=1.
 */
#if !defined(_WIN32)

#include "nv2a_gpu.h"

#include <SDL.h>
#include <epoxy/gl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "kernel.h"
#include "xbox_memory_layout.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* nv2a_exec_tex_* are declared in nv2a_gpu.h (provided by nv2a_pb_exec.c). */

#define GPU_MAX_SURF 32
#define GPU_MAX_TEX  96
#define FLOATS_PER_VERT 32            /* sizeof(Nv2aGpuVert)/4 */

/* ── shaders ───────────────────────────────────────────────────────────── */

static const char *VS_SRC =
"#version 330 core\n"
"layout(location=0) in vec4 a_pos;\n"
"layout(location=1) in vec4 a_d0;\n"
"layout(location=2) in vec4 a_d1;\n"
"layout(location=3) in vec4 a_fog;\n"
"layout(location=4) in vec4 a_t0;\n"
"layout(location=5) in vec4 a_t1;\n"
"layout(location=6) in vec4 a_t2;\n"
"layout(location=7) in vec4 a_t3;\n"
"out vec4 v_pos;\n"
"noperspective out vec4 v_d0;\n"
"noperspective out vec4 v_d1;\n"
"noperspective out vec4 v_fog;\n"
"out vec4 v_t0; out vec4 v_t1; out vec4 v_t2; out vec4 v_t3;\n"
"uniform int u_is2d;\n"
"uniform vec2 u_half;\n"
"void main(){\n"
"  /* The executor's vertices are screen-space: x,y in guest pixels and z in\n"
"     24-bit depth units (the screen-space epilogue), 2D or program alike. */\n"
"  float ndc_x = a_pos.x * u_half.x - 1.0;\n"
"  float ndc_y = 1.0 - a_pos.y * u_half.y;\n"
"  float ndc_z = u_is2d != 0 ? 0.0\n"
"              : clamp(a_pos.z * (1.0 / 16777215.0), 0.0, 1.0) * 2.0 - 1.0;\n"
"  gl_Position = vec4(ndc_x, ndc_y, ndc_z, 1.0);\n"
"  v_pos = a_pos;\n"
"  v_d0=a_d0; v_d1=a_d1; v_fog=a_fog;\n"
"  v_t0=a_t0; v_t1=a_t1; v_t2=a_t2; v_t3=a_t3;\n"
"}\n";

static const char *FS_HEAD =
"#version 330 core\n"
"noperspective in vec4 v_d0; noperspective in vec4 v_d1; noperspective in vec4 v_fog;\n"
"in vec4 v_t0; in vec4 v_t1; in vec4 v_t2; in vec4 v_t3; in vec4 v_pos;\n"
"out vec4 frag_color;\n"
"uniform uvec4 g_mode;\n"
"uniform uvec4 g_rc;\n"
"uniform uvec4 g_rc2;\n"
"uniform uvec4 g_cw[8];\n"
"uniform uvec4 g_fac[8];\n"
"uniform uvec4 g_tex[4];\n"
"uniform vec4  g_tsize[4];\n"
"uniform vec4  g_fogc;\n"
"uniform vec4  g_zc;\n"
"uniform sampler2DArray T0; uniform sampler2DArray T1;\n"
"uniform sampler2DArray T2; uniform sampler2DArray T3;\n";

static const char *FS_BODY =
"uint RCX=g_rc.x, RCY=g_rc.y, RCZ=g_rc.z, RCW=g_rc.w;\n"
"vec4 R[16];\n"
"float sat(float x){ return clamp(x,0.0,1.0); }\n"
"float lodf(vec4 t, uint s, bool project){\n"
"  float q=(project&&t.w!=0.0)?t.w:1.0; vec2 uv=t.xy/q;\n"
"  if((g_tex[s].x&2u)!=0u) uv*=g_tsize[s].xy;\n"
"  vec2 dx=dFdx(uv), dy=dFdy(uv);\n"
"  return 0.5*log2(max(max(dot(dx,dx),dot(dy,dy)),1e-12));\n"
"}\n"
"vec4 texel(uint s, ivec3 c, uint f, uint m){\n"
"  int w=max(int(g_tsize[s].x)>>int(m),1), h=max(int(g_tsize[s].y)>>int(m),1);\n"
"  ivec3 cc=ivec3(c.x%w, c.y%h, c.z);\n"
"  cc.x=cc.x<0?cc.x+w:cc.x; cc.y=cc.y<0?cc.y+h:cc.y;\n"
"  vec4 r=(s==0u)?texelFetch(T0,cc,int(m)):(s==1u)?texelFetch(T1,cc,int(m)):\n"
"          (s==2u)?texelFetch(T2,cc,int(m)):texelFetch(T3,cc,int(m));\n"
"  if((g_tex[s].x&16u)!=0u) r.a=1.0;\n"
"  return r;\n"
"}\n"
"vec4 rc_texel(uint s, uint f, float u, float v){\n"
"  uint fl=g_tex[s].x; if((fl&1u)==0u) return vec4(1.0);\n"
"  if((fl&2u)!=0u){ u*=g_tsize[s].x; v*=g_tsize[s].y; }\n"
"  return texel(s, ivec3(int(floor(u)),int(floor(v)),0), f, 0u);\n"
"}\n"
"bool stage_fetch(uint st, vec4 c, out vec4 o){\n"
"  uint mode=(RCY>>(st*5u))&0x1Fu; float q=c.w!=0.0?c.w:1.0; o=vec4(0.0);\n"
"  if(mode==0u){ o=vec4(0,0,0,1); return true; }\n"
"  if(mode==1u||mode==2u){ o=rc_texel(st,0u,c.x/q,c.y/q); return true; }\n"
"  if(mode==4u){ o=clamp(c,0.0,1.0); return true; }\n"
"  if(mode==5u){ for(int j=0;j<4;j++){ uint ge=(g_rc2.z>>uint(st*4u+uint(j)))&1u;\n"
"      if(ge!=0u?c[j]>=0.0:c[j]<0.0) return false; } return true; }\n"
"  return true;\n"
"}\n"
"float map_in(float x, uint b){\n"
"  float p=max(x,0.0); uint m=(b>>5)&7u;\n"
"  if(m==1u)return 1.0-sat(x); if(m==2u)return 2.0*p-1.0; if(m==3u)return -2.0*p+1.0;\n"
"  if(m==4u)return p-0.5; if(m==5u)return -p+0.5; if(m==6u)return x; if(m==7u)return -x;\n"
"  return p;\n"
"}\n"
"float map_out(float x, uint m){\n"
"  if(m==0x08u)return x-0.5; if(m==0x10u)return x*2.0; if(m==0x18u)return (x-0.5)*2.0;\n"
"  if(m==0x20u)return x*4.0; if(m==0x30u)return x*0.5; return x;\n"
"}\n"
"vec3 map_out3(vec3 v,uint m){ return clamp(vec3(map_out(v.x,m),map_out(v.y,m),map_out(v.z,m)),-1.0,1.0); }\n"
"vec4 unpackc(uint c){ return vec4((c>>16)&255u,(c>>8)&255u,c&255u,(c>>24)&255u)/255.0; }\n"
"vec3 read_rgb(uint b){ vec4 r=R[b&15u]; vec3 v=(b&0x10u)!=0u?r.aaa:r.rgb;\n"
"  return vec3(map_in(v.x,b),map_in(v.y,b),map_in(v.z,b)); }\n"
"float read_a(uint b){ vec4 r=R[b&15u]; return map_in((b&0x10u)!=0u?r.a:r.b,b); }\n"
"vec4 rc_eval(vec4 v0,vec4 v1,vec4 fog,vec4 t0,vec4 t1,vec4 t2,vec4 t3){\n"
"  uint n=min(RCX&0xFFu,8u), flags=RCX>>8;\n"
"  for(int s=0;s<16;s++) R[s]=vec4(0.0);\n"
"  R[3]=fog; R[4]=v0; R[5]=v1; R[8]=t0; R[9]=t1; R[10]=t2; R[11]=t3;\n"
"  R[12].a=(RCY&0x1Fu)!=0u?t0.a:1.0;\n"
"  for(uint s=0u;s<n;s++){\n"
"    uint icw=g_cw[s].x, ocw=g_cw[s].y, aicw=g_cw[s].z, aocw=g_cw[s].w;\n"
"    uint fl=ocw>>12, afl=aocw>>12;\n"
"    R[1]=unpackc(g_fac[(flags&0x010u)!=0u?s:0u].x);\n"
"    R[2]=unpackc(g_fac[(flags&0x100u)!=0u?s:0u].y);\n"
"    bool mux=(flags&1u)!=0u?(R[12].a>=0.5):(((int(R[12].a*255.0))&1)!=0);\n"
"    vec3 A=read_rgb(icw>>24u),B=read_rgb(icw>>16u),C=read_rgb(icw>>8u),D=read_rgb(icw);\n"
"    float aA=read_a(aicw>>24u),aB=read_a(aicw>>16u),aC=read_a(aicw>>8u),aD=read_a(aicw);\n"
"    float dab=dot(A,B),dcd=dot(C,D);\n"
"    vec3 pab=(fl&2u)!=0u?vec3(dab):A*B, pcd=(fl&1u)!=0u?vec3(dcd):C*D;\n"
"    vec3 sum=(fl&4u)!=0u?(mux?pcd:pab):pab+pcd;\n"
"    vec3 ab=map_out3(pab,fl&0x38u),cd=map_out3(pcd,fl&0x38u),ms=map_out3(sum,fl&0x38u);\n"
"    float apab=aA*aB,apcd=aC*aD, asum=(afl&4u)!=0u?(mux?apcd:apab):apab+apcd;\n"
"    float aab=clamp(map_out(apab,afl&0x38u),-1.0,1.0),acd=clamp(map_out(apcd,afl&0x38u),-1.0,1.0),ams=clamp(map_out(asum,afl&0x38u),-1.0,1.0);\n"
"    uint d;\n"
"    d=(ocw>>4)&15u; if(d!=0u){R[d].rgb=ab; if((fl&0x80u)!=0u)R[d].a=ab.z;}\n"
"    d=ocw&15u;      if(d!=0u){R[d].rgb=cd; if((fl&0x40u)!=0u)R[d].a=cd.z;}\n"
"    d=(ocw>>8)&15u; if(d!=0u)R[d].rgb=ms;\n"
"    d=(aocw>>4)&15u;if(d!=0u)R[d].a=aab;\n"
"    d=aocw&15u;     if(d!=0u)R[d].a=acd;\n"
"    d=(aocw>>8)&15u;if(d!=0u)R[d].a=ams;\n"
"    R[0]=vec4(0.0);\n"
"  }\n"
"  vec4 outc;\n"
"  if(RCZ!=0u||RCW!=0u){\n"
"    uint f0=RCZ,f1=RCW,ff=f1&0xFFu;\n"
"    R[1]=unpackc(g_rc2.x); R[2]=unpackc(g_rc2.y);\n"
"    vec3 va=(ff&0x40u)!=0u?vec3(1.0)-R[5].rgb:R[5].rgb;\n"
"    vec3 vb=(ff&0x20u)!=0u?vec3(1.0)-R[12].rgb:R[12].rgb;\n"
"    R[14]=vec4((ff&0x80u)!=0u?clamp(va+vb,0.0,1.0):va+vb,0.0);\n"
"    vec3 E=read_rgb(f1>>24u),F=read_rgb(f1>>16u); R[15]=vec4(E*F,0.0);\n"
"    vec3 A=read_rgb(f0>>24u),B=read_rgb(f0>>16u),C=read_rgb(f0>>8u),D=read_rgb(f0);\n"
"    outc.rgb=D+C*(vec3(1.0)-A)+B*A; outc.a=read_a(f1>>8u);\n"
"  } else outc=R[12];\n"
"  return clamp(outc,0.0,1.0);\n"
"}\n"
"bool atest(float a){\n"
"  int v=int(a*255.0+0.5), r=int(g_mode.w&255u); uint f=g_mode.z;\n"
"  if(f==0x200u)return false; if(f==0x201u)return v<r; if(f==0x202u)return v==r;\n"
"  if(f==0x203u)return v<=r; if(f==0x204u)return v>r; if(f==0x205u)return v!=r;\n"
"  if(f==0x206u)return v>=r; return true;\n"
"}\n";

static const char *FS_TAIL =
"  if(mode==0u){ vec4 col=clamp(v_d0,0.0,1.0);\n"
"    if((g_tex[0].x&1u)!=0u) col=texel(0u,ivec3(int(floor(max(v_t0.x,0.0))),int(floor(max(v_t0.y,0.0))),0),0u,0u);\n"
"    frag_color=col; return; }\n"
"  if(mode==1u){ vec4 col=clamp(v_d0,0.0,1.0);\n"
"    if((g_tex[0].x&1u)!=0u) col=clamp(rc_texel(0u,0u,v_t0.x,v_t0.y)*v_d0,0.0,1.0);\n"
"    if(g_mode.y!=0u && !atest(col.a)) discard; frag_color=col; return; }\n"
"  if((g_zc.z>0.5) && (v_pos.z<g_zc.x || v_pos.z>g_zc.y)) discard;\n"
"  vec4 t0=vec4(0,0,0,1),t1=t0,t2=t0,t3=t0;\n"
"  if(!stage_fetch(0u,v_t0,t0)) discard;\n"
"  if(!stage_fetch(1u,v_t1,t1)) discard;\n"
"  if(!stage_fetch(2u,v_t2,t2)) discard;\n"
"  if(!stage_fetch(3u,v_t3,t3)) discard;\n"
"  vec4 col=rc_eval(v_d0,v_d1,vec4(g_fogc.rgb,v_fog.x),t0,t1,t2,t3);\n"
"  if(g_mode.y!=0u && !atest(col.a)) discard;\n"
"  frag_color=col;\n"
"}\n";

/* present blit: sample the presented surface into the window */
static const char *PRESENT_VS =
"#version 330 core\n"
"layout(location=0) in vec2 a_p;\n"
"out vec2 v_uv;\n"
"void main(){ v_uv=a_p*0.5+0.5; gl_Position=vec4(a_p,0,1); }\n";
static const char *PRESENT_FS =
"#version 330 core\n"
"in vec2 v_uv; out vec4 o; uniform sampler2D S;\n"
"void main(){ o=vec4(texture(S,v_uv).rgb,1); }\n";

/* NV2A video overlay (PVIDEO): the FMV plane, composited into the frame at
 * present, exactly as the D3D11 backend's ps_overlay does. */
extern int nv2a_exec_pvideo(const uint32_t **rgba, uint32_t *w, uint32_t *h,
                            float rect[4], float key[4]);
static const char *OVERLAY_VS =
"#version 330 core\n"
"layout(location=0) in vec2 a_p;\n"           /* -1..1 quad */
"uniform vec4 u_rect;\n"                       /* x,y,w,h in guest pixels */
"uniform vec2 u_gsize;\n"                      /* guest surface size */
"uniform vec2 u_dest;\n"                       /* ndc x,y of the rect origin */
"uniform vec2 u_half;\n"                       /* 2/width, 2/height of the target */
"out vec2 v_uv;\n"
"void main(){\n"
"  float tx = (a_p.x + 1.0) * 0.5;\n"
"  float ty = (a_p.y + 1.0) * 0.5;\n"           /* 0 at bottom */
"  vec2 g = vec2(u_rect.x + tx*u_rect.z, u_rect.y + (1.0 - ty)*u_rect.w);\n"
"  gl_Position = vec4(g.x*u_half.x - 1.0, 1.0 - g.y*u_half.y, 0.0, 1.0);\n"
"  v_uv = vec2(tx, 1.0 - ty);\n"               /* movie row 0 is the top */
"}\n";
static const char *OVERLAY_FS =
"#version 330 core\n"
"in vec2 v_uv; out vec4 o; uniform sampler2D O; uniform vec4 u_key;\n"
"void main(){\n"
"  vec4 c = texture(O, v_uv);\n"
"  /* Where the key is enabled, xemu shows the movie only through the key\n"
"     colour; without the framebuffer sample, drop key-coloured texels. */\n"
"  if (u_key.w > 0.5 && abs(c.r-u_key.r) < 0.05 && abs(c.g-u_key.g) < 0.05\n"
"      && abs(c.b-u_key.b) < 0.05) discard;\n"
"  o = vec4(c.rgb, 1.0);\n"
"}\n";

/* ── state ─────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t base, pitch, bpp, w, h;
    GLuint   tex, fbo, depth;
    int      uploaded, dirty, used, drawn;
    uint32_t touched_frame;        /* last frame the GPU drew into this */
    int32_t  bx0, by0, bx1, by1;    /* union of GPU-drawn rects, -1 = none */
    uint64_t hash;
} GSurf;

typedef struct {
    uint32_t used;
    uint64_t key;
    Nv2aGpuTexDesc d;
    GLuint   tex;
    int      w, h;
} GTex;

static struct {
    int       ok, tried;
    unsigned long gl_tid;    /* the thread that owns the GL context */
    SDL_Window   *win;
    SDL_GLContext ctx;
    GLuint    prog, vao, vbo;
    GLuint    pprog, pvao, pvbo;
    GLuint    oprog, otex;
    GLint     o_rect, o_gsize, o_half, o_key;
    GLint     u_mode, u_rc, u_rc2, u_cw, u_fac, u_tex, u_tsize, u_fogc, u_zc;
    GSurf     surf[GPU_MAX_SURF];
    GTex      tex[GPU_MAX_TEX];
    uint32_t  frame;
    Nv2aGpuState st;
    int       have_st;
    float     verts[FLOATS_PER_VERT * 3200];
    int       nverts;
    int       zpass_on;
    float     winh;
    GLint     u_is2d, u_half;
    unsigned  c_begin, c_tri, c_clear, c_flip, c_tex, c_surf, c_draw;
} G;

static uint8_t *gmem(uint32_t a)
{
    return (uint8_t *)((uintptr_t)xbox_GetMemoryOffset() + (uintptr_t)a);
}

/* Union of the rects the GPU has drawn into a surface, so write-back carries
 * only what the GPU produced and does not clobber memory the CPU touched. */
static void surf_touch(GSurf *s, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)s->w) x1 = (int32_t)s->w;
    if (y1 > (int32_t)s->h) y1 = (int32_t)s->h;
    if (x0 < s->bx0) s->bx0 = x0;
    if (y0 < s->by0) s->by0 = y0;
    if (x1 > s->bx1) s->bx1 = x1;
    if (y1 > s->by1) s->by1 = y1;
}

static uint64_t hash_bytes(const uint8_t *p, size_t n, uint64_t h)
{
    while (n--) h = (h ^ *p++) * 1099511628211ull;
    return h ? h : 1;
}

/* ── GL helpers ────────────────────────────────────────────────────────── */

static GLuint compile(GLenum t, const char *src)
{
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof log, &n, log);
        fprintf(stderr, "[GPU-GL] shader compile failed: %.*s\n", n, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link2(const char *vs, const char *fs, int present)
{
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    GLuint p = 0;
    if (v && f) {
        p = glCreateProgram();
        glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
        GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048]; GLsizei n = 0;
            glGetProgramInfoLog(p, sizeof log, &n, log);
            fprintf(stderr, "[GPU-GL] %s link failed: %.*s\n",
                    present ? "present" : "main", n, log);
            glDeleteProgram(p); p = 0;
        }
    }
    if (v) glDeleteShader(v);
    if (f) glDeleteShader(f);
    return p;
}

static int gpu_init(void)
{
    char *fs;
    size_t n;

    if (!getenv("RECOMP_GPU"))
        return 0;

    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[GPU-GL] SDL video init failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    G.winh = 960.0f;
    {
        const char *wh = getenv("RECOMP_WINDOW_HEIGHT");
        if (wh && atoi(wh) > 240) G.winh = (float)atoi(wh);
    }
    G.win = SDL_CreateWindow("Xbox Game", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                             (int)(G.winh * 4.0f / 3.0f), (int)G.winh,
                             SDL_WINDOW_OPENGL |
                             (getenv("RECOMP_GL_HIDE") ? SDL_WINDOW_HIDDEN
                                                       : SDL_WINDOW_SHOWN));
    if (!G.win) { fprintf(stderr, "[GPU-GL] window: %s\n", SDL_GetError()); return 0; }
    G.ctx = SDL_GL_CreateContext(G.win);
    if (!G.ctx) { fprintf(stderr, "[GPU-GL] gl context: %s\n", SDL_GetError()); SDL_DestroyWindow(G.win); G.win=NULL; return 0; }
    SDL_GL_MakeCurrent(G.win, G.ctx);
    SDL_GL_SetSwapInterval(1);

    /* assemble the fragment shader */
    {
        const char *mh = "void main(){\n  uint mode=g_mode.x;\n";
        n = strlen(FS_HEAD) + strlen(FS_BODY) + strlen(mh) + strlen(FS_TAIL) + 64;
        fs = (char *)malloc(n);
        snprintf(fs, n, "%s%s%s%s", FS_HEAD, FS_BODY, mh, FS_TAIL);
    }
    G.prog = link2(VS_SRC, fs, 0);
    free(fs);
    if (!G.prog) { SDL_GL_DeleteContext(G.ctx); SDL_DestroyWindow(G.win); G.win=NULL; G.ctx=NULL; return 0; }

    G.u_mode  = glGetUniformLocation(G.prog, "g_mode");
    G.u_rc    = glGetUniformLocation(G.prog, "g_rc");
    G.u_rc2   = glGetUniformLocation(G.prog, "g_rc2");
    G.u_cw    = glGetUniformLocation(G.prog, "g_cw");
    G.u_fac   = glGetUniformLocation(G.prog, "g_fac");
    G.u_tex   = glGetUniformLocation(G.prog, "g_tex");
    G.u_tsize = glGetUniformLocation(G.prog, "g_tsize");
    G.u_fogc  = glGetUniformLocation(G.prog, "g_fogc");
    G.u_zc    = glGetUniformLocation(G.prog, "g_zc");
    G.u_is2d  = glGetUniformLocation(G.prog, "u_is2d");
    G.u_half  = glGetUniformLocation(G.prog, "u_half");

    glGenVertexArrays(1, &G.vao);
    glBindVertexArray(G.vao);
    glGenBuffers(1, &G.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof G.verts, NULL, GL_STREAM_DRAW);
    {
        int i;
        for (i = 0; i < 8; i++) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, FLOATS_PER_VERT * 4,
                                  (const void *)(uintptr_t)(i * 16));
        }
    }

    G.pprog = link2(PRESENT_VS, PRESENT_FS, 1);
    glGenVertexArrays(1, &G.pvao);
    glBindVertexArray(G.pvao);
    glGenBuffers(1, &G.pvbo);
    glBindBuffer(GL_ARRAY_BUFFER, G.pvbo);
    {
        static const float quad[8] = { -1,-1, 1,-1, -1,1, 1,1 };
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    }

    G.oprog = link2(OVERLAY_VS, OVERLAY_FS, 1);
    if (G.oprog) {
        G.o_rect  = glGetUniformLocation(G.oprog, "u_rect");
        G.o_gsize = glGetUniformLocation(G.oprog, "u_gsize");
        G.o_half  = glGetUniformLocation(G.oprog, "u_half");
        G.o_key   = glGetUniformLocation(G.oprog, "u_key");
        glGenTextures(1, &G.otex);
    }

    fprintf(stderr, "[GPU-GL] GL %s / %s\n",
            glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    G.gl_tid = (unsigned long)GetCurrentThreadId();
    G.ok = 1;
    return 1;
}

/* ── surfaces ──────────────────────────────────────────────────────────── */

static void surf_free(GSurf *s)
{
    if (s->tex) glDeleteTextures(1, &s->tex);
    if (s->fbo) glDeleteFramebuffers(1, &s->fbo);
    if (s->depth) glDeleteRenderbuffers(1, &s->depth);
    memset(s, 0, sizeof *s);
}

static void surf_upload(GSurf *s)
{
    const uint8_t *src = gmem(s->base);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (s->bpp == 4) {
        if (s->pitch == s->w * 4)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s->w, s->h, GL_BGRA,
                            GL_UNSIGNED_INT_8_8_8_8_REV, src);
        else {
            uint32_t y;
            for (y = 0; y < s->h; y++)
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)y, s->w, 1, GL_BGRA,
                                GL_UNSIGNED_INT_8_8_8_8_REV, src + (size_t)y * s->pitch);
        }
    } else if (s->bpp == 2) {
        uint32_t y;
        for (y = 0; y < s->h; y++)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, (GLint)y, s->w, 1, GL_BGRA,
                            GL_UNSIGNED_SHORT_1_5_5_5_REV, src + (size_t)y * s->pitch);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s->uploaded = 1;
}

static void surf_writeback(GSurf *s)
{
    uint8_t *dst;
    int32_t x0, y0, x1, y1;
    if (s->base == 0x80000000u) {   /* DirectSound's memory, not a colour target */
        s->dirty = 0;
        return;
    }
    if (s->bx1 <= s->bx0 || s->by1 <= s->by0) { s->dirty = 0; return; }
    x0 = s->bx0 < 0 ? 0 : s->bx0;
    y0 = s->by0 < 0 ? 0 : s->by0;
    x1 = s->bx1 > (int32_t)s->w ? (int32_t)s->w : s->bx1;
    y1 = s->by1 > (int32_t)s->h ? (int32_t)s->h : s->by1;
    if (x1 <= x0 || y1 <= y0) { s->dirty = 0; return; }
    dst = gmem(s->base);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (s->bpp == 4) {
        int32_t y;
        for (y = y0; y < y1; y++)
            /* GL row 0 is the bottom; guest row 0 is the top. */
            glReadPixels(x0, (GLint)((int32_t)s->h - 1 - y), x1 - x0, 1, GL_BGRA,
                         GL_UNSIGNED_INT_8_8_8_8_REV,
                         dst + (size_t)y * s->pitch + (size_t)x0 * 4);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s->dirty = 0;
    s->bx0 = s->by0 = 0x7FFFFFFF;
    s->bx1 = s->by1 = -1;
}

/* A guest address range the GPU may own (mirrors nv2a_gpu.c's guest_range_ok):
 * low RAM or the contiguous arena. A wild surface base must never be uploaded
 * from or written back to, or it clobbers whatever else lives there. */
static int guest_range_ok(uint32_t a, uint32_t n)
{
    uint64_t e = (uint64_t)a + n;
    if (a >= 0x00010000u && e <= 0x04000000u) return 1;
    if (a >= 0x80000000u && e <= 0x84000000u) return 1;
    return 0;
}

static GSurf *surf_get(const Nv2aGpuState *st)
{
    uint32_t need = st->clip_y + st->clip_h, i, w;
    GSurf *s = NULL, *lru = NULL;

    if (!st->pitch || !st->bpp || (w = st->pitch / st->bpp) == 0 || w > 4096
        || need == 0 || need > 4096
        || !guest_range_ok(st->surf_base, need * st->pitch))
        return NULL;

    for (i = 0; i < GPU_MAX_SURF; i++) {
        GSurf *c = &G.surf[i];
        if (c->tex && c->base == st->surf_base && c->pitch == st->pitch && c->bpp == st->bpp) {
            s = c; break;
        }
        if (!lru || !c->tex || (lru->tex && c->used < lru->used))
            lru = c;
    }
    if (s && s->h < need) { surf_free(s); lru = s; s = NULL; }
    if (!s) {
        if (lru->tex) surf_free(lru);
        s = lru;
        s->base = st->surf_base; s->pitch = st->pitch; s->bpp = st->bpp;
        s->w = w; s->h = need;
        s->bx0 = s->by0 = 0x7FFFFFFF; s->bx1 = s->by1 = -1;
        G.c_surf++;
        glGenTextures(1, &s->tex);
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->w, s->h, 0, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &s->fbo);
        glGenRenderbuffers(1, &s->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, s->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s->w, s->h);
        glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s->depth);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s->depth);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        surf_upload(s);
    }
    s->used = G.frame ? G.frame : 1;
    return s;
}

/* ── textures ──────────────────────────────────────────────────────────── */

static uint64_t tex_key(const Nv2aGpuTexDesc *d)
{
    uint64_t h = 1469598103934665603ull;
    h = hash_bytes((const uint8_t *)&d->offset, sizeof(uint32_t), h);
    h = hash_bytes((const uint8_t *)&d->width, sizeof(uint32_t), h);
    h = hash_bytes((const uint8_t *)&d->height, sizeof(uint32_t), h);
    h = hash_bytes((const uint8_t *)&d->pitch, sizeof(uint32_t), h);
    h = hash_bytes((const uint8_t *)&d->color, sizeof(uint32_t), h);
    return h;
}

static GLuint tex_get(const Nv2aGpuTexDesc *d)
{
    GTex *e = NULL, *lru = NULL;
    uint32_t bytes, i;
    uint32_t *buf;
    uint64_t key;
    int hw, hh;

    if (!d->valid || d->width == 0 || d->height == 0 || d->cube)
        return 0;
    key = tex_key(d);
    for (i = 0; i < GPU_MAX_TEX; i++) {
        GTex *c = &G.tex[i];
        if (c->tex && c->key == key) { e = c; break; }
        if (!lru || !c->tex || c->used < lru->used) lru = c;
    }
    if (e) { e->used = G.frame ? G.frame : 1; return e->tex; }
    if (!lru) return 0;
    if (lru->tex) { glDeleteTextures(1, &lru->tex); lru->tex = 0; }

    hw = (int)d->width; hh = (int)d->height;
    {
        size_t out_sz = (size_t)hw * (size_t)hh * 4u;   /* decode writes ARGB */
        bytes = nv2a_exec_tex_bytes(d);
        if (bytes == 0 || bytes > (64u << 20) || out_sz == 0 || out_sz > (64u << 20))
            return 0;
        buf = (uint32_t *)malloc(out_sz);
        if (!buf) return 0;
    }
    if (!nv2a_exec_tex_decode(d, 0, buf)) { free(buf); return 0; }

    glGenTextures(1, &lru->tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, lru->tex);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, hw, hh, 1, 0,
                 GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, buf);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(buf);

    lru->key = key; lru->d = *d; lru->w = hw; lru->h = hh;
    lru->used = G.frame ? G.frame : 1;
    G.c_tex++;
    return lru->tex;
}

/* ── state application ─────────────────────────────────────────────────── */

static GLenum blend_of(uint32_t f)
{
    switch (f) {
    case 0: return GL_ZERO; case 1: return GL_ONE;
    case 2: return GL_SRC_COLOR; case 3: return GL_ONE_MINUS_SRC_COLOR;
    case 4: return GL_SRC_ALPHA; case 5: return GL_ONE_MINUS_SRC_ALPHA;
    case 6: return GL_DST_ALPHA; case 7: return GL_ONE_MINUS_DST_ALPHA;
    case 8: return GL_DST_COLOR; case 9: return GL_ONE_MINUS_DST_COLOR;
    default: return GL_ONE;
    }
}
static GLenum cmp_of(uint32_t f)
{
    /* NV2A stores the comparison as SET_DEPTH_FUNC / alpha func: 0x200 NEVER,
     * 0x201 LESS, ... 0x207 ALWAYS. D3D's D3DCMP is the same order 1..7. */
    if (f >= 0x200u && f <= 0x207u) f -= 0x200u;
    switch (f) {
    case 1: return GL_LESS; case 2: return GL_EQUAL; case 3: return GL_LEQUAL;
    case 4: return GL_GREATER; case 5: return GL_NOTEQUAL; case 6: return GL_GEQUAL;
    case 0: return GL_NEVER; default: return GL_ALWAYS;
    }
}

static void set_uniforms(const Nv2aGpuState *st, const GLuint ttex[4])
{
    int i;
    glUniform4ui(G.u_mode, st->mode, st->alpha_test, st->alpha_func, st->alpha_ref);
    glUniform4ui(G.u_rc, st->rc.control, st->rc.stage_program, st->rc.final0, st->rc.final1);
    glUniform4ui(G.u_rc2, st->rc.final_c0, st->rc.final_c1, st->clip_plane_mode, st->z_float);
    {
        GLuint cw[8][4]; GLuint fac[8][4];
        for (i = 0; i < 8; i++) {
            cw[i][0] = st->rc.color_icw[i]; cw[i][1] = st->rc.color_ocw[i];
            cw[i][2] = st->rc.alpha_icw[i]; cw[i][3] = st->rc.alpha_ocw[i];
            fac[i][0] = st->rc.factor0[i]; fac[i][1] = st->rc.factor1[i];
            fac[i][2] = 0; fac[i][3] = 0;
        }
        glUniform4uiv(G.u_cw, 8, &cw[0][0]);
        glUniform4uiv(G.u_fac, 8, &fac[0][0]);
    }
    {
        GLuint tx[4][4]; float ts[4][4];
        for (i = 0; i < 4; i++) {
            uint32_t flags = 0;
            if (st->tex[i].valid) flags |= 1u;
            if (st->tex_norm[i]) flags |= 2u;
            tx[i][0] = flags; tx[i][1] = st->tex[i].addr_u; tx[i][2] = st->tex[i].addr_v;
            tx[i][3] = st->tex[i].levels;
            ts[i][0] = (float)st->tex[i].width; ts[i][1] = (float)st->tex[i].height;
            ts[i][2] = (float)st->tex[i].width; ts[i][3] = (float)st->tex[i].height;
        }
        glUniform4uiv(G.u_tex, 4, &tx[0][0]);
        glUniform4fv(G.u_tsize, 4, &ts[0][0]);
    }
    glUniform4f(G.u_fogc, st->fog_color[0], st->fog_color[1], st->fog_color[2], st->fog_color[3]);
    /* Depth-range drop (g_zc.z) is left off in this first cut: the executor's
     * zclip units and the shader's clip-space z are not yet reconciled. */
    glUniform4f(G.u_zc, 0.0f, 1.0f, 0.0f, 480.0f);

    for (i = 0; i < 4; i++) {
        GLuint id = ttex[i] ? ttex[i] : 0;
        glActiveTexture(GL_TEXTURE0 + i);
        if (id) glBindTexture(GL_TEXTURE_2D_ARRAY, id);
    }
}

static void set_gl_state(const Nv2aGpuState *st)
{
    if (st->blend_enable) {
        glEnable(GL_BLEND);
        glBlendFunc(blend_of(st->blend_sfactor), blend_of(st->blend_dfactor));
    } else glDisable(GL_BLEND);

    if (st->depth_test && !getenv("RECOMP_GL_NODEPTH")) { glEnable(GL_DEPTH_TEST); glDepthFunc(cmp_of(st->depth_func)); }
    else glDisable(GL_DEPTH_TEST);
    glDepthMask(st->depth_mask ? GL_TRUE : GL_FALSE);

    if (st->cull && !getenv("RECOMP_GL_NOCULL")) {
        glEnable(GL_CULL_FACE);
        glFrontFace(st->front_ccw ? GL_CCW : GL_CW);
        glCullFace(st->cull == 0x404 ? GL_FRONT : GL_BACK);
    } else glDisable(GL_CULL_FACE);

    glColorMask(1, 1, 1, 1);

    if (st->clip_w && st->clip_h && !getenv("RECOMP_GL_NOSCISSOR")) {
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)st->clip_x, (GLint)st->clip_y, (GLsizei)st->clip_w, (GLsizei)st->clip_h);
    } else glDisable(GL_SCISSOR_TEST);
}

/* ── interface ─────────────────────────────────────────────────────────── */

int nv2a_gpu_enabled(void)
{
    if (!G.tried) { G.tried = 1; G.ok = gpu_init(); }
    return G.ok;
}

static void flush_tri(void)
{
    if (!G.nverts) return;
    glBindVertexArray(G.vao);
    glBindBuffer(GL_ARRAY_BUFFER, G.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)G.nverts * FLOATS_PER_VERT * 4, G.verts);
    glUseProgram(G.prog);
    glDrawArrays(GL_TRIANGLES, 0, G.nverts);
    G.c_draw++;
    {
        static int said;
        GLenum e = glGetError();
        if (said < 4) {
            said++;
            fprintf(stderr, "[GPU-GL] draw nverts=%d first=(%g %g %g %g) glerr=0x%X\n",
                    G.nverts, G.verts[0], G.verts[1], G.verts[2], G.verts[3], e);
        }
    }
    G.nverts = 0;
}

void nv2a_gpu_begin(const Nv2aGpuState *st)
{
    GSurf *s;
    GLuint tt[4]; int i;

    if (!G.ok) return;
    flush_tri();
    G.st = *st;
    G.have_st = 1;
    s = surf_get(st);
    if (!s) return;
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glViewport(0, 0, (GLsizei)s->w, (GLsizei)s->h);
    set_gl_state(st);
    {
        static int said;
        if (said++ < 6)
            fprintf(stderr, "[GPU-GL] begin base=%08X %ux%u mode=%u dt=%u df=%u dm=%u clip=%u,%u %ux%u\n",
                    s->base, s->w, s->h, st->mode, st->depth_test, st->depth_func,
                    st->depth_mask, st->clip_x, st->clip_y, st->clip_w, st->clip_h);
    }
    if ((st->color_mask & 0x01010101u) && s->base != 0x80000000u) {
        s->dirty = 1;
        s->touched_frame = G.frame;
        if (st->clip_w && st->clip_h)
            surf_touch(s, (int32_t)st->clip_x, (int32_t)st->clip_y,
                       (int32_t)(st->clip_x + st->clip_w), (int32_t)(st->clip_y + st->clip_h));
        else
            surf_touch(s, 0, 0, (int32_t)s->w, (int32_t)s->h);
    }
    glUseProgram(G.prog);
    glBindVertexArray(G.vao);
    glUniform1i(G.u_is2d, st->mode == NV2A_GPU_MODE_2D ? 1 : 0);
    glUniform2f(G.u_half, 2.0f / (float)s->w, 2.0f / (float)s->h);
    for (i = 0; i < 4; i++) tt[i] = tex_get(&st->tex[i]);
    set_uniforms(st, tt);
}

void nv2a_gpu_tri(const Nv2aGpuVert *a, const Nv2aGpuVert *b, const Nv2aGpuVert *c)
{
    if (!G.ok) return;
    G.c_tri++;
    if (G.nverts + 3 > (int)(sizeof G.verts / (FLOATS_PER_VERT * 4)))
        flush_tri();
    memcpy(&G.verts[G.nverts * FLOATS_PER_VERT], a, sizeof *a);
    memcpy(&G.verts[(G.nverts + 1) * FLOATS_PER_VERT], b, sizeof *b);
    memcpy(&G.verts[(G.nverts + 2) * FLOATS_PER_VERT], c, sizeof *c);
    G.nverts += 3;
}

void nv2a_gpu_clear(const Nv2aGpuState *st, int color, uint32_t argb,
                    int depth, uint32_t zstencil_clear)
{
    GSurf *s;
    if (!G.ok) return;
    flush_tri();
    G.c_clear++;
    {
        static int said;
        if (said++ < 8)
            fprintf(stderr, "[GPU-GL] clear color=%d argb=%08X depth=%d z=%08X "
                            "dt=%u df=%u dm=%u\n",
                    color, argb, depth, zstencil_clear,
                    st->depth_test, st->depth_func, st->depth_mask);
    }
    G.st = *st; G.have_st = 1;
    s = surf_get(st);
    if (!s) return;
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glViewport(0, 0, (GLsizei)s->w, (GLsizei)s->h);
    if (st->clip_w && st->clip_h && !getenv("RECOMP_GL_NOSCISSOR")) {
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)st->clip_x, (GLint)st->clip_y, (GLsizei)st->clip_w, (GLsizei)st->clip_h);
    } else glDisable(GL_SCISSOR_TEST);
    if (color) {
        float a = ((argb >> 24) & 0xFF) / 255.0f;
        float r = ((argb >> 16) & 0xFF) / 255.0f;
        float g = ((argb >> 8) & 0xFF) / 255.0f;
        float b = (argb & 0xFF) / 255.0f;
        glClearColor(r, g, b, a);
        glDepthMask(GL_TRUE); glColorMask(1,1,1,1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    if (depth) {
        glDepthMask(GL_TRUE);
        glClearDepth((double)(zstencil_clear & 0xFFFFFF) / (double)0xFFFFFF);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
    if (color && s->base != 0x80000000u) {
        s->dirty = 1;
        s->touched_frame = G.frame;
        if (st->clip_w && st->clip_h)
            surf_touch(s, (int32_t)st->clip_x, (int32_t)st->clip_y,
                       (int32_t)(st->clip_x + st->clip_w), (int32_t)(st->clip_y + st->clip_h));
        else
            surf_touch(s, 0, 0, (int32_t)s->w, (int32_t)s->h);
    }
    s->drawn = 1;
}

static GSurf *surf_find(uint32_t base)
{
    uint32_t i;
    for (i = 0; i < GPU_MAX_SURF; i++)
        if (G.surf[i].tex && G.surf[i].base == base) return &G.surf[i];
    return NULL;
}

/* Development aid: dump a surface (the presented one) to a 24-bit BMP. */
static void gl_dump_surf(GSurf *s, const char *path)
{
    uint32_t row = ((s->w * 3u) + 3u) & ~3u, img = row * s->h, total = 54u + img, y, x;
    uint8_t hdr[54], *line, *px;
    FILE *f;
    if (!s || !s->tex) return;
    px = (uint8_t *)calloc(1, (size_t)s->w * s->h * 4);
    line = (uint8_t *)calloc(1, row);
    if (!px || !line) { free(px); free(line); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, s->w, s->h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    f = fopen(path, "wb");
    if (!f) { free(px); free(line); return; }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4); hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s->w, 4); memcpy(hdr + 22, &s->h, 4);
    hdr[26] = 1; hdr[28] = 24; memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = 0; y < s->h; y++) {
        const uint8_t *src = px + (size_t)y * s->w * 4;
        for (x = 0; x < s->w; x++) {
            line[x*3+0] = src[x*4+0]; line[x*3+1] = src[x*4+1]; line[x*3+2] = src[x*4+2];
        }
        fwrite(line, 1, row, f);
    }
    fclose(f);
    fprintf(stderr, "[GPU-GL] dumped %s (%ux%u base=%08X)\n", path, s->w, s->h, s->base);
}

void nv2a_gpu_sync_range(uint32_t base, uint32_t bytes)
{
    uint32_t i;
    if (!G.ok) return;
    if ((unsigned long)GetCurrentThreadId() != G.gl_tid) return;  /* GL is single-threaded */
    for (i = 0; i < GPU_MAX_SURF; i++) {
        GSurf *s = &G.surf[i];
        if (!s->tex || !s->dirty) continue;
        if (s->base < base + bytes && base < s->base + s->h * s->pitch)
            surf_writeback(s);
    }
}

void nv2a_gpu_sync_all(void)
{
    uint32_t i;
    if (!G.ok) return;
    if ((unsigned long)GetCurrentThreadId() != G.gl_tid) return;
    for (i = 0; i < GPU_MAX_SURF; i++)
        if (G.surf[i].tex && G.surf[i].dirty) surf_writeback(&G.surf[i]);
}

/* nv2a_gpu_forget_range is declared by the toolkit; a no-op here (surfaces are
 * re-uploaded only on demand). */
void nv2a_gpu_forget_range(uint32_t base, uint32_t bytes) { (void)base; (void)bytes; }

void nv2a_gpu_flip(uint32_t present_base)
{
    GSurf *s;
    uint32_t i;
    if (!G.ok) return;
    flush_tri();
    G.frame++;
    G.c_flip++;
    if ((G.c_flip % 300) == 0)
        fprintf(stderr, "[GPU-GL] flips=%u tris=%u clears=%u surfaces=%u textures=%u present=%08X%s\n",
                G.c_flip, G.c_tri, G.c_clear, G.c_surf, G.c_tex, present_base,
                surf_find(present_base) ? "" : " (no surface)");
    s = surf_find(present_base);

    /* The title can write the frame with the CPU (the FMV decoder). If the GPU
     * did not draw this surface this frame, guest memory is the source of
     * truth -- upload it so the decoded frame is shown, not the stale texture. */
    if (s && s->touched_frame + 1 < G.frame)
        surf_upload(s);

    /* NV2A video overlay (the FMV plane): composite it into the frame before
     * write-back and present, as the D3D11 backend does in present_overlay. */
    if (s && G.oprog) {
        const uint32_t *rgba = NULL;
        uint32_t mw = 0, mh = 0;
        float rect[4] = {0}, key[4] = {0};
        if (nv2a_exec_pvideo(&rgba, &mw, &mh, rect, key) && rgba && mw && mh) {
            glBindTexture(GL_TEXTURE_2D, G.otex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)mw, (GLsizei)mh, 0,
                         GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, rgba);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
            glViewport(0, 0, (GLsizei)s->w, (GLsizei)s->h);
            glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glUseProgram(G.oprog);
            glBindVertexArray(G.pvao);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, G.otex);
            glUniform1i(glGetUniformLocation(G.oprog, "O"), 0);
            glUniform4f(G.o_rect, rect[0], rect[1], rect[2], rect[3]);
            glUniform2f(G.o_gsize, 640.0f, 480.0f);
            glUniform2f(G.o_half, 2.0f / (float)s->w, 2.0f / (float)s->h);
            glUniform4f(G.o_key, key[0], key[1], key[2], key[3]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }

    for (i = 0; i < GPU_MAX_SURF; i++)
        if (G.surf[i].tex && G.surf[i].dirty) surf_writeback(&G.surf[i]);

    {
        const char *dump = getenv("RECOMP_GL_DUMP");
        const char *df = getenv("RECOMP_GL_DUMP_FLIP");
        int want = df ? atoi(df) : 2500;
        if (dump && (int)G.c_flip == want && s)
            gl_dump_surf(s, dump);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    {
        int dw, dh;
        SDL_GL_GetDrawableSize(G.win, &dw, &dh);
        glViewport(0, 0, dw, dh);
    }
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (s && G.pprog) {
        glUseProgram(G.pprog);
        glBindVertexArray(G.pvao);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glUniform1i(glGetUniformLocation(G.pprog, "S"), 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    SDL_GL_SwapWindow(G.win);
}

void nv2a_gpu_zpass_enable(uint32_t on) { G.zpass_on = (int)on; }
uint32_t nv2a_gpu_zpass_collect(void) { (void)G.zpass_on; return 0; }

int  nv2a_gpu_vsh_enabled(void) { return 0; }
float *nv2a_gpu_vtx_begin(uint32_t nmax) { (void)nmax; return NULL; }
void nv2a_gpu_vtx_cancel(void) {}
void nv2a_gpu_hitch_counters(double *t, uint32_t *u) { *t = 0; *u = 0; }
void nv2a_gpu_draw_program(const Nv2aGpuState *st, const float *in, uint32_t nverts,
                           const uint32_t *idx, uint32_t nidx, const uint32_t *prog,
                           uint32_t start, const float *consts)
{
    (void)st; (void)in; (void)nverts; (void)idx; (void)nidx; (void)prog; (void)start; (void)consts;
}

#endif /* !_WIN32 */
