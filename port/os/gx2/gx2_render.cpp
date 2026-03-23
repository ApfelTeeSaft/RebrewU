// gx2_render.cpp — OpenGL back-end for GX2
//
// Implements FBO management, texture upload, raster state, and scan-buffer
// blitting.
//
// GL 3.x functions are loaded dynamically via SDL_GL_GetProcAddress to avoid
// any dependency on GLEW / GLAD.

#include "gx2_render.h"
#include "gx2_surface.h"
#include "r700_to_glsl.h"
#include "../os_common.h"  // for rbrew_read32 helpers

#if !defined(HAVE_SDL2)
// Stub bodies when building without SDL2 / OpenGL
void gx2_render_init() {}
void gx2_render_set_color_buffer(uint32_t, uint8_t*) {}
void gx2_render_set_depth_buffer(uint32_t, uint8_t*) {}
void gx2_render_clear_color(float, float, float, float) {}
void gx2_render_clear_depth_stencil(float, uint8_t, uint32_t) {}
void gx2_render_clear_buffers(float, float, float, float, float, uint8_t) {}
void gx2_render_set_viewport(float, float, float, float, float, float) {}
void gx2_render_set_scissor(uint32_t, uint32_t, uint32_t, uint32_t) {}
void gx2_render_set_depth_stencil_control(bool, bool, uint32_t) {}
void gx2_render_set_polygon_control(uint32_t, bool, bool) {}
void gx2_render_set_blend_control(bool, uint32_t, uint32_t, uint32_t,
                                  uint32_t, uint32_t, uint32_t) {}
void gx2_render_upload_texture(uint32_t, uint32_t, uint8_t*) {}
void gx2_render_bind_texture(uint32_t, uint32_t, uint8_t*) {}
void gx2_render_copy_to_scan(uint32_t, uint32_t, uint8_t*) {}
void gx2_render_present(void*, bool) {}
void gx2_render_save_fetch_shader(uint32_t, uint32_t, uint32_t) {}
void gx2_render_set_active_fetch_shader(uint32_t) {}
void gx2_render_set_attrib_buffer(uint32_t, uint32_t, uint32_t, uint32_t) {}
void gx2_render_draw(uint32_t, uint32_t, uint32_t, uint8_t*) {}
void gx2_render_draw_indexed(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t*) {}
void gx2_render_set_vertex_shader(uint32_t, uint8_t*) {}
void gx2_render_set_pixel_shader(uint32_t, uint8_t*) {}
void gx2_render_set_uniform_reg(bool, uint32_t, uint32_t, uint32_t, uint8_t*) {}
void gx2_render_set_uniform_block(bool, uint32_t, uint32_t, uint32_t, uint8_t*) {}
#else

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unordered_map>
#include <vector>
#include <cstdlib>
#include "gx2_attrib.h"

// ---------------------------------------------------------------------------
// GL 3.x+ constants (all from the OpenGL spec — no external header needed)
// ---------------------------------------------------------------------------

// Framebuffer targets
static constexpr GLenum GL_FRAMEBUFFER_C         = 0x8D40;
static constexpr GLenum GL_READ_FRAMEBUFFER_C    = 0x8CA8;
static constexpr GLenum GL_DRAW_FRAMEBUFFER_C    = 0x8CA9;
// Framebuffer attachments
static constexpr GLenum GL_COLOR_ATTACHMENT0_C   = 0x8CE0;
static constexpr GLenum GL_DEPTH_ATTACHMENT_C    = 0x8D00;
static constexpr GLenum GL_DEPTH_STENCIL_ATTACH_C= 0x821A;
// Renderbuffer
static constexpr GLenum GL_RENDERBUFFER_C        = 0x8D41;
static constexpr GLenum GL_DEPTH24_STENCIL8_C    = 0x88F0;
static constexpr GLenum GL_DEPTH_COMPONENT32_C   = 0x81A7;
// Framebuffer status
static constexpr GLenum GL_FRAMEBUFFER_COMPLETE_C= 0x8CD5;
// Texture targets / formats
static constexpr GLenum GL_TEXTURE_2D_C          = GL_TEXTURE_2D;
static constexpr GLenum GL_CLAMP_TO_EDGE_C       = 0x812F;
static constexpr GLenum GL_LINEAR_MIPMAP_LINEAR_C= 0x2703;
static constexpr GLenum GL_SRGB8_ALPHA8_C        = 0x8C43;
static constexpr GLenum GL_COMPRESSED_RGBA_DXT1_C= 0x83F1; // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
static constexpr GLenum GL_COMPRESSED_RGBA_DXT3_C= 0x83F2;
static constexpr GLenum GL_COMPRESSED_RGBA_DXT5_C= 0x83F3;
static constexpr GLenum GL_R8_C                  = 0x8229;
static constexpr GLenum GL_R16F_C                = 0x822D;
static constexpr GLenum GL_R32F_C                = 0x822E;
static constexpr GLenum GL_RG16F_C               = 0x822F;
static constexpr GLenum GL_RG32F_C               = 0x8230;
static constexpr GLenum GL_RGBA16F_C             = 0x881A;
static constexpr GLenum GL_RGBA32F_C             = 0x8814;
static constexpr GLenum GL_R11F_G11F_B10F_C      = 0x8C3A;
static constexpr GLenum GL_RG_C                  = 0x8227;
static constexpr GLenum GL_RED_C                 = 0x1903;
// Active texture
static constexpr GLenum GL_TEXTURE0_C            = 0x84C0;
// Blend operations
static constexpr GLenum GL_FUNC_ADD_C            = 0x8006;
static constexpr GLenum GL_FUNC_SUBTRACT_C       = 0x800A;
static constexpr GLenum GL_FUNC_REVERSE_SUBTRACT_C = 0x800B;
static constexpr GLenum GL_MIN_C                 = 0x8007;
static constexpr GLenum GL_MAX_C                 = 0x8008;
// Buffer targets
static constexpr GLenum GL_ARRAY_BUFFER_C        = 0x8892;
static constexpr GLenum GL_ELEMENT_ARRAY_BUFFER_C= 0x8893;
static constexpr GLenum GL_STREAM_DRAW_C         = 0x88E0;

// ---------------------------------------------------------------------------
// GL function pointer types
// ---------------------------------------------------------------------------
typedef void    (APIENTRY *PFN_glGenFramebuffers)          (GLsizei, GLuint*);
typedef void    (APIENTRY *PFN_glDeleteFramebuffers)       (GLsizei, const GLuint*);
typedef void    (APIENTRY *PFN_glBindFramebuffer)          (GLenum, GLuint);
typedef void    (APIENTRY *PFN_glFramebufferTexture2D)     (GLenum, GLenum, GLenum, GLuint, GLint);
typedef void    (APIENTRY *PFN_glFramebufferRenderbuffer)  (GLenum, GLenum, GLenum, GLuint);
typedef GLenum  (APIENTRY *PFN_glCheckFramebufferStatus)   (GLenum);
typedef void    (APIENTRY *PFN_glBlitFramebuffer)          (GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
typedef void    (APIENTRY *PFN_glGenRenderbuffers)         (GLsizei, GLuint*);
typedef void    (APIENTRY *PFN_glDeleteRenderbuffers)      (GLsizei, const GLuint*);
typedef void    (APIENTRY *PFN_glBindRenderbuffer)         (GLenum, GLuint);
typedef void    (APIENTRY *PFN_glRenderbufferStorage)      (GLenum, GLenum, GLsizei, GLsizei);
typedef void    (APIENTRY *PFN_glGenVertexArrays)          (GLsizei, GLuint*);
typedef void    (APIENTRY *PFN_glBindVertexArray)          (GLuint);
typedef void    (APIENTRY *PFN_glActiveTexture)            (GLenum);
typedef void    (APIENTRY *PFN_glGenerateMipmap)           (GLenum);
typedef void    (APIENTRY *PFN_glCompressedTexImage2D)     (GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*);
typedef void    (APIENTRY *PFN_glBlendFuncSeparate)        (GLenum, GLenum, GLenum, GLenum);
typedef void    (APIENTRY *PFN_glBlendEquationSeparate)    (GLenum, GLenum);
typedef GLuint  (APIENTRY *PFN_glCreateShader)             (GLenum);
typedef void    (APIENTRY *PFN_glShaderSource)             (GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void    (APIENTRY *PFN_glCompileShader)            (GLuint);
typedef void    (APIENTRY *PFN_glGetShaderiv)              (GLuint, GLenum, GLint*);
typedef void    (APIENTRY *PFN_glGetShaderInfoLog)         (GLuint, GLsizei, GLsizei*, GLchar*);
typedef GLuint  (APIENTRY *PFN_glCreateProgram)            ();
typedef void    (APIENTRY *PFN_glAttachShader)             (GLuint, GLuint);
typedef void    (APIENTRY *PFN_glLinkProgram)              (GLuint);
typedef void    (APIENTRY *PFN_glUseProgram)               (GLuint);
typedef void    (APIENTRY *PFN_glDeleteShader)             (GLuint);
typedef void    (APIENTRY *PFN_glGetProgramiv)             (GLuint, GLenum, GLint*);
typedef void    (APIENTRY *PFN_glGetProgramInfoLog)        (GLuint, GLsizei, GLsizei*, GLchar*);
// VBO + attrib setup
typedef void    (APIENTRY *PFN_glGenBuffers)               (GLsizei, GLuint*);
typedef void    (APIENTRY *PFN_glDeleteBuffers)            (GLsizei, const GLuint*);
typedef void    (APIENTRY *PFN_glBindBuffer)               (GLenum, GLuint);
typedef void    (APIENTRY *PFN_glBufferData)               (GLenum, GLsizeiptr, const void*, GLenum);
typedef void    (APIENTRY *PFN_glVertexAttribPointer)      (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void    (APIENTRY *PFN_glEnableVertexAttribArray)  (GLuint);
typedef void    (APIENTRY *PFN_glDisableVertexAttribArray) (GLuint);
// UBO support
typedef void    (APIENTRY *PFN_glGenBuffersC)              (GLsizei, GLuint*);
typedef void    (APIENTRY *PFN_glBindBufferBase)           (GLenum, GLuint, GLuint);
typedef void    (APIENTRY *PFN_glBufferSubData)            (GLenum, GLintptr, GLsizeiptr, const void*);

static PFN_glGenFramebuffers          s_GenFBO        = nullptr;
static PFN_glDeleteFramebuffers       s_DelFBO        = nullptr;
static PFN_glBindFramebuffer          s_BindFBO       = nullptr;
static PFN_glFramebufferTexture2D     s_FBOTex        = nullptr;
static PFN_glFramebufferRenderbuffer  s_FBORbo        = nullptr;
static PFN_glCheckFramebufferStatus   s_CheckFBO      = nullptr;
static PFN_glBlitFramebuffer          s_BlitFBO       = nullptr;
static PFN_glGenRenderbuffers         s_GenRBO        = nullptr;
static PFN_glDeleteRenderbuffers      s_DelRBO        = nullptr;
static PFN_glBindRenderbuffer         s_BindRBO       = nullptr;
static PFN_glRenderbufferStorage      s_RBOStorage    = nullptr;
static PFN_glGenVertexArrays          s_GenVAO        = nullptr;
static PFN_glBindVertexArray          s_BindVAO       = nullptr;
static PFN_glActiveTexture            s_ActiveTex     = nullptr;
static PFN_glGenerateMipmap           s_GenMip        = nullptr;
static PFN_glCompressedTexImage2D     s_CompTex       = nullptr;
static PFN_glBlendFuncSeparate        s_BlendFuncSep  = nullptr;
static PFN_glBlendEquationSeparate    s_BlendEqSep    = nullptr;
static PFN_glCreateShader             s_CreateShader  = nullptr;
static PFN_glShaderSource             s_ShaderSrc     = nullptr;
static PFN_glCompileShader            s_CompileShader = nullptr;
static PFN_glGetShaderiv              s_GetShaderiv   = nullptr;
static PFN_glGetShaderInfoLog         s_GetShaderLog  = nullptr;
static PFN_glCreateProgram            s_CreateProg    = nullptr;
static PFN_glAttachShader             s_AttachShader  = nullptr;
static PFN_glLinkProgram              s_LinkProg      = nullptr;
static PFN_glUseProgram               s_UseProg       = nullptr;
static PFN_glDeleteShader             s_DelShader     = nullptr;
static PFN_glGetProgramiv             s_GetProgiv     = nullptr;
static PFN_glGetProgramInfoLog        s_GetProgLog    = nullptr;
static PFN_glGenBuffers               s_GenBuffers    = nullptr;
static PFN_glDeleteBuffers            s_DelBuffers    = nullptr;
static PFN_glBindBuffer               s_BindBuffer    = nullptr;
static PFN_glBufferData               s_BufferData    = nullptr;
static PFN_glVertexAttribPointer      s_VertexAttrib  = nullptr;
static PFN_glEnableVertexAttribArray  s_EnableVA      = nullptr;
static PFN_glDisableVertexAttribArray s_DisableVA     = nullptr;
static PFN_glBindBufferBase           s_BindBufBase   = nullptr;
static PFN_glBufferSubData            s_BufSubData    = nullptr;

#define LOAD(fn, type, name) \
    fn = (type)SDL_GL_GetProcAddress(name); \
    if (!fn) fprintf(stderr, "[gx2_render] WARNING: could not load %s\n", name)

// ---------------------------------------------------------------------------
// Per-color-buffer state
// ---------------------------------------------------------------------------
struct CBEntry {
    GLuint fbo     = 0;  // framebuffer object
    GLuint color   = 0;  // GL colour texture
    uint32_t width = 0;
    uint32_t height= 0;
    GLenum   gl_internal = GL_RGBA8;
    GLenum   gl_format   = GL_RGBA;
    GLenum   gl_type     = GL_UNSIGNED_BYTE;
};

// Per-texture state (keyed by GX2Surface.image guest ptr)
struct TexEntry {
    GLuint  id      = 0;
    uint32_t image  = 0;  // last uploaded image guest ptr (detect changes)
    uint32_t width  = 0;
    uint32_t height = 0;
};

static std::unordered_map<uint32_t, CBEntry>  s_cb_map;   // cb_addr → CBEntry
static std::unordered_map<uint32_t, TexEntry> s_tex_map;  // tex_addr → TexEntry
static std::unordered_map<uint32_t, GLuint>   s_rbo_map;  // db_addr → RBO

static GLuint s_current_fbo    = 0;   // currently-bound FBO for rendering
static GLuint s_scan_tv_fbo    = 0;   // TV scan buffer FBO
static GLuint s_scan_drc_fbo   = 0;   // DRC scan buffer FBO
static GLuint s_scan_tv_tex    = 0;   // colour texture of TV FBO (for blit)
static GLuint s_scan_tv_w      = 0;
static GLuint s_scan_tv_h      = 0;
static GLuint s_scan_drc_w     = 0;  // DRC framebuffer width  (854)
static GLuint s_scan_drc_h     = 0;  // DRC framebuffer height (480)
static GLuint s_dummy_vao      = 0;   // VAO required in core profile
static GLuint s_stub_program   = 0;   // minimal GLSL program (avoids draw errors)

// ---------------------------------------------------------------------------
// Fetch-shader side-table + attrib buffer state
// ---------------------------------------------------------------------------
struct FetchShaderInfo {
    uint32_t attrib_count       = 0;
    uint32_t streams_guest_addr = 0; // guest ptr to GX2AttribStream array
};

struct AttribBufferState {
    uint32_t guest_addr = 0;
    uint32_t size       = 0;
    uint32_t stride     = 0;
    GLuint   vbo        = 0;
};

static std::unordered_map<uint32_t, FetchShaderInfo> s_fetch_shader_map;
static AttribBufferState s_attrib_buffers[32];
static uint32_t s_active_fetch_shader = 0;
static GLuint   s_draw_vao    = 0; // single persistent VAO for game draw calls
static GLuint   s_draw_ebo    = 0; // persistent EBO for indexed draws

// ---------------------------------------------------------------------------
// Shader program cache + UBO state
// ---------------------------------------------------------------------------
// UBO constants: 256 × vec4 = 4096 bytes per stage
static constexpr uint32_t UBO_VEC4_COUNT = 256u;
static constexpr uint32_t UBO_BYTE_SIZE  = UBO_VEC4_COUNT * 16u;

// UBO binding points must match the GLSL layout(binding=N) declarations in
// r700_to_glsl.cpp: VS → binding=0, PS → binding=1.
static constexpr GLenum  GL_UNIFORM_BUFFER_C = 0x8A11;

static GLuint   s_vs_ubo = 0;   // vertex shader UBO
static GLuint   s_ps_ubo = 0;   // pixel shader UBO
static float    s_vs_constants[UBO_VEC4_COUNT * 4]; // host-side mirror
static float    s_ps_constants[UBO_VEC4_COUNT * 4];

// Compiled shader programs: keyed by (vs_guest_addr | (uint64_t)ps_guest_addr << 32)
static std::unordered_map<uint64_t, GLuint> s_prog_cache;
static uint32_t s_cur_vs_addr    = 0;  // currently-bound VS guest addr
static uint32_t s_cur_ps_addr    = 0;  // currently-bound PS guest addr
static uint32_t s_cur_vs_attribs = 0;  // attrib count of current VS
static GLuint   s_active_program = 0;  // currently UseProgram'd compiled prog

// ---------------------------------------------------------------------------
// GX2SurfaceFormat → OpenGL format info
// ---------------------------------------------------------------------------
struct GLFmtInfo {
    GLint  internal;
    GLenum format;
    GLenum type;
    uint32_t bpp;          // 0 = compressed
    bool   is_depth;
    bool   is_compressed;
    uint32_t block_size;   // compressed bytes per 4x4 block (0 if uncompressed)
};

static GLFmtInfo gx2_gl_format(uint32_t gx2_fmt) {
    switch (gx2_fmt) {
    case GX2FMT_UNORM_R8:
        return { (GLint)GL_R8_C,          (GLenum)GL_RED_C,  GL_UNSIGNED_BYTE,  1, false, false, 0 };
    case GX2FMT_UNORM_R8_G8:
        return { (GLint)GL_RG8,           (GLenum)GL_RG_C,   GL_UNSIGNED_BYTE,  2, false, false, 0 };
    case GX2FMT_UNORM_R8_G8_B8_A8:
        return { (GLint)GL_RGBA8,         GL_RGBA,           GL_UNSIGNED_BYTE,  4, false, false, 0 };
    case GX2FMT_SRGB_R8_G8_B8_A8:
        return { (GLint)GL_SRGB8_ALPHA8_C,GL_RGBA,           GL_UNSIGNED_BYTE,  4, false, false, 0 };
    case GX2FMT_UNORM_R5_G6_B5:
        // Big-endian swap handled by GL_UNSIGNED_SHORT_5_6_5_REV on LE host
        return { (GLint)GL_RGB,           GL_RGB,            GL_UNSIGNED_SHORT_5_6_5, 2, false, false, 0 };
    case GX2FMT_UNORM_BC1:
        return { (GLint)GL_COMPRESSED_RGBA_DXT1_C, GL_RGBA,  GL_UNSIGNED_BYTE, 0, false, true, 8 };
    case GX2FMT_UNORM_BC2:
        return { (GLint)GL_COMPRESSED_RGBA_DXT3_C, GL_RGBA,  GL_UNSIGNED_BYTE, 0, false, true, 16 };
    case GX2FMT_UNORM_BC3:
        return { (GLint)GL_COMPRESSED_RGBA_DXT5_C, GL_RGBA,  GL_UNSIGNED_BYTE, 0, false, true, 16 };
    case GX2FMT_FLOAT_D24_S8:
        return { (GLint)GL_DEPTH24_STENCIL8_C,GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, true, false, 0 };
    case GX2FMT_FLOAT_D32:
        return { (GLint)GL_DEPTH_COMPONENT32_C,GL_DEPTH_COMPONENT, GL_FLOAT,         4, true, false, 0 };
    case GX2FMT_FLOAT_R11_G11_B10:
        return { (GLint)GL_R11F_G11F_B10F_C, GL_RGB,         GL_FLOAT,             4, false, false, 0 };
    case GX2FMT_FLOAT_R16:
        return { (GLint)GL_R16F_C,           (GLenum)GL_RED_C,GL_FLOAT,             2, false, false, 0 };
    case GX2FMT_FLOAT_R16_G16:
        return { (GLint)GL_RG16F_C,          (GLenum)GL_RG_C, GL_FLOAT,             4, false, false, 0 };
    case GX2FMT_FLOAT_R16_G16_B16_A16:
        return { (GLint)GL_RGBA16F_C,        GL_RGBA,         GL_FLOAT,             8, false, false, 0 };
    case GX2FMT_FLOAT_R32:
        return { (GLint)GL_R32F_C,           (GLenum)GL_RED_C,GL_FLOAT,             4, false, false, 0 };
    case GX2FMT_FLOAT_R32_G32_B32_A32:
        return { (GLint)GL_RGBA32F_C,        GL_RGBA,         GL_FLOAT,            16, false, false, 0 };
    default:
        // Unknown format — fall back to RGBA8
        fprintf(stderr, "[gx2_render] unknown GX2SurfaceFormat 0x%04X, using RGBA8\n", gx2_fmt);
        return { (GLint)GL_RGBA8,            GL_RGBA,          GL_UNSIGNED_BYTE,  4, false, false, 0 };
    }
}

// ---------------------------------------------------------------------------
// Stub shader (renders nothing but prevents GL errors)
// ---------------------------------------------------------------------------
static const char* STUB_VERT_SRC =
    "#version 410 core\n"
    "void main() { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); }\n"; // behind clip plane

static const char* STUB_FRAG_SRC =
    "#version 410 core\n"
    "out vec4 color;\n"
    "void main() { color = vec4(1.0, 0.0, 1.0, 1.0); }\n"; // magenta stub

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = s_CreateShader(type);
    s_ShaderSrc(s, 1, &src, nullptr);
    s_CompileShader(s);
    GLint ok = 0;
    s_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; s_GetShaderLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "[gx2_render] shader compile error: %s\n", log);
    }
    return s;
}

static GLuint create_stub_program() {
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   STUB_VERT_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, STUB_FRAG_SRC);
    GLuint p  = s_CreateProg();
    s_AttachShader(p, vs);
    s_AttachShader(p, fs);
    s_LinkProg(p);
    GLint ok = 0;
    s_GetProgiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; s_GetProgLog(p, sizeof(log), nullptr, log);
        fprintf(stderr, "[gx2_render] stub program link error: %s\n", log);
    }
    s_DelShader(vs);
    s_DelShader(fs);
    return p;
}

// ---------------------------------------------------------------------------
// GX2CompareFunction → GL depth/stencil enum
// ---------------------------------------------------------------------------
static GLenum gx2_compare_to_gl(uint32_t f) {
    static const GLenum tbl[] = {
        GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
        GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS
    };
    return (f < 8) ? tbl[f] : GL_ALWAYS;
}

// ---------------------------------------------------------------------------
// GX2BlendMode → GL blend factor enum
// ---------------------------------------------------------------------------
static GLenum gx2_blend_factor(uint32_t m) {
    switch (m) {
    case  0: return GL_ZERO;
    case  1: return GL_ONE;
    case  2: return GL_SRC_COLOR;
    case  3: return GL_ONE_MINUS_SRC_COLOR;
    case  4: return GL_SRC_ALPHA;
    case  5: return GL_ONE_MINUS_SRC_ALPHA;
    case  6: return GL_DST_ALPHA;
    case  7: return GL_ONE_MINUS_DST_ALPHA;
    case  8: return GL_DST_COLOR;
    case  9: return GL_ONE_MINUS_DST_COLOR;
    case 10: return GL_SRC_ALPHA_SATURATE;
    case 13: return GL_CONSTANT_ALPHA;
    case 14: return GL_ONE_MINUS_CONSTANT_ALPHA;
    case 19: return GL_CONSTANT_COLOR;
    case 20: return GL_ONE_MINUS_CONSTANT_COLOR;
    default: return GL_ONE;
    }
}

static GLenum gx2_blend_equation(uint32_t c) {
    switch (c) {
    case 0: return (GLenum)GL_FUNC_ADD_C;
    case 1: return (GLenum)GL_FUNC_SUBTRACT_C;
    case 2: return (GLenum)GL_FUNC_REVERSE_SUBTRACT_C;
    case 3: return (GLenum)GL_MIN_C;
    case 4: return (GLenum)GL_MAX_C;
    default: return (GLenum)GL_FUNC_ADD_C;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void gx2_render_init() {
    // Load all GL 3.x+ functions
    LOAD(s_GenFBO,       PFN_glGenFramebuffers,         "glGenFramebuffers");
    LOAD(s_DelFBO,       PFN_glDeleteFramebuffers,      "glDeleteFramebuffers");
    LOAD(s_BindFBO,      PFN_glBindFramebuffer,         "glBindFramebuffer");
    LOAD(s_FBOTex,       PFN_glFramebufferTexture2D,    "glFramebufferTexture2D");
    LOAD(s_FBORbo,       PFN_glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    LOAD(s_CheckFBO,     PFN_glCheckFramebufferStatus,  "glCheckFramebufferStatus");
    LOAD(s_BlitFBO,      PFN_glBlitFramebuffer,         "glBlitFramebuffer");
    LOAD(s_GenRBO,       PFN_glGenRenderbuffers,        "glGenRenderbuffers");
    LOAD(s_DelRBO,       PFN_glDeleteRenderbuffers,     "glDeleteRenderbuffers");
    LOAD(s_BindRBO,      PFN_glBindRenderbuffer,        "glBindRenderbuffer");
    LOAD(s_RBOStorage,   PFN_glRenderbufferStorage,     "glRenderbufferStorage");
    LOAD(s_GenVAO,       PFN_glGenVertexArrays,         "glGenVertexArrays");
    LOAD(s_BindVAO,      PFN_glBindVertexArray,         "glBindVertexArray");
    LOAD(s_ActiveTex,    PFN_glActiveTexture,           "glActiveTexture");
    LOAD(s_GenMip,       PFN_glGenerateMipmap,          "glGenerateMipmap");
    LOAD(s_CompTex,      PFN_glCompressedTexImage2D,    "glCompressedTexImage2D");
    LOAD(s_BlendFuncSep, PFN_glBlendFuncSeparate,       "glBlendFuncSeparate");
    LOAD(s_BlendEqSep,   PFN_glBlendEquationSeparate,   "glBlendEquationSeparate");
    LOAD(s_CreateShader, PFN_glCreateShader,            "glCreateShader");
    LOAD(s_ShaderSrc,    PFN_glShaderSource,            "glShaderSource");
    LOAD(s_CompileShader,PFN_glCompileShader,           "glCompileShader");
    LOAD(s_GetShaderiv,  PFN_glGetShaderiv,             "glGetShaderiv");
    LOAD(s_GetShaderLog, PFN_glGetShaderInfoLog,        "glGetShaderInfoLog");
    LOAD(s_CreateProg,   PFN_glCreateProgram,           "glCreateProgram");
    LOAD(s_AttachShader, PFN_glAttachShader,            "glAttachShader");
    LOAD(s_LinkProg,     PFN_glLinkProgram,             "glLinkProgram");
    LOAD(s_UseProg,      PFN_glUseProgram,              "glUseProgram");
    LOAD(s_DelShader,    PFN_glDeleteShader,            "glDeleteShader");
    LOAD(s_GetProgiv,    PFN_glGetProgramiv,            "glGetProgramiv");
    LOAD(s_GetProgLog,   PFN_glGetProgramInfoLog,       "glGetProgramInfoLog");
    // VBO + attrib functions
    LOAD(s_GenBuffers,   PFN_glGenBuffers,              "glGenBuffers");
    LOAD(s_DelBuffers,   PFN_glDeleteBuffers,           "glDeleteBuffers");
    LOAD(s_BindBuffer,   PFN_glBindBuffer,              "glBindBuffer");
    LOAD(s_BufferData,   PFN_glBufferData,              "glBufferData");
    LOAD(s_VertexAttrib, PFN_glVertexAttribPointer,     "glVertexAttribPointer");
    LOAD(s_EnableVA,     PFN_glEnableVertexAttribArray, "glEnableVertexAttribArray");
    LOAD(s_DisableVA,    PFN_glDisableVertexAttribArray,"glDisableVertexAttribArray");
    // UBO
    LOAD(s_BindBufBase,  PFN_glBindBufferBase,          "glBindBufferBase");
    LOAD(s_BufSubData,   PFN_glBufferSubData,           "glBufferSubData");

    // Core-profile VAO required for any draw call
    if (s_GenVAO && s_BindVAO) {
        s_GenVAO(1, &s_dummy_vao);
        s_BindVAO(s_dummy_vao);
        // Separate VAO for game draw calls (so we can configure attribs freely)
        s_GenVAO(1, &s_draw_vao);
    }
    // EBO for indexed draws
    if (s_GenBuffers) {
        s_GenBuffers(1, &s_draw_ebo);
    }

    // Compile stub program (keeps GL pipeline valid before real shaders land)
    if (s_CreateShader && s_CreateProg) {
        s_stub_program = create_stub_program();
        if (s_UseProg) s_UseProg(s_stub_program);
    }

    // Allocate UBOs for VS and PS constant banks
    if (s_GenBuffers && s_BindBufBase && s_BufSubData) {
        memset(s_vs_constants, 0, sizeof(s_vs_constants));
        memset(s_ps_constants, 0, sizeof(s_ps_constants));

        s_GenBuffers(1, &s_vs_ubo);
        s_BindBuffer((GLenum)GL_UNIFORM_BUFFER_C, s_vs_ubo);
        s_BufferData((GLenum)GL_UNIFORM_BUFFER_C, (GLsizeiptr)UBO_BYTE_SIZE,
                     s_vs_constants, (GLenum)GL_STREAM_DRAW_C);
        s_BindBufBase((GLenum)GL_UNIFORM_BUFFER_C, 0u, s_vs_ubo);

        s_GenBuffers(1, &s_ps_ubo);
        s_BindBuffer((GLenum)GL_UNIFORM_BUFFER_C, s_ps_ubo);
        s_BufferData((GLenum)GL_UNIFORM_BUFFER_C, (GLsizeiptr)UBO_BYTE_SIZE,
                     s_ps_constants, (GLenum)GL_STREAM_DRAW_C);
        s_BindBufBase((GLenum)GL_UNIFORM_BUFFER_C, 1u, s_ps_ubo);
    }

    // Default GL state to match Wii U defaults
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    fprintf(stderr, "[gx2_render] GL rendering back-end initialised\n");
}

void gx2_render_set_color_buffer(uint32_t cb_addr, uint8_t* mem) {
    fprintf(stderr, "[gx2_render] GX2SetColorBuffer cb=0x%08X\n", cb_addr);
    if (!cb_addr || !s_GenFBO) return;

    auto it = s_cb_map.find(cb_addr);
    if (it == s_cb_map.end()) {
        // Read surface from guest memory
        uint32_t surf = cb_addr + GX2CB_SURFACE;
        uint32_t w    = rbrew_read32(mem, surf + GX2SURF_WIDTH);
        uint32_t h    = rbrew_read32(mem, surf + GX2SURF_HEIGHT);
        uint32_t fmt  = rbrew_read32(mem, surf + GX2SURF_FORMAT);
        if (w == 0) w = 1920;
        if (h == 0) h = 1080;
        auto glfmt    = gx2_gl_format(fmt);

        CBEntry e;
        e.width      = w;
        e.height     = h;
        e.gl_internal= glfmt.internal;
        e.gl_format  = glfmt.format;
        e.gl_type    = glfmt.type;

        // Create colour texture
        glGenTextures(1, &e.color);
        glBindTexture(GL_TEXTURE_2D, e.color);
        glTexImage2D(GL_TEXTURE_2D, 0, e.gl_internal,
                     (GLsizei)w, (GLsizei)h, 0,
                     e.gl_format, e.gl_type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE_C);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE_C);

        // Create FBO and attach
        s_GenFBO(1, &e.fbo);
        s_BindFBO(GL_FRAMEBUFFER_C, e.fbo);
        s_FBOTex(GL_FRAMEBUFFER_C, GL_COLOR_ATTACHMENT0_C,
                 GL_TEXTURE_2D, e.color, 0);

        s_cb_map[cb_addr] = e;
        it = s_cb_map.find(cb_addr);
        fprintf(stderr, "[gx2_render] created FBO %u for CB 0x%08X (%ux%u fmt=0x%04X)\n",
                e.fbo, cb_addr, w, h, fmt);
    }

    s_BindFBO(GL_FRAMEBUFFER_C, it->second.fbo);
    s_current_fbo = it->second.fbo;
}

void gx2_render_set_depth_buffer(uint32_t db_addr, uint8_t* mem) {
    if (!db_addr || !s_GenRBO || s_current_fbo == 0) return;

    auto it = s_rbo_map.find(db_addr);
    if (it == s_rbo_map.end()) {
        // Read surface dimensions
        uint32_t surf = db_addr + GX2DB_SURFACE;
        uint32_t w    = rbrew_read32(mem, surf + GX2SURF_WIDTH);
        uint32_t h    = rbrew_read32(mem, surf + GX2SURF_HEIGHT);
        if (w == 0) w = 1920;
        if (h == 0) h = 1080;

        GLuint rbo;
        s_GenRBO(1, &rbo);
        s_BindRBO(GL_RENDERBUFFER_C, rbo);
        s_RBOStorage(GL_RENDERBUFFER_C, GL_DEPTH24_STENCIL8_C,
                     (GLsizei)w, (GLsizei)h);
        s_rbo_map[db_addr] = rbo;
        it = s_rbo_map.find(db_addr);
        fprintf(stderr, "[gx2_render] created depth RBO %u for DB 0x%08X (%ux%u)\n",
                rbo, db_addr, w, h);
    }

    // Attach depth+stencil to current FBO
    s_BindFBO(GL_FRAMEBUFFER_C, s_current_fbo);
    s_FBORbo(GL_FRAMEBUFFER_C, GL_DEPTH_STENCIL_ATTACH_C,
             GL_RENDERBUFFER_C, it->second);
}

void gx2_render_clear_color(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

void gx2_render_clear_depth_stencil(float depth, uint8_t stencil,
                                    uint32_t clear_mode) {
    GLbitfield mask = 0;
    if (clear_mode & GX2_CLEAR_FLAGS_DEPTH)   { glClearDepth(depth);   mask |= GL_DEPTH_BUFFER_BIT; }
    if (clear_mode & GX2_CLEAR_FLAGS_STENCIL) { glClearStencil(stencil); mask |= GL_STENCIL_BUFFER_BIT; }
    if (mask) glClear(mask);
}

void gx2_render_clear_buffers(float r, float g, float b, float a,
                               float depth, uint8_t stencil) {
    glClearColor(r, g, b, a);
    glClearDepth(depth);
    glClearStencil(stencil);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

void gx2_render_set_viewport(float x, float y, float w, float h,
                              float near_z, float far_z) {
    glViewport((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
    glDepthRange((GLdouble)near_z, (GLdouble)far_z);
}

void gx2_render_set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        glEnable(GL_SCISSOR_TEST);
        glScissor((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
    }
}

void gx2_render_set_depth_stencil_control(bool depth_test, bool depth_write,
                                          uint32_t gx2_cmp) {
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gx2_compare_to_gl(gx2_cmp));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(depth_write ? GL_TRUE : GL_FALSE);
}

void gx2_render_set_polygon_control(uint32_t front_face,
                                    bool cull_front, bool cull_back) {
    glFrontFace((front_face == GX2_FRONT_FACE_CW) ? GL_CW : GL_CCW);
    if (cull_front && cull_back) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT_AND_BACK);
    } else if (cull_back) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else if (cull_front) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
    } else {
        glDisable(GL_CULL_FACE);
    }
}

void gx2_render_set_blend_control(bool blend_enable,
                                  uint32_t src_color, uint32_t dst_color,
                                  uint32_t color_combine,
                                  uint32_t src_alpha, uint32_t dst_alpha,
                                  uint32_t alpha_combine) {
    if (blend_enable && s_BlendFuncSep && s_BlendEqSep) {
        glEnable(GL_BLEND);
        s_BlendFuncSep(gx2_blend_factor(src_color), gx2_blend_factor(dst_color),
                       gx2_blend_factor(src_alpha),  gx2_blend_factor(dst_alpha));
        s_BlendEqSep(gx2_blend_equation(color_combine),
                     gx2_blend_equation(alpha_combine));
    } else {
        glDisable(GL_BLEND);
    }
}

void gx2_render_upload_texture(uint32_t tex_addr, uint32_t slot, uint8_t* mem) {
    if (!tex_addr || !s_ActiveTex) return;

    auto& entry = s_tex_map[tex_addr];

    // Read surface info
    uint32_t surf    = tex_addr + GX2TEX_SURFACE;
    uint32_t w       = rbrew_read32(mem, surf + GX2SURF_WIDTH);
    uint32_t h       = rbrew_read32(mem, surf + GX2SURF_HEIGHT);
    uint32_t fmt     = rbrew_read32(mem, surf + GX2SURF_FORMAT);
    uint32_t img_ptr = rbrew_read32(mem, surf + GX2SURF_IMAGE);
    uint32_t img_sz  = rbrew_read32(mem, surf + GX2SURF_IMAGE_SIZE);

    if (w == 0 || h == 0) { return; }

    // Allocate GL texture if new
    if (entry.id == 0) {
        glGenTextures(1, &entry.id);
    }

    s_ActiveTex(GL_TEXTURE0_C + slot);
    glBindTexture(GL_TEXTURE_2D, entry.id);

    // Re-upload only if image pointer or size changed (or first time)
    if (img_ptr != 0 && img_ptr != entry.image) {
        entry.image  = img_ptr;
        entry.width  = w;
        entry.height = h;

        auto glfmt   = gx2_gl_format(fmt);
        const uint8_t* pixels = (img_ptr < 0xC0000000u) ? (mem + img_ptr) : nullptr;

        if (!glfmt.is_depth) {
            if (glfmt.is_compressed && s_CompTex && pixels) {
                // Compressed block size: ceil(w/4)*ceil(h/4)*block_size_bytes
                uint32_t bw = (w + 3) / 4;
                uint32_t bh = (h + 3) / 4;
                uint32_t data_size = bw * bh * glfmt.block_size;
                if (img_sz > 0 && data_size > img_sz) data_size = img_sz;
                s_CompTex(GL_TEXTURE_2D, 0, (GLenum)glfmt.internal,
                          (GLsizei)w, (GLsizei)h, 0,
                          (GLsizei)data_size, pixels);
            } else if (!glfmt.is_compressed) {
                glTexImage2D(GL_TEXTURE_2D, 0, glfmt.internal,
                             (GLsizei)w, (GLsizei)h, 0,
                             glfmt.format, glfmt.type, pixels);
            }
        } else {
            // Depth textures: just allocate without pixel data
            glTexImage2D(GL_TEXTURE_2D, 0, glfmt.internal,
                         (GLsizei)w, (GLsizei)h, 0,
                         glfmt.format, glfmt.type, nullptr);
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)GL_CLAMP_TO_EDGE_C);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)GL_CLAMP_TO_EDGE_C);
    }
}

void gx2_render_bind_texture(uint32_t tex_addr, uint32_t slot, uint8_t* mem) {
    gx2_render_upload_texture(tex_addr, slot, mem);
}

void gx2_render_copy_to_scan(uint32_t cb_addr, uint32_t scan_target,
                              uint8_t* mem) {
    fprintf(stderr, "[gx2_render] GX2CopyColorBufferToScanBuffer cb=0x%08X target=%u fbo_map_size=%zu\n",
            cb_addr, scan_target, s_cb_map.size());
    auto it = s_cb_map.find(cb_addr);
    if (it == s_cb_map.end()) return;

    // scan_target: 1 = TV, 4 = DRC
    if (scan_target == 1 || scan_target == 0) {
        s_scan_tv_fbo = it->second.fbo;
        s_scan_tv_tex = it->second.color;
        s_scan_tv_w   = it->second.width;
        s_scan_tv_h   = it->second.height;
    } else {
        s_scan_drc_fbo = it->second.fbo;
        s_scan_drc_w   = it->second.width;
        s_scan_drc_h   = it->second.height;
    }
}

void gx2_render_present(void* sdl_window, bool gamepad_mode) {
    GLuint src_fbo = gamepad_mode ? s_scan_drc_fbo : s_scan_tv_fbo;
    // Source dimensions depend on which view is active
    GLint src_w = (GLint)(gamepad_mode ? s_scan_drc_w : s_scan_tv_w);
    GLint src_h = (GLint)(gamepad_mode ? s_scan_drc_h : s_scan_tv_h);

    if (src_fbo && s_BlitFBO && src_w > 0 && src_h > 0) {
        // Blit game FBO → default framebuffer
        s_BindFBO(GL_READ_FRAMEBUFFER_C, src_fbo);
        s_BindFBO(GL_DRAW_FRAMEBUFFER_C, 0);

        int ww = 0, wh = 0;
        SDL_GetWindowSize((SDL_Window*)sdl_window, &ww, &wh);
        s_BlitFBO(0, 0, src_w, src_h,
                  0, 0, ww, wh,
                  GL_COLOR_BUFFER_BIT, GL_LINEAR);

        // Restore for next frame
        s_BindFBO(GL_FRAMEBUFFER_C, s_current_fbo);
    }
    SDL_GL_SwapWindow((SDL_Window*)sdl_window);
}

// ---------------------------------------------------------------------------
// Primitive type conversion
// ---------------------------------------------------------------------------
// GX2PrimitiveMode enum values (AMD R600-derived):
//   0x01 POINTS, 0x02 LINES, 0x03 LINE_STRIP, 0x04 TRIANGLES,
//   0x05 TRIANGLE_FAN, 0x06 TRIANGLE_STRIP, 0x0D LINE_LOOP,
//   0x11 RECTS, 0x13 QUADS, 0x14 QUAD_STRIP
static GLenum gx2_prim_to_gl(uint32_t prim) {
    switch (prim) {
    case 0x01: return GL_POINTS;
    case 0x02: return GL_LINES;
    case 0x03: return GL_LINE_STRIP;
    case 0x0D: return GL_LINE_LOOP;
    case 0x04: return GL_TRIANGLES;
    case 0x05: return GL_TRIANGLE_FAN;
    case 0x06: return GL_TRIANGLE_STRIP;
    // RECTS / QUADS / QUAD_STRIP — approximate; proper conversion
    case 0x11:
    case 0x13:
    case 0x14: return GL_TRIANGLES;
    default:   return GL_TRIANGLES;
    }
}

// Apply in-place byte swap of 'component_bytes'-sized words within [buf, buf+size)
static void apply_endian_swap(uint8_t* buf, uint32_t size, uint32_t component_bytes) {
    if (component_bytes == 4) {
        for (uint32_t i = 0; i + 3 < size; i += 4) {
            uint8_t a = buf[i], b = buf[i+1], c = buf[i+2], d = buf[i+3];
            buf[i] = d; buf[i+1] = c; buf[i+2] = b; buf[i+3] = a;
        }
    } else if (component_bytes == 2) {
        for (uint32_t i = 0; i + 1 < size; i += 2) {
            uint8_t a = buf[i]; buf[i] = buf[i+1]; buf[i+1] = a;
        }
    }
}

// Determine the effective endian-swap for an attrib stream entry.
// DEFAULT (0) is resolved based on the format's component size.
static uint32_t resolve_endian_swap(uint32_t endian_swap, uint32_t fmt) {
    if (endian_swap != GX2_ENDIAN_SWAP_DEFAULT) return endian_swap;
    GX2AttribGL gl = gx2_attrib_format_to_gl(fmt);
    if (gl.component_bytes == 4) return GX2_ENDIAN_SWAP_8_IN_32;
    if (gl.component_bytes == 2) return GX2_ENDIAN_SWAP_8_IN_16;
    return GX2_ENDIAN_SWAP_NONE;
}

// Configure VAO attrib pointers from the active fetch shader and upload VBOs
// (with per-buffer endian-swap) from guest memory.
// Returns false if there is no active fetch shader or the VAO functions are missing.
static bool gx2_setup_vertex_attribs(uint8_t* mem) {
    if (!s_BindVAO || !s_draw_vao || !s_GenBuffers) return false;

    auto it = s_fetch_shader_map.find(s_active_fetch_shader);
    if (it == s_fetch_shader_map.end()) return true; // no fetch shader set, skip

    const FetchShaderInfo& fsi = it->second;
    uint32_t attrib_count = fsi.attrib_count;
    uint32_t streams_addr = fsi.streams_guest_addr;
    if (attrib_count == 0 || streams_addr == 0) return true;

    // --- Pass 1: determine per-buffer endian-swap needs ---
    // If different attribs in the same buffer require different swaps,
    // the larger (32-bit) swap wins as it is the most common case.
    uint32_t buf_swap[32] = {};
    for (uint32_t i = 0; i < attrib_count && i < 64; i++) {
        uint32_t stream_base = streams_addr + i * 0x20u;
        if (stream_base + 0x20u > 0xC0000000u) break;
        uint32_t buf_slot   = rbrew_read32(mem, stream_base + 0x04u);
        uint32_t fmt        = rbrew_read32(mem, stream_base + 0x0Cu);
        uint32_t endian_sw  = rbrew_read32(mem, stream_base + 0x1Cu);
        uint32_t resolved   = resolve_endian_swap(endian_sw, fmt);
        if (buf_slot < 32 && resolved > buf_swap[buf_slot])
            buf_swap[buf_slot] = resolved;
    }

    // --- Pass 2: upload each referenced buffer to its VBO ---
    for (uint32_t slot = 0; slot < 32; slot++) {
        AttribBufferState& ab = s_attrib_buffers[slot];
        if (ab.guest_addr == 0 || ab.size == 0) continue;
        if (ab.guest_addr + ab.size > 0xC0000000u) continue;

        if (ab.vbo == 0) s_GenBuffers(1, &ab.vbo);
        s_BindBuffer(GL_ARRAY_BUFFER_C, ab.vbo);

        uint32_t swap = buf_swap[slot];
        if (swap == GX2_ENDIAN_SWAP_8_IN_32 || swap == GX2_ENDIAN_SWAP_8_IN_16) {
            std::vector<uint8_t> tmp(mem + ab.guest_addr,
                                     mem + ab.guest_addr + ab.size);
            uint32_t comp_bytes = (swap == GX2_ENDIAN_SWAP_8_IN_32) ? 4u : 2u;
            apply_endian_swap(tmp.data(), ab.size, comp_bytes);
            s_BufferData(GL_ARRAY_BUFFER_C, (GLsizeiptr)ab.size,
                         tmp.data(), (GLenum)GL_STREAM_DRAW_C);
        } else {
            s_BufferData(GL_ARRAY_BUFFER_C, (GLsizeiptr)ab.size,
                         mem + ab.guest_addr, (GLenum)GL_STREAM_DRAW_C);
        }
    }

    // --- Pass 3: bind draw VAO and configure attrib pointers ---
    s_BindVAO(s_draw_vao);
    for (uint32_t i = 0; i < attrib_count && i < 64; i++) {
        uint32_t stream_base = streams_addr + i * 0x20u;
        if (stream_base + 0x20u > 0xC0000000u) break;
        uint32_t location = rbrew_read32(mem, stream_base + 0x00u);
        uint32_t buf_slot = rbrew_read32(mem, stream_base + 0x04u);
        uint32_t offset   = rbrew_read32(mem, stream_base + 0x08u);
        uint32_t fmt      = rbrew_read32(mem, stream_base + 0x0Cu);
        if (buf_slot >= 32) continue;

        AttribBufferState& ab = s_attrib_buffers[buf_slot];
        if (ab.vbo == 0) continue;

        GX2AttribGL gl = gx2_attrib_format_to_gl(fmt);
        s_BindBuffer(GL_ARRAY_BUFFER_C, ab.vbo);
        s_VertexAttrib(location,
                       (GLint)gl.components,
                       (GLenum)gl.gl_type,
                       (GLboolean)gl.normalized,
                       (GLsizei)ab.stride,
                       (const void*)(uintptr_t)offset);
        s_EnableVA(location);
    }
    return true;
}

// Forward declarations for helpers used by draw functions
static GLuint resolve_active_program(uint8_t* mem);
static void   bind_ubos_to_program(GLuint prog);

// ---------------------------------------------------------------------------
// Public API implementations
// ---------------------------------------------------------------------------

void gx2_render_save_fetch_shader(uint32_t fs_addr,
                                  uint32_t attrib_count,
                                  uint32_t streams_guest_addr) {
    if (!fs_addr) return;
    s_fetch_shader_map[fs_addr] = { attrib_count, streams_guest_addr };
}

void gx2_render_set_active_fetch_shader(uint32_t fs_addr) {
    s_active_fetch_shader = fs_addr;
}

void gx2_render_set_attrib_buffer(uint32_t slot,
                                  uint32_t guest_addr,
                                  uint32_t size,
                                  uint32_t stride) {
    if (slot >= 32) return;
    AttribBufferState& ab = s_attrib_buffers[slot];
    ab.guest_addr = guest_addr;
    ab.size       = size;
    ab.stride     = stride;
    // VBO will be (re-)uploaded at draw time so we don't need to do it now.
}

void gx2_render_draw(uint32_t prim_type,
                     uint32_t count,
                     uint32_t first_vertex,
                     uint8_t* mem) {
    if (!s_UseProg) return;
    GLuint prog = resolve_active_program(mem);
    if (!prog) return;
    s_UseProg(prog);
    bind_ubos_to_program(prog);
    gx2_setup_vertex_attribs(mem);
    glDrawArrays(gx2_prim_to_gl(prim_type),
                 (GLint)first_vertex,
                 (GLsizei)count);
    // Restore dummy VAO so texture / FBO operations don't accidentally modify s_draw_vao
    if (s_BindVAO && s_dummy_vao) s_BindVAO(s_dummy_vao);
}

void gx2_render_draw_indexed(uint32_t prim_type,
                             uint32_t count,
                             uint32_t index_type,
                             uint32_t idx_guest_addr,
                             uint32_t first_vertex,
                             uint8_t* mem) {
    if (!s_UseProg || !s_draw_ebo) return;
    if (idx_guest_addr == 0 || idx_guest_addr + count * 4 > 0xC0000000u) return;

    // Determine index size and whether big-endian swap is needed
    bool is_u32   = (index_type == GX2_INDEX_TYPE_U32_BE ||
                     index_type == GX2_INDEX_TYPE_U32_LE);
    bool need_swap= (index_type == GX2_INDEX_TYPE_U16_BE ||
                     index_type == GX2_INDEX_TYPE_U32_BE);
    uint32_t idx_bytes = count * (is_u32 ? 4u : 2u);

    s_BindBuffer(GL_ELEMENT_ARRAY_BUFFER_C, s_draw_ebo);
    if (need_swap) {
        std::vector<uint8_t> tmp(mem + idx_guest_addr,
                                 mem + idx_guest_addr + idx_bytes);
        apply_endian_swap(tmp.data(), idx_bytes, is_u32 ? 4u : 2u);
        s_BufferData(GL_ELEMENT_ARRAY_BUFFER_C, (GLsizeiptr)idx_bytes,
                     tmp.data(), (GLenum)GL_STREAM_DRAW_C);
    } else {
        s_BufferData(GL_ELEMENT_ARRAY_BUFFER_C, (GLsizeiptr)idx_bytes,
                     mem + idx_guest_addr, (GLenum)GL_STREAM_DRAW_C);
    }

    {
        GLuint prog = resolve_active_program(mem);
        if (!prog) return;
        s_UseProg(prog);
        bind_ubos_to_program(prog);
    }
    gx2_setup_vertex_attribs(mem);

    GLenum idx_gl_type = is_u32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
    (void)first_vertex;
    glDrawElements(gx2_prim_to_gl(prim_type),
                   (GLsizei)count,
                   idx_gl_type,
                   nullptr);
    if (s_BindVAO && s_dummy_vao) s_BindVAO(s_dummy_vao);
}

// ---------------------------------------------------------------------------
// Helper — compile and link one translated GLSL program
// ---------------------------------------------------------------------------
static GLuint compile_translated_program(const char* vs_src, const char* ps_src) {
    if (!s_CreateShader || !s_CreateProg) return 0;

    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, ps_src);
    GLuint p  = s_CreateProg();
    s_AttachShader(p, vs);
    s_AttachShader(p, fs);
    s_LinkProg(p);
    GLint ok = 0;
    s_GetProgiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; s_GetProgLog(p, sizeof(log), nullptr, log);
        fprintf(stderr, "[gx2_render] translated program link error: %s\n", log);
        s_DelShader(vs); s_DelShader(fs);
        return 0;
    }
    s_DelShader(vs);
    s_DelShader(fs);
    return p;
}

// Bind UBOs to the currently active program's uniform block indices.
static void bind_ubos_to_program(GLuint prog) {
    if (!prog || !s_BindBufBase) return;
    // The translated shaders use layout(binding=0) for VS and layout(binding=1)
    // for PS, so glBindBufferBase to the same indices is sufficient in GL 4.2+.
    // For GL 4.1 compat we also query the block index and call glUniformBlockBinding.
    // However glUniformBlockBinding is a core 3.1 function so load it dynamically.
    typedef void (APIENTRY *PFN_glUniformBlockBinding)(GLuint,GLuint,GLuint);
    static PFN_glUniformBlockBinding s_UBB = nullptr;
    if (!s_UBB) s_UBB = (PFN_glUniformBlockBinding)
        SDL_GL_GetProcAddress("glUniformBlockBinding");

    // Re-bind UBO base points every time we switch programs
    s_BindBufBase((GLenum)GL_UNIFORM_BUFFER_C, 0u, s_vs_ubo);
    s_BindBufBase((GLenum)GL_UNIFORM_BUFFER_C, 1u, s_ps_ubo);
    (void)s_UBB;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void gx2_render_set_vertex_shader(uint32_t vs_addr, uint8_t* mem) {
    if (!vs_addr) return;
    s_cur_vs_addr = vs_addr;

    // GX2VertexShader: shaderData.buffer at +0x58, elemCount at +0x54
    uint32_t elem_count = rbrew_read32(mem, vs_addr + 0x54u);
    uint32_t buf_ptr    = rbrew_read32(mem, vs_addr + 0x58u);
    s_cur_vs_attribs = rbrew_read32(mem, vs_addr + 0x10u); // attribVarCount
    if (!buf_ptr || !elem_count) return;

    // Binary may already be cached — nothing extra to do here; the program
    // will be compiled lazily when both VS and PS are available.
    (void)elem_count; (void)buf_ptr;
}

void gx2_render_set_pixel_shader(uint32_t ps_addr, uint8_t* mem) {
    if (!ps_addr) return;
    s_cur_ps_addr = ps_addr;
    (void)mem;
}

// Resolve (or compile) the current VS+PS program and call glUseProgram.
// Called internally at draw time.
static GLuint resolve_active_program(uint8_t* mem) {
    if (!s_cur_vs_addr || !s_cur_ps_addr) return s_stub_program;

    uint64_t key = ((uint64_t)s_cur_vs_addr << 32) | (uint64_t)s_cur_ps_addr;
    auto it = s_prog_cache.find(key);
    if (it != s_prog_cache.end()) return it->second;

    // Translate VS
    char* vs_glsl = nullptr;
    {
        uint32_t buf_ptr  = rbrew_read32(mem, s_cur_vs_addr + 0x58u);
        uint32_t buf_size = rbrew_read32(mem, s_cur_vs_addr + 0x54u) *
                            rbrew_read32(mem, s_cur_vs_addr + 0x50u);
        if (buf_ptr && buf_size) {
            R700TranslateInput ti;
            ti.data        = mem + buf_ptr;
            ti.size        = buf_size;
            ti.is_pixel    = false;
            ti.num_attribs = s_cur_vs_attribs;
            vs_glsl = r700_to_glsl(ti);
        }
    }

    // Translate PS
    char* ps_glsl = nullptr;
    {
        uint32_t buf_ptr  = rbrew_read32(mem, s_cur_ps_addr + 0x38u);
        uint32_t buf_size = rbrew_read32(mem, s_cur_ps_addr + 0x34u) *
                            rbrew_read32(mem, s_cur_ps_addr + 0x30u);
        if (buf_ptr && buf_size) {
            R700TranslateInput ti;
            ti.data        = mem + buf_ptr;
            ti.size        = buf_size;
            ti.is_pixel    = true;
            ti.num_attribs = 0;
            ps_glsl = r700_to_glsl(ti);
        }
    }

    GLuint prog = 0;
    if (vs_glsl && ps_glsl) {
        prog = compile_translated_program(vs_glsl, ps_glsl);
        if (!prog) {
            fprintf(stderr, "[gx2_render] VS src:\n%s\n", vs_glsl);
            fprintf(stderr, "[gx2_render] PS src:\n%s\n", ps_glsl);
        }
    }
    free(vs_glsl);
    free(ps_glsl);

    if (!prog) prog = s_stub_program;
    s_prog_cache[key] = prog;
    return prog;
}

void gx2_render_set_uniform_reg(bool is_pixel,
                                uint32_t index,
                                uint32_t src_addr,
                                uint32_t num_vecs,
                                uint8_t* mem) {
    if (!src_addr || !num_vecs) return;
    if (index + num_vecs > UBO_VEC4_COUNT) {
        num_vecs = UBO_VEC4_COUNT - index;
    }

    float* dst = is_pixel ? (s_ps_constants + index * 4)
                           : (s_vs_constants + index * 4);
    GLuint ubo = is_pixel ? s_ps_ubo : s_vs_ubo;

    // Byte-swap from big-endian source into host floats
    for (uint32_t v = 0; v < num_vecs; v++) {
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t off = src_addr + (v * 4 + c) * 4u;
            uint32_t raw = rbrew_read32(mem, off);
            float f; memcpy(&f, &raw, 4);
            dst[v * 4 + c] = f;
        }
    }

    if (ubo && s_BindBuffer && s_BufSubData) {
        s_BindBuffer((GLenum)GL_UNIFORM_BUFFER_C, ubo);
        s_BufSubData((GLenum)GL_UNIFORM_BUFFER_C,
                     (GLintptr)(index * 16u),
                     (GLsizeiptr)(num_vecs * 16u),
                     dst);
    }
}

void gx2_render_set_uniform_block(bool is_pixel,
                                  uint32_t /*block_index*/,
                                  uint32_t src_addr,
                                  uint32_t size,
                                  uint8_t* mem) {
    if (!src_addr || !size) return;
    uint32_t num_vecs = size / 16u;
    if (num_vecs > UBO_VEC4_COUNT) num_vecs = UBO_VEC4_COUNT;
    gx2_render_set_uniform_reg(is_pixel, 0, src_addr, num_vecs, mem);
}

#endif // HAVE_SDL2