// GX2 draw, state, shader, texture, and surface stubs

#include "../os_common.h"
#include "gx2_surface.h"
#include "gx2_attrib.h"
#include "gx2_render.h"
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Draw calls
// ---------------------------------------------------------------------------

// GX2DrawEx(primitiveType, count, firstVertex, numInstances)
// r3=primType, r4=count, r5=firstVertex, r6=numInstances
static void GX2DrawEx(CPUState* cpu) {
    gx2_render_draw(ARG0, ARG1, ARG2, MEM);
}

// GX2DrawIndexedEx(primitiveType, count, indexType, indices, firstVertex, numInstances)
// r3=primType, r4=count, r5=indexType, r6=indices(guest), r7=firstVertex, r8=numInstances
static void GX2DrawIndexedEx(CPUState* cpu) {
    gx2_render_draw_indexed(ARG0, ARG1, ARG2, ARG3, ARG4, MEM);
}

// GX2DrawEx2 — same layout as GX2DrawEx but with additional params; treat as DrawEx
static void GX2DrawEx2(CPUState* cpu) {
    gx2_render_draw(ARG0, ARG1, ARG2, MEM);
}

// GX2DrawIndexedEx2 — same as DrawIndexedEx with extra params
static void GX2DrawIndexedEx2(CPUState* cpu) {
    gx2_render_draw_indexed(ARG0, ARG1, ARG2, ARG3, ARG4, MEM);
}

static void GX2DrawStreamOut(CPUState* cpu)  { (void)cpu; }
static void GX2SetPrimitiveRestartIndex(CPUState* cpu) { (void)cpu; }

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

static void GX2ClearColor(CPUState* cpu) {
    // GX2ClearColor(GX2ColorBuffer* cb, float r, float g, float b, float a)
    // r3=cb_ptr, f1-f4=r,g,b,a  (PPC float args use FPR, not GPR)
    (void)ARG0; // cb_ptr — we already have the FBO bound via GX2SetColorBuffer
    gx2_render_clear_color((float)cpu->f[1], (float)cpu->f[2],
                           (float)cpu->f[3], (float)cpu->f[4]);
}

static void GX2ClearDepthStencilEx(CPUState* cpu) {
    // GX2ClearDepthStencilEx(GX2DepthBuffer* db, float depth, uint8 stencil, GX2ClearFlags flags)
    // r3=db, f1=depth, r4=stencil, r5=flags
    float   depth   = (float)cpu->f[1];
    uint8_t stencil = (uint8_t)ARG1;
    uint32_t flags  = ARG2;
    gx2_render_clear_depth_stencil(depth, stencil, flags);
}

static void GX2ClearBuffersEx(CPUState* cpu) {
    // GX2ClearBuffersEx(GX2ColorBuffer* cb, GX2DepthBuffer* db,
    //                   float r, float g, float b, float a,
    //                   float depth, uint8 stencil, GX2ClearFlags flags)
    // r3=cb, r4=db, f1-f4=r,g,b,a, f5=depth, r5=stencil, r6=flags
    gx2_render_clear_buffers((float)cpu->f[1], (float)cpu->f[2],
                              (float)cpu->f[3], (float)cpu->f[4],
                              (float)cpu->f[5], (uint8_t)ARG2);
}

static void GX2ResolveAAColorBuffer(CPUState* cpu) { (void)cpu; }

// ---------------------------------------------------------------------------
// Raster state
// ---------------------------------------------------------------------------

static void GX2SetColorControl(CPUState* cpu) {
    // GX2SetColorControl(logicOp, blendEnable, unk, colorWriteEnable)
    // r3=logicOp, r4=blendEnable (uint8 bitmask), r5=unk, r6=colorWriteEnable
    // Blend enable per-target is handled more precisely by GX2SetBlendControl.
    // If blendEnable == 0 for all targets, disable blend globally.
    bool any_blend = (ARG1 != 0);
    if (!any_blend) {
        gx2_render_set_blend_control(false, 0, 0, 0, 0, 0, 0);
    }
    (void)cpu;
}

static void GX2SetDepthOnlyControl(CPUState* cpu) {
    // GX2SetDepthOnlyControl(depthTest, depthWrite, compareFunc)
    gx2_render_set_depth_stencil_control(ARG0 != 0, ARG1 != 0, ARG2);
}

static void GX2SetAlphaTest(CPUState* cpu)          { (void)cpu; } // GL 3.x has no alpha test
static void GX2SetBlendConstantColor(CPUState* cpu) { (void)cpu; }
static void GX2SetColorMask(CPUState* cpu)          { (void)cpu; }
static void GX2SetAlphaToMask(CPUState* cpu)        { (void)cpu; }
static void GX2SetStencilMask(CPUState* cpu)        { (void)cpu; }
static void GX2SetPolygonOffset(CPUState* cpu)      { (void)cpu; }
static void GX2SetPointSize(CPUState* cpu)          { (void)cpu; }

static void GX2SetTargetChannelMasks(CPUState* cpu) { (void)cpu; }

static void GX2SetBlendControl(CPUState* cpu) {
    // GX2SetBlendControl(target, srcColor, dstColor, combine,
    //                    alphaBlend, srcAlpha, dstAlpha, alphaCombine)
    // r3=target, r4=srcColor, r5=dstColor, r6=combine,
    // r7=alphaBlend, r8=srcAlpha, r9=dstAlpha, r10=alphaCombine
    bool blend = (ARG4 != 0) || (ARG1 != GX2_BLEND_MODE_ONE) || (ARG2 != GX2_BLEND_MODE_ZERO);
    gx2_render_set_blend_control(blend, ARG1, ARG2, ARG3, ARG5, ARG6, ARG7);
}

static void GX2SetPolygonControl(CPUState* cpu) {
    // GX2SetPolygonControl(frontFaceMode, cullFront, cullBack, ...)
    // r3=frontFace, r4=cullFront, r5=cullBack, ...
    gx2_render_set_polygon_control(ARG0, ARG1 != 0, ARG2 != 0);
}

static void GX2SetCullOnlyControl(CPUState* cpu) {
    // GX2SetCullOnlyControl(frontFaceMode, cullFront, cullBack)
    gx2_render_set_polygon_control(ARG0, ARG1 != 0, ARG2 != 0);
}

static void GX2SetScissor(CPUState* cpu) {
    // GX2SetScissor(x, y, w, h) — uint32 in r3-r6
    gx2_render_set_scissor(ARG0, ARG1, ARG2, ARG3);
}

static void GX2SetViewport(CPUState* cpu) {
    // GX2SetViewport(x, y, w, h, near, far) — all floats in f1-f6
    gx2_render_set_viewport((float)cpu->f[1], (float)cpu->f[2],
                             (float)cpu->f[3], (float)cpu->f[4],
                             (float)cpu->f[5], (float)cpu->f[6]);
}

static void GX2SetViewportReg(CPUState* cpu) { (void)cpu; }
static void GX2SetScissorReg(CPUState* cpu)  { (void)cpu; }
static void GX2InitDepthStencilControlReg(CPUState* cpu) { (void)cpu; }
static void GX2GetDepthStencilControlReg(CPUState* cpu)  { (void)cpu; }

static void GX2SetDepthStencilControl(CPUState* cpu) {
    // GX2SetDepthStencilControl(depthTest, depthWrite, compareFunc, ...)
    gx2_render_set_depth_stencil_control(ARG0 != 0, ARG1 != 0, ARG2);
}

// ---------------------------------------------------------------------------
// Surface / color buffer / depth buffer
// ---------------------------------------------------------------------------

static void GX2CalcSurfaceSizeAndAlignment(CPUState* cpu) {
    // ARG0 = GX2Surface*
    uint32_t surf = ARG0;
    if (!surf) return;

    uint32_t width  = rbrew_read32(MEM, surf + GX2SURF_WIDTH);
    uint32_t height = rbrew_read32(MEM, surf + GX2SURF_HEIGHT);
    uint32_t fmt    = rbrew_read32(MEM, surf + GX2SURF_FORMAT);
    if (width  == 0) width  = 1;
    if (height == 0) height = 1;

    // BPP lookup from format
    uint32_t bpp;
    switch (fmt) {
    case GX2FMT_UNORM_R8:                           bpp = 1;  break;
    case GX2FMT_UNORM_R8_G8:                        bpp = 2;  break;
    case GX2FMT_UNORM_R5_G6_B5:
    case GX2FMT_UNORM_R5_G5_B5_A1:
    case GX2FMT_UNORM_R4_G4_B4_A4:
    case GX2FMT_FLOAT_R16:                          bpp = 2;  break;
    case GX2FMT_UNORM_R8_G8_B8_A8:
    case GX2FMT_SRGB_R8_G8_B8_A8:
    case GX2FMT_FLOAT_D24_S8:
    case GX2FMT_FLOAT_D32:
    case GX2FMT_FLOAT_R11_G11_B10:
    case GX2FMT_FLOAT_R32:
    case GX2FMT_FLOAT_R16_G16:                      bpp = 4;  break;
    case GX2FMT_FLOAT_R16_G16_B16_A16:              bpp = 8;  break;
    case GX2FMT_FLOAT_R32_G32:                      bpp = 8;  break;
    case GX2FMT_FLOAT_R32_G32_B32_A32:              bpp = 16; break;
    case GX2FMT_UNORM_BC1:
    case GX2FMT_UNORM_BC4: {
        // 4x4 blocks of 8 bytes = 0.5 bpp; store as compressed size
        uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
        uint32_t img_sz  = bw * bh * 8;
        rbrew_write32(MEM, surf + GX2SURF_IMAGE_SIZE,  img_sz);
        rbrew_write32(MEM, surf + GX2SURF_MIPMAP_SIZE, 0);
        rbrew_write32(MEM, surf + GX2SURF_ALIGNMENT,   256);
        rbrew_write32(MEM, surf + GX2SURF_PITCH,       bw * 4);
        rbrew_write32(MEM, surf + GX2SURF_TILE_MODE,   GX2_TILE_MODE_LINEAR_ALIGNED);
        return;
    }
    case GX2FMT_UNORM_BC2:
    case GX2FMT_UNORM_BC3:
    case GX2FMT_UNORM_BC5: {
        uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
        uint32_t img_sz  = bw * bh * 16;
        rbrew_write32(MEM, surf + GX2SURF_IMAGE_SIZE,  img_sz);
        rbrew_write32(MEM, surf + GX2SURF_MIPMAP_SIZE, 0);
        rbrew_write32(MEM, surf + GX2SURF_ALIGNMENT,   256);
        rbrew_write32(MEM, surf + GX2SURF_PITCH,       bw * 4);
        rbrew_write32(MEM, surf + GX2SURF_TILE_MODE,   GX2_TILE_MODE_LINEAR_ALIGNED);
        return;
    }
    default:
        bpp = 4;
        break;
    }

    uint32_t pitch  = width;
    uint32_t img_sz = pitch * height * bpp;
    rbrew_write32(MEM, surf + GX2SURF_IMAGE_SIZE,  img_sz);
    rbrew_write32(MEM, surf + GX2SURF_MIPMAP_SIZE, 0);
    rbrew_write32(MEM, surf + GX2SURF_ALIGNMENT,   256);
    rbrew_write32(MEM, surf + GX2SURF_PITCH,        pitch);
    rbrew_write32(MEM, surf + GX2SURF_TILE_MODE,    GX2_TILE_MODE_LINEAR_ALIGNED);
}

static void GX2CalcColorBufferAuxInfo(CPUState* cpu) {
    if (ARG1) rbrew_write32(MEM, ARG1, 0u);
    if (ARG2) rbrew_write32(MEM, ARG2, 0u);
}

static void GX2CalcDepthBufferHiZInfo(CPUState* cpu) {
    if (ARG1) rbrew_write32(MEM, ARG1, 0u);
    if (ARG2) rbrew_write32(MEM, ARG2, 0u);
}

static void GX2InitColorBufferRegs(CPUState* cpu) { (void)cpu; }
static void GX2InitDepthBufferRegs(CPUState* cpu) { (void)cpu; }
static void GX2InitDepthBufferHiZEnable(CPUState* cpu) { (void)cpu; }

static void GX2SetColorBuffer(CPUState* cpu) {
    // GX2SetColorBuffer(GX2ColorBuffer* cb, GX2RenderTarget target)
    // r3=cb, r4=target (0 = colour buffer 0)
    gx2_render_set_color_buffer(ARG0, MEM);
}

static void GX2SetDepthBuffer(CPUState* cpu) {
    // GX2SetDepthBuffer(GX2DepthBuffer* db)
    gx2_render_set_depth_buffer(ARG0, MEM);
}

static void GX2CopySurface(CPUState* cpu) { (void)cpu; }

// ---------------------------------------------------------------------------
// Texture / sampler
// ---------------------------------------------------------------------------

static void GX2InitTextureRegs(CPUState* cpu) {
    uint32_t tex = ARG0;
    if (!tex) return;
    for (uint32_t i = 0; i < 0x60; i += 4)
        rbrew_write32(MEM, tex + GX2TEX_VIEW_MIP + i, 0);
}

static void GX2SetPixelTexture(CPUState* cpu) {
    // GX2SetPixelTexture(GX2Texture* tex, uint32 slot)
    gx2_render_upload_texture(ARG0, ARG1, MEM);
}

static void GX2SetVertexTexture(CPUState* cpu) {
    gx2_render_bind_texture(ARG0, ARG1, MEM);
}

static void GX2SetGeometryTexture(CPUState* cpu) {
    gx2_render_bind_texture(ARG0, ARG1, MEM);
}

static void GX2InitSampler(CPUState* cpu) {
    uint32_t smp = ARG0;
    if (!smp) return;
    rbrew_write32(MEM, smp + 0, 0);
    rbrew_write32(MEM, smp + 4, 0);
    rbrew_write32(MEM, smp + 8, 0);
}

static void GX2InitSamplerBorderType(CPUState* cpu) { (void)cpu; }
static void GX2SetPixelSampler(CPUState* cpu)       { (void)cpu; }
static void GX2SetVertexSampler(CPUState* cpu)      { (void)cpu; }
static void GX2SetGeometrySampler(CPUState* cpu)    { (void)cpu; }

// ---------------------------------------------------------------------------
// Shaders
// ---------------------------------------------------------------------------

// GX2SetVertexShader(GX2VertexShader* vs)   r3 = vs_addr
static void GX2SetVertexShader(CPUState* cpu) {
    gx2_render_set_vertex_shader(ARG0, MEM);
}

// GX2SetPixelShader(GX2PixelShader* ps)   r3 = ps_addr
static void GX2SetPixelShader(CPUState* cpu) {
    gx2_render_set_pixel_shader(ARG0, MEM);
}

static void GX2SetGeometryShader(CPUState* cpu) { (void)cpu; }

// GX2SetVertexUniformReg(uint32_t index, uint32_t count, const void* data)
// r3=index (in vec4 units), r4=count (num vec4s), r5=data ptr
static void GX2SetVertexUniformReg(CPUState* cpu) {
    gx2_render_set_uniform_reg(false, ARG0, ARG2, ARG1, MEM);
}

// GX2SetPixelUniformReg(uint32_t index, uint32_t count, const void* data)
static void GX2SetPixelUniformReg(CPUState* cpu) {
    gx2_render_set_uniform_reg(true, ARG0, ARG2, ARG1, MEM);
}

static void GX2SetGeometryUniformReg(CPUState* cpu) { (void)cpu; }

// GX2SetVertexUniformBlock(uint32_t blockIndex, uint32_t size, const void* data)
// r3=blockIndex, r4=size (bytes), r5=data ptr
static void GX2SetVertexUniformBlock(CPUState* cpu) {
    gx2_render_set_uniform_block(false, ARG0, ARG2, ARG1, MEM);
}

// GX2SetPixelUniformBlock(uint32_t blockIndex, uint32_t size, const void* data)
static void GX2SetPixelUniformBlock(CPUState* cpu) {
    gx2_render_set_uniform_block(true, ARG0, ARG2, ARG1, MEM);
}

static void GX2SetGeometryUniformBlock(CPUState* cpu) { (void)cpu; }
static void GX2SetShaderModeEx(CPUState* cpu)         { (void)cpu; }

// GX2SetFetchShader(GX2FetchShader* fs)
// Sets the active fetch shader, which defines the vertex attribute layout.
static void GX2SetFetchShader(CPUState* cpu) {
    gx2_render_set_active_fetch_shader(ARG0);
}

// GX2SetAttribBuffer(uint32_t index, uint32_t size, uint32_t stride, void* buffer)
// r3=index, r4=size, r5=stride, r6=buffer (guest ptr)
static void GX2SetAttribBuffer(CPUState* cpu) {
    gx2_render_set_attrib_buffer(ARG0, ARG3, ARG1, ARG2);
}

static void GX2CalcFetchShaderSizeEx(CPUState* cpu) {
    uint32_t attrib_count = ARG0;
    RET = 16 + attrib_count * 16;
}

// GX2InitFetchShaderEx(GX2FetchShader* fs, void* program, uint32_t attribCount,
//                      GX2AttribStream* attribs, ...)
// r3=fs, r4=program, r5=attribCount, r6=attribs
static void GX2InitFetchShaderEx(CPUState* cpu) {
    uint32_t fs           = ARG0;
    uint32_t attrib_count = ARG2;
    uint32_t streams_addr = ARG3;
    if (!fs) return;
    rbrew_write32(MEM, fs + 0,  0);
    rbrew_write32(MEM, fs + 4,  attrib_count);
    rbrew_write32(MEM, fs + 8,  ARG1); // program pointer
    rbrew_write32(MEM, fs + 12, 0);
    // Register the attrib stream layout so we can set up VAO at draw time
    gx2_render_save_fetch_shader(fs, attrib_count, streams_addr);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "gx2_addrs.h"

void gx2_draw_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(GX2DrawEx);
    REG(GX2DrawIndexedEx);
    REG(GX2DrawIndexedEx2);
    REG(GX2SetPrimitiveRestartIndex);
    REG(GX2ClearColor);
    REG(GX2ClearDepthStencilEx);
    REG(GX2ClearBuffersEx);
    REG(GX2SetColorControl);
    REG(GX2SetDepthOnlyControl);
    REG(GX2SetAlphaTest);
    REG(GX2SetBlendControl);
    REG(GX2SetBlendConstantColor);
    REG(GX2SetTargetChannelMasks);
    REG(GX2SetAlphaToMask);
    REG(GX2SetStencilMask);
    REG(GX2SetPolygonControl);
    REG(GX2SetCullOnlyControl);
    REG(GX2SetPointSize);
    REG(GX2SetScissor);
    REG(GX2SetViewport);
    REG(GX2SetDepthStencilControl);
    REG(GX2CalcSurfaceSizeAndAlignment);
    REG(GX2CalcColorBufferAuxInfo);
    REG(GX2CalcDepthBufferHiZInfo);
    REG(GX2InitColorBufferRegs);
    REG(GX2InitDepthBufferRegs);
    REG(GX2InitDepthBufferHiZEnable);
    REG(GX2SetColorBuffer);
    REG(GX2SetDepthBuffer);
    REG(GX2CopySurface);
    REG(GX2InitTextureRegs);
    REG(GX2SetPixelTexture);
    REG(GX2SetVertexTexture);
    REG(GX2SetGeometryTexture);
    REG(GX2InitSampler);
    REG(GX2InitSamplerBorderType);
    REG(GX2SetPixelSampler);
    REG(GX2SetVertexSampler);
    REG(GX2SetGeometrySampler);
    REG(GX2SetVertexShader);
    REG(GX2SetPixelShader);
    REG(GX2SetGeometryShader);
    REG(GX2SetVertexUniformReg);
    REG(GX2SetPixelUniformReg);
    REG(GX2SetVertexUniformBlock);
    REG(GX2SetPixelUniformBlock);
    REG(GX2SetGeometryUniformBlock);
    REG(GX2SetShaderModeEx);
    REG(GX2SetFetchShader);
    REG(GX2CalcFetchShaderSizeEx);
    REG(GX2InitFetchShaderEx);
    REG(GX2SetAttribBuffer);
#undef REG
}
