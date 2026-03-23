// r700_to_glsl.cpp — AMD R700 / Wii U Latte GPU shader bytecode → GLSL 410 core
//
// Translates the subset of the R700/Latte instruction set used by Gambit:
//   CF  : CF_ALU*, CF_TEX, CF_EXPORT / CF_EXPORT_DONE, CF_NOP
//   ALU : OP2 (ADD, MUL, MOV, DOT4, EXP, LOG, RECIP, RSQRT, SQRT, SIN, COS …)
//         OP3 (MULADD, CNDE, CNDGT, CNDGE)
//   TEX : SAMPLE, SAMPLE_L (2-D)
//   KCACHE: SRC 128-159 → vc[kc0_base+(sel-128)], 160-191 → vc[kc1_base+(sel-160)]
//
// All bytecode words are stored big-endian on the Wii U; this translator
// expects the raw bytes already present in memory (no further byte swap).

#include "r700_to_glsl.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Simple heap-backed string builder
// ---------------------------------------------------------------------------
struct SB {
    char*    buf = nullptr;
    uint32_t len = 0;
    uint32_t cap = 0;

    void ensure(uint32_t extra) {
        if (len + extra + 2 > cap) {
            cap = (cap + extra + 1) * 2 + 4096;
            buf = (char*)realloc(buf, cap);
            if (!buf) { cap = 0; len = 0; return; }
        }
    }
    void append(const char* s) {
        if (!s) return;
        uint32_t sl = (uint32_t)strlen(s);
        ensure(sl);
        if (!buf) return;
        memcpy(buf + len, s, sl + 1);
        len += sl;
    }
    void appendf(const char* fmt, ...) {
        char tmp[512];
        va_list va; va_start(va, fmt);
        vsnprintf(tmp, sizeof(tmp), fmt, va);
        va_end(va);
        append(tmp);
    }
    // Return the built string (caller must free()); clears this SB.
    char* take() {
        char* r = buf ? buf : strdup("");
        buf = nullptr; len = cap = 0;
        return r;
    }
    ~SB() { free(buf); }
};

// ---------------------------------------------------------------------------
// Big-endian 32-bit read from raw byte array
// ---------------------------------------------------------------------------
static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8) |(uint32_t)p[3];
}

// ---------------------------------------------------------------------------
// CF opcode constants (WORD1 bits [29:23])
// ---------------------------------------------------------------------------
static constexpr uint32_t CF_NOP         = 0x00;
static constexpr uint32_t CF_TEX         = 0x01;
static constexpr uint32_t CF_EXPORT      = 0x27;
static constexpr uint32_t CF_EXPORT_DONE = 0x28;
// ALU variants: low (0x08-0x0B) and high-bit Latte forms (0x48-0x4B)
static inline bool cf_is_alu(uint32_t op) {
    return (op >= 0x08 && op <= 0x0B) || (op >= 0x48 && op <= 0x4B);
}

// Channel swizzle names
static const char* CH[4] = { "x","y","z","w" };

// ---------------------------------------------------------------------------
// Emit a source operand GLSL expression
//   sel  : source selector (0-127 = GPR, 128-159 = KCACHE0, 160-191 = KCACHE1,
//          248 = 0.0, 249 = 1.0, 253 = PV)
//   chan : channel 0-3
//   neg  : if true, negate
//   kc0/kc1: KCACHE base indices (units of vec4 into vc[])
// ---------------------------------------------------------------------------
static void emit_src(SB& s, uint32_t sel, uint32_t chan, bool neg,
                     uint32_t kc0, uint32_t kc1) {
    if (neg) s.append("-");
    chan &= 3;
    if (sel < 128) {
        s.appendf("r[%u].%s", sel, CH[chan]);
    } else if (sel < 160) {
        s.appendf("vc[%u].%s", kc0 + (sel - 128), CH[chan]);
    } else if (sel < 192) {
        s.appendf("vc[%u].%s", kc1 + (sel - 160), CH[chan]);
    } else if (sel == 248) {
        s.append("0.0");
    } else if (sel == 249) {
        s.append("1.0");
    } else if (sel == 253) {
        s.appendf("pv.%s", CH[chan]);
    } else {
        s.appendf("0.0/*sel=%u*/", sel);
    }
}

// ---------------------------------------------------------------------------
// Emit a GLSL expression for an ALU OP2 instruction.
// ---------------------------------------------------------------------------
static void alu_op2_rhs(SB& out, uint32_t opcode,
                         uint32_t s0_sel, uint32_t s0_ch, bool s0_neg,
                         uint32_t s1_sel, uint32_t s1_ch, bool s1_neg,
                         uint32_t kc0, uint32_t kc1) {
    SB a, b;
    emit_src(a, s0_sel, s0_ch, s0_neg, kc0, kc1);
    emit_src(b, s1_sel, s1_ch, s1_neg, kc0, kc1);
    const char* A = a.buf ? a.buf : "0.0";
    const char* B = b.buf ? b.buf : "0.0";

    switch (opcode) {
    case 0x00: out.appendf("(%s)+(%s)", A, B);                 break; // ADD
    case 0x01: case 0x02:
               out.appendf("(%s)*(%s)", A, B);                 break; // MUL
    case 0x03: out.appendf("max(%s,%s)", A, B);                break; // MAX
    case 0x04: out.appendf("min(%s,%s)", A, B);                break; // MIN
    case 0x08: case 0x10:
               out.appendf("fract(%s)", A);                    break; // FRACT
    case 0x09: case 0x11:
               out.appendf("trunc(%s)", A);                    break; // TRUNC
    case 0x0A: case 0x12:
               out.appendf("ceil(%s)", A);                     break; // CEIL
    case 0x0C: case 0x14:
               out.appendf("floor(%s)", A);                    break; // FLOOR
    case 0x19: out.appendf("%s", A);                           break; // MOV
    case 0x1B: out.appendf("((%s)!=(%s)?1.0:0.0)", A, B);     break; // NOT_EQUAL
    case 0x1C: out.appendf("((%s)==(%s)?1.0:0.0)", A, B);     break; // EQUAL
    case 0x20: out.appendf("((%s)>(%s)?1.0:0.0)", A, B);      break; // PRED_SETGT
    case 0x21: out.appendf("((%s)>=(%s)?1.0:0.0)", A, B);     break; // PRED_SETGE
    // DOT4 — emitted as a MUL per component; caller folds the 4 into a dot4 result
    case 0x50: case 0x51:
               out.appendf("(%s)*(%s)", A, B);                 break;
    case 0x61: out.appendf("exp2(%s)", A);                     break; // EXP_IEEE
    case 0x62: case 0x63:
               out.appendf("log2(max(abs(%s),1e-30))", A);     break; // LOG
    case 0x66: out.appendf("(1.0/max(abs(%s),1e-30)*sign(%s+1e-30))", A, A); break; // RECIP
    case 0x69: out.appendf("inversesqrt(max(abs(%s),1e-30))", A); break; // RSQRT
    case 0x6A: out.appendf("sqrt(max(%s,0.0))", A);            break; // SQRT
    case 0x6E: out.appendf("sin((%s)*3.14159265358979)", A);   break; // SIN
    case 0x6F: out.appendf("cos((%s)*3.14159265358979)", A);   break; // COS
    default:   out.appendf("0.0/*op2=0x%02X*/", opcode);      break;
    }
}

// ---------------------------------------------------------------------------
// Translate one CF_ALU clause.
// ALU slots are 8 bytes each (2 × big-endian DWORD).
// ---------------------------------------------------------------------------
static void translate_alu_clause(SB& out,
                                  const uint8_t* data, uint32_t size,
                                  uint32_t addr, uint32_t count,
                                  uint32_t kc0, uint32_t kc1) {
    uint32_t end = addr + count * 8u;
    if (end > size) end = size;

    for (uint32_t off = addr; off + 7 < end; off += 8) {
        uint32_t w0 = be32(data + off + 0);
        uint32_t w1 = be32(data + off + 4);

        // WORD0 source fields
        uint32_t s0_sel  = (w0 >>  0) & 0x1FF;
        uint32_t s0_chan = (w0 >> 10) & 0x3;
        bool     s0_neg  = (w0 >> 12) & 1;
        uint32_t s1_sel  = (w0 >> 13) & 0x1FF;
        uint32_t s1_chan = (w0 >> 23) & 0x3;
        bool     s1_neg  = (w0 >> 25) & 1;

        // WORD1 common fields
        bool     write   = (w1 >>  4) & 1;
        bool     clamp   = (w1 >> 31) & 1;
        uint32_t dst_gpr = (w1 >> 21) & 0x7F;
        uint32_t dst_ch  = (w1 >> 29) & 0x3;
        uint32_t op11    = (w1 >>  7) & 0x7FF;

        if (!write) continue;

        out.appendf("    r[%u].%s = ", dst_gpr, CH[dst_ch & 3]);

        if (op11 >= 0x200) {
            // OP3: src2 in WORD1[8:0], src2_chan WORD1[11:10] (approx)
            uint32_t s2_sel  = (w1 >>  0) & 0x1FF;
            uint32_t s2_chan = (w1 >>  9) & 0x3;
            bool     s2_neg  = false; // OP3 negation less common; skip for now
            uint32_t op3     = (op11 >> 6) & 0x1F;

            SB a, b, c;
            emit_src(a, s0_sel, s0_chan, s0_neg, kc0, kc1);
            emit_src(b, s1_sel, s1_chan, s1_neg, kc0, kc1);
            emit_src(c, s2_sel, s2_chan, s2_neg, kc0, kc1);
            const char* A = a.buf?a.buf:"0.0";
            const char* B = b.buf?b.buf:"0.0";
            const char* C = c.buf?c.buf:"0.0";

            switch (op3) {
            case 0x10: case 0x14:
                out.appendf("(%s)*(%s)+(%s)", A, B, C); break; // MULADD
            case 0x18:
                out.appendf("((%s)==0.0?(%s):(%s))", A, B, C); break; // CNDE
            case 0x19:
                out.appendf("((%s)>0.0?(%s):(%s))", A, B, C); break; // CNDGT
            case 0x1A:
                out.appendf("((%s)>=0.0?(%s):(%s))", A, B, C); break; // CNDGE
            default:
                out.appendf("0.0/*op3=0x%02X*/", op3); break;
            }
        } else {
            SB rhs;
            alu_op2_rhs(rhs, op11,
                        s0_sel, s0_chan, s0_neg,
                        s1_sel, s1_chan, s1_neg,
                        kc0, kc1);
            out.append(rhs.buf ? rhs.buf : "0.0");
        }

        if (clamp) {
            out.appendf(";\n    r[%u].%s = clamp(r[%u].%s,0.0,1.0)",
                        dst_gpr, CH[dst_ch&3], dst_gpr, CH[dst_ch&3]);
        }
        out.append(";\n");
    }
}

// ---------------------------------------------------------------------------
// Translate one CF_TEX clause.
// TEX fetch slots are 16 bytes (4 × big-endian DWORD).
// ---------------------------------------------------------------------------
static void translate_tex_clause(SB& out,
                                  const uint8_t* data, uint32_t size,
                                  uint32_t addr, uint32_t count) {
    uint32_t end = addr + count * 16u;
    if (end > size) end = size;

    for (uint32_t off = addr; off + 15 < end; off += 16) {
        uint32_t w0 = be32(data + off + 0);
        uint32_t w1 = be32(data + off + 4);

        uint32_t res_id  = (w0 >>  8) & 0xFF; // resource slot (texture unit)
        uint32_t src_gpr = (w0 >> 16) & 0x7F;
        uint32_t dst_gpr = (w1 >> 24) & 0x7F;

        // Emit SAMPLE instruction — use .xy of src GPR as UV
        out.appendf("    r[%u] = texture(tex%u, r[%u].xy);\n",
                    dst_gpr, res_id, src_gpr);
    }
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
char* r700_to_glsl(const R700TranslateInput& in) {
    if (!in.data || in.size < 8) return nullptr;

    const uint8_t* data = in.data;
    uint32_t       size = in.size;

    // -------------------------------------------------------------------
    // Pass 1 — scan CF program: collect clause info, exports, GPR range
    // -------------------------------------------------------------------
    struct CFInst {
        uint32_t cf_op;
        uint32_t addr;          // byte offset of clause
        uint32_t count;         // clause slot count
        uint32_t kc0, kc1;     // KCACHE base indices (vec4 units)
        // EXPORT fields
        uint32_t exp_type;      // 0=PIXEL,1=POSITION,2=PARAMETER
        uint32_t exp_base;      // ARRAY_BASE
        uint32_t exp_src_gpr;
    };

    static constexpr uint32_t MAX_CF = 256;
    CFInst cf[MAX_CF];
    uint32_t ncf    = 0;
    uint32_t max_gpr = 3;
    uint32_t num_tex = 0;

    for (uint32_t off = 0; off + 7 < size && ncf < MAX_CF; off += 8) {
        uint32_t w0 = be32(data + off + 0);
        uint32_t w1 = be32(data + off + 4);
        uint32_t op = (w1 >> 23) & 0x7F;

        CFInst c = {};
        c.cf_op = op;

        if (cf_is_alu(op)) {
            c.addr  = ((w0 >>  0) & 0x3FFFFFu) * 8u;
            c.count = ((w1 >> 10) & 0x7Fu) + 1u;
            // KCACHE0: addr = WORD0[31:24]; KCACHE1: addr = WORD1[9:2]
            c.kc0   = ((w0 >> 24) & 0xFF) * 16u;
            c.kc1   = ((w1 >>  2) & 0xFF) * 16u;
            cf[ncf++] = c;

            // Scan for max GPR
            uint32_t e = c.addr + c.count * 8u;
            if (e > size) e = size;
            for (uint32_t a = c.addr; a + 7 < e; a += 8) {
                uint32_t g = (be32(data + a + 4) >> 21) & 0x7Fu;
                if (g > max_gpr) max_gpr = g;
            }
        } else if (op == CF_TEX) {
            c.addr  = ((w0 >>  0) & 0x3FFFFFu) * 8u;
            c.count = ((w1 >> 10) & 0x7Fu) + 1u;
            cf[ncf++] = c;

            uint32_t e = c.addr + c.count * 16u;
            if (e > size) e = size;
            for (uint32_t a = c.addr; a + 15 < e; a += 16) {
                uint32_t rid = (be32(data+a) >> 8) & 0xFF;
                if (rid + 1u > num_tex) num_tex = rid + 1u;
                uint32_t g = (be32(data+a+4) >> 24) & 0x7F;
                if (g > max_gpr) max_gpr = g;
            }
        } else if (op == CF_EXPORT || op == CF_EXPORT_DONE) {
            c.exp_type    = (w0 >> 13) & 0x3;
            c.exp_base    = (w0 >>  0) & 0x3F;
            c.exp_src_gpr = (w1 >> 16) & 0x7F;
            cf[ncf++] = c;
            if (c.exp_src_gpr > max_gpr) max_gpr = c.exp_src_gpr;
            if (op == CF_EXPORT_DONE) break;
        }
        // CF_NOP and unknown ops — keep scanning
    }

    // -------------------------------------------------------------------
    // Pass 2 — emit GLSL source
    // -------------------------------------------------------------------
    SB src;
    uint32_t ubo_binding = in.is_pixel ? 1u : 0u;

    src.append("#version 410 core\n\n");

    // Uniform constant buffer (256 vec4 entries)
    src.appendf("layout(std140, binding=%u) uniform Constants {\n"
                "    vec4 vc[256];\n"
                "};\n\n", ubo_binding);

    // Texture samplers
    for (uint32_t t = 0; t < num_tex; t++)
        src.appendf("uniform sampler2D tex%u;\n", t);
    if (num_tex) src.append("\n");

    if (!in.is_pixel) {
        // Vertex shader: inputs + varyings out
        for (uint32_t i = 0; i < in.num_attribs && i < 16u; i++)
            src.appendf("layout(location=%u) in vec4 a%u;\n", i, i);
        src.append("\nout vec4 vs_out[8];\n\n");
    } else {
        src.append("in  vec4 vs_out[8];\n"
                   "out vec4 frag_color;\n\n");
    }

    src.append("void main() {\n");

    // Temp registers — GLSL supports non-const array indexing in 4.10
    uint32_t nregs = max_gpr + 1u;
    if (nregs < 4)   nregs = 4;
    if (nregs > 128) nregs = 128;
    src.appendf("    vec4 r[%u];\n", nregs);
    src.append( "    vec4 pv = vec4(0.0);\n");

    // Load vertex attribs into r[0..num_attribs-1]
    if (!in.is_pixel) {
        for (uint32_t i = 0; i < in.num_attribs && i < nregs; i++)
            src.appendf("    r[%u] = a%u;\n", i, i);
    }
    src.append("\n");

    // Emit each CF instruction
    for (uint32_t i = 0; i < ncf; i++) {
        const CFInst& c = cf[i];
        if (cf_is_alu(c.cf_op)) {
            translate_alu_clause(src, data, size,
                                 c.addr, c.count, c.kc0, c.kc1);
        } else if (c.cf_op == CF_TEX) {
            translate_tex_clause(src, data, size, c.addr, c.count);
        } else if (c.cf_op == CF_EXPORT || c.cf_op == CF_EXPORT_DONE) {
            if (c.exp_type == 1) {
                // POSITION → gl_Position
                src.appendf("    gl_Position = r[%u];\n", c.exp_src_gpr);
            } else if (c.exp_type == 0 && in.is_pixel) {
                // PIXEL colour → frag_color (first export)
                src.appendf("    frag_color = r[%u];\n", c.exp_src_gpr);
            } else if (c.exp_type == 2 && !in.is_pixel) {
                // PARAMETER → vs_out varying
                if (c.exp_base < 8)
                    src.appendf("    vs_out[%u] = r[%u];\n",
                                c.exp_base, c.exp_src_gpr);
            }
        }
    }

    src.append("}\n");
    return src.take();
}
