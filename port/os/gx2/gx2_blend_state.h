#pragma once
// Colour/blend state as gx2.rpl builds it (GX2InitColorControlReg 0202d820,
// GX2InitBlendControlReg 0202e1b4): blending is switched on per render
// target by GX2SetColorControl's blendEnableMask; GX2SetBlendControl only
// sets factors, and its 5th argument selects separate alpha factors (when
// off, alpha uses the colour factors). colorBufferEnable == 0 turns colour
// writes off (CB_COLOR_CONTROL special op "disable").
#include <cstdint>

struct Gx2BlendFactors {
    uint32_t src_color = 1, dst_color = 0, color_combine = 0;  // ONE, ZERO, ADD
    uint32_t src_alpha = 1, dst_alpha = 0, alpha_combine = 0;
};

struct Gx2ColorBlendState {
    uint32_t blend_enable_mask = 0;
    bool color_write = true;
    Gx2BlendFactors target0;

    // GX2SetColorControl(logicOp, blendEnableMask, multiWrite, colorBufferEnable)
    void set_color_control(uint32_t blend_enable_mask_, uint32_t color_buffer_enable) {
        blend_enable_mask = blend_enable_mask_ & 0xFF;
        color_write = color_buffer_enable != 0;
    }
    // GX2SetBlendControl(target, cSrc, cDst, cComb, separateAlpha, aSrc, aDst, aComb)
    void set_blend_control(uint32_t target, uint32_t src_c, uint32_t dst_c, uint32_t comb_c,
                           uint32_t separate_alpha, uint32_t src_a, uint32_t dst_a, uint32_t comb_a) {
        if (target != 0) return;  // only render target 0 is drawn natively so far
        target0.src_color = src_c; target0.dst_color = dst_c; target0.color_combine = comb_c;
        if (separate_alpha) { target0.src_alpha = src_a; target0.dst_alpha = dst_a; target0.alpha_combine = comb_a; }
        else { target0.src_alpha = src_c; target0.dst_alpha = dst_c; target0.alpha_combine = comb_c; }
    }
    bool blend_target0() const { return (blend_enable_mask & 1) != 0; }
};
