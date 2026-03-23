// snd_core / AX audio
//
// Implements a 96-voice AX pool backed by miniaudio (single-header library).
// All voice state is tracked host-side; the miniaudio data callback mixes
// active PCM16/PCM8/ADPCM voices to stereo f32 output at 48000 Hz.
//
// Nintendo DSP ADPCM (GameCube/Wii/Wii U):
//   Frame layout: 1 header byte + 7 data bytes = 14 4-bit samples per frame.
//   Header byte: bits[7:4] = coefficient-pair index (0-7),
//                bits[3:0] = shift amount (scale).
//   Decode: samp = (nibble_signed << scale) + (c1*h1 + c2*h2 + 1024) >> 11
//   Coefficients are signed Q1.11 fixed-point (as in the Wii U AX API).
//
//   ADPCM voices are pre-decoded to float PCM when AXSetVoiceOffsets is called
//   (while the voice is stopped), so the mixer callback can treat them as PCM.
//
// Without miniaudio: voice state is tracked but no audio is output.

#include "../os_common.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>
#include <cstdint>
#include <cmath>

// ---------------------------------------------------------------------------
// miniaudio — optional header-only back-end
// ---------------------------------------------------------------------------
#if defined(HAVE_MINIAUDIO)
#define MINIAUDIO_IMPLEMENTATION
#include "../../third_party/miniaudio.h"
#endif

// ---------------------------------------------------------------------------
// Voice format constants (AXVoiceFormat)
// ---------------------------------------------------------------------------
static constexpr uint32_t AX_FMT_ADPCM  = 0x00u;
static constexpr uint32_t AX_FMT_PCM8   = 0x0Au;
static constexpr uint32_t AX_FMT_PCM16  = 0x1Au;

// AX voice state constants
static constexpr uint32_t AX_VOICE_STATE_STOPPED = 0;
static constexpr uint32_t AX_VOICE_STATE_RUNNING  = 1;

// ---------------------------------------------------------------------------
// AXVoiceOffsets guest struct layout (big-endian)
//   +0x00  uint16  format   (AX_FMT_*)
//   +0x02  uint16  loopFlag (0=no loop, 1=loop)
//   +0x04  uint32  loopOffset  (in samples)
//   +0x08  uint32  endOffset   (in samples, exclusive)
//   +0x0C  uint32  currentOffset (in samples)
//   +0x10  uint32  data         (guest ptr to PCM data)
// ---------------------------------------------------------------------------
static constexpr uint32_t AXVO_FORMAT       = 0x00u;
static constexpr uint32_t AXVO_LOOP_FLAG    = 0x02u;
static constexpr uint32_t AXVO_LOOP_OFFSET  = 0x04u;
static constexpr uint32_t AXVO_END_OFFSET   = 0x08u;
static constexpr uint32_t AXVO_CUR_OFFSET   = 0x0Cu;
static constexpr uint32_t AXVO_DATA         = 0x10u;

// AXVoiceVe guest struct (+0x00 uint16 volume Q15, +0x02 int16 delta)
static constexpr uint32_t AXVE_VOLUME       = 0x00u;

// AXVoiceSrc guest struct (+0x00 uint32 ratio Q16.16)
static constexpr uint32_t AXSRC_RATIO       = 0x00u;

// ---------------------------------------------------------------------------
// Voice pool
// ---------------------------------------------------------------------------
static constexpr uint32_t AX_VOICE_COUNT = 96u;

// Guest base address for voice handles.
// Handle for slot i = AX_VOICE_BASE + i * AX_VOICE_HANDLE_STRIDE
static constexpr uint32_t AX_VOICE_BASE           = 0x50000000u;
static constexpr uint32_t AX_VOICE_HANDLE_STRIDE  = 0x40u;     // 64 bytes per slot

struct AXVoiceState {
    std::atomic<bool> active{false};   // acquired (not free)
    std::atomic<bool> playing{false};  // currently producing audio

    uint32_t format    = 0;
    uint32_t pcm_addr  = 0;   // guest ptr to PCM data
    uint32_t loop_offs = 0;   // loop start in samples
    uint32_t end_offs  = 0;   // end position in samples

    // Playback position as a fractional sample index (written/read by callback)
    std::atomic<uint32_t> pos_int{0};       // integer part
    float pos_frac     = 0.0f;              // fractional part (callback-private)

    float src_ratio    = 1.0f;             // pitch ratio (source_rate / 48000)
    float vol          = 1.0f;             // linear volume [0,1]
    float vol_delta    = 0.0f;             // per-sample volume delta
    float mix_l        = 1.0f;
    float mix_r        = 1.0f;
    bool  loop         = false;

    // Nintendo DSP ADPCM state
    int16_t adpcm_coefs[16]   = {};  // 8 coefficient pairs (c1, c2)
    int16_t adpcm_hist1       = 0;   // yn1 — set from AXSetVoiceAdpcm
    int16_t adpcm_hist2       = 0;   // yn2
    int16_t adpcm_loop_hist1  = 0;   // yn1 at loop start
    int16_t adpcm_loop_hist2  = 0;   // yn2 at loop start
    bool    adpcm_coefs_valid = false;

    // Pre-decoded ADPCM buffer (nullptr for PCM voices)
    // Replaced atomically: game thread writes, callback reads.
    float*                    adpcm_buf   = nullptr;
    uint32_t                  adpcm_len   = 0;      // total decoded samples
    std::atomic<bool>         adpcm_ready{false};   // true once buf is valid
};

static AXVoiceState s_voices[AX_VOICE_COUNT];
static uint8_t*     s_mem = nullptr;       // guest arena (set at AXInit time)
static bool         s_ax_inited = false;

// ---------------------------------------------------------------------------
// Nintendo DSP ADPCM decoder
//
// Frame layout (8 bytes per frame, 14 samples):
//   byte 0     : header — bits[7:4] = coef_index (0-7), bits[3:0] = scale
//   bytes 1..7 : 14 4-bit samples, high-nibble first
//
// Decode formula (Dolphin-verified):
//   samp = (nibble_signed << scale) + (c1*h1 + c2*h2 + 1024) >> 11
//   clamp samp to [-32768, 32767]
//   h2 = h1; h1 = samp
// ---------------------------------------------------------------------------
static constexpr uint32_t ADPCM_SAMPLES_PER_FRAME = 14u;
static constexpr uint32_t ADPCM_BYTES_PER_FRAME   =  8u;

// Decode entire ADPCM stream (from guest ptr src_addr, byte_count bytes) into
// a newly-allocated float array.  Returns the array; *out_len receives count.
// Caller must free() the returned pointer.
static float* adpcm_decode_stream(const uint8_t* src,
                                   uint32_t       byte_count,
                                   const int16_t  coefs[16],
                                   int16_t        init_h1,
                                   int16_t        init_h2,
                                   uint32_t*      out_len) {
    if (!src || byte_count < ADPCM_BYTES_PER_FRAME) {
        *out_len = 0;
        return nullptr;
    }

    uint32_t num_frames    = byte_count / ADPCM_BYTES_PER_FRAME;
    uint32_t total_samples = num_frames * ADPCM_SAMPLES_PER_FRAME;
    float*   out           = (float*)malloc(total_samples * sizeof(float));
    if (!out) { *out_len = 0; return nullptr; }
    *out_len = total_samples;

    int32_t h1 = init_h1, h2 = init_h2;

    for (uint32_t fr = 0; fr < num_frames; fr++) {
        const uint8_t* fp  = src + fr * ADPCM_BYTES_PER_FRAME;
        uint8_t  header    = fp[0];
        int32_t  coef_idx  = (header >> 4) & 0x7;
        int32_t  scale     = header & 0xF;
        int32_t  c1        = coefs[coef_idx * 2 + 0];
        int32_t  c2        = coefs[coef_idx * 2 + 1];
        uint32_t base      = fr * ADPCM_SAMPLES_PER_FRAME;

        for (uint32_t n = 0; n < ADPCM_SAMPLES_PER_FRAME; n++) {
            uint8_t raw    = fp[1 + n / 2];
            int32_t nibble = (n & 1) ? (raw & 0xF) : ((raw >> 4) & 0xF);
            if (nibble & 0x8) nibble |= ~0xF;  // sign-extend 4-bit

            int32_t samp = (nibble << scale) + ((c1 * h1 + c2 * h2 + 1024) >> 11);
            if (samp >  32767) samp =  32767;
            if (samp < -32768) samp = -32768;

            out[base + n] = (float)samp / 32768.0f;
            h2 = h1;
            h1 = samp;
        }
    }
    return out;
}

// Trigger (re-)decode of an ADPCM voice.
// Called after coefs and offset info are both set.
// The voice must be stopped before calling (caller's responsibility).
static void ax_adpcm_decode(AXVoiceState& vs) {
    if (vs.format != AX_FMT_ADPCM) return;
    if (!vs.pcm_addr || vs.end_offs == 0) return;
    if (vs.pcm_addr + vs.end_offs / ADPCM_SAMPLES_PER_FRAME * ADPCM_BYTES_PER_FRAME
            > 0xC0000000u) return;

    uint32_t num_frames = (vs.end_offs + ADPCM_SAMPLES_PER_FRAME - 1)
                          / ADPCM_SAMPLES_PER_FRAME;
    uint32_t byte_count = num_frames * ADPCM_BYTES_PER_FRAME;

    // Atomically swap in new buffer
    vs.adpcm_ready.store(false);
    free(vs.adpcm_buf);

    uint32_t new_len = 0;
    float* new_buf = adpcm_decode_stream(
        s_mem + vs.pcm_addr,
        byte_count,
        vs.adpcm_coefs,
        vs.adpcm_hist1,
        vs.adpcm_hist2,
        &new_len);

    vs.adpcm_buf = new_buf;
    vs.adpcm_len = new_len;
    vs.adpcm_ready.store(new_buf != nullptr);
}

// ---------------------------------------------------------------------------
// Handle ↔ slot index helpers
// ---------------------------------------------------------------------------
static int voice_index(uint32_t handle) {
    if (handle < AX_VOICE_BASE) return -1;
    uint32_t idx = (handle - AX_VOICE_BASE) / AX_VOICE_HANDLE_STRIDE;
    if (idx >= AX_VOICE_COUNT) return -1;
    return (int)idx;
}

static uint32_t voice_handle(uint32_t idx) {
    return AX_VOICE_BASE + idx * AX_VOICE_HANDLE_STRIDE;
}

// ---------------------------------------------------------------------------
// miniaudio data callback — mixes all playing voices to stereo f32 output
// ---------------------------------------------------------------------------
#if defined(HAVE_MINIAUDIO)
static ma_device s_ma_device;

static void ax_audio_callback(ma_device* device,
                               void*       output,
                               const void* /*input*/,
                               ma_uint32   frameCount) {
    (void)device;
    float* out = (float*)output;
    memset(out, 0, frameCount * 2 * sizeof(float));

    if (!s_mem) return;

    for (uint32_t v = 0; v < AX_VOICE_COUNT; v++) {
        AXVoiceState& vs = s_voices[v];
        if (!vs.active.load() || !vs.playing.load()) continue;
        if (vs.pcm_addr == 0 || vs.end_offs == 0) continue;

        uint32_t fmt       = vs.format;
        uint32_t pcm_addr  = vs.pcm_addr;
        uint32_t loop_offs = vs.loop_offs;
        uint32_t end_offs  = vs.end_offs;
        float    ratio     = vs.src_ratio;
        float    vol_l     = vs.vol * vs.mix_l;
        float    vol_r     = vs.vol * vs.mix_r;

        // Reconstruct continuous position from atomic integer part + saved fraction
        float pos = (float)vs.pos_int.load() + vs.pos_frac;

        for (ma_uint32 f = 0; f < frameCount; f++) {
            // Advance volume envelope
            vs.vol += vs.vol_delta;
            if (vs.vol < 0.0f) vs.vol = 0.0f;
            if (vs.vol > 1.0f) vs.vol = 1.0f;

            // Check end / loop
            if ((uint32_t)pos >= end_offs) {
                if (vs.loop) {
                    pos = (float)loop_offs + (pos - (float)end_offs);
                } else {
                    vs.playing.store(false);
                    break;
                }
            }

            // Sample read
            float sample = 0.0f;
            uint32_t si = (uint32_t)pos;
            if (fmt == AX_FMT_PCM16) {
                uint32_t byte_off = pcm_addr + si * 2u;
                if (byte_off + 1u < 0xC0000000u) {
                    int16_t raw = (int16_t)(((uint16_t)s_mem[byte_off] << 8) |
                                             (uint16_t)s_mem[byte_off + 1]);
                    sample = (float)raw / 32768.0f;
                }
            } else if (fmt == AX_FMT_PCM8) {
                uint32_t byte_off = pcm_addr + si;
                if (byte_off < 0xC0000000u) {
                    int8_t raw = (int8_t)s_mem[byte_off];
                    sample = (float)raw / 128.0f;
                }
            } else if (fmt == AX_FMT_ADPCM) {
                // Read from pre-decoded float buffer
                if (vs.adpcm_ready.load() && vs.adpcm_buf && si < vs.adpcm_len)
                    sample = vs.adpcm_buf[si];
            }

            out[f * 2 + 0] += sample * vol_l;
            out[f * 2 + 1] += sample * vol_r;
            pos += ratio;
        }

        // Save fractional position back
        uint32_t pos_i = (uint32_t)pos;
        vs.pos_int.store(pos_i);
        vs.pos_frac = pos - (float)pos_i;
    }

    // Soft-clip output to [-1, 1]
    for (ma_uint32 i = 0; i < frameCount * 2; i++) {
        if (out[i] >  1.0f) out[i] =  1.0f;
        if (out[i] < -1.0f) out[i] = -1.0f;
    }
}
#endif // HAVE_MINIAUDIO

// ---------------------------------------------------------------------------
// AX lifecycle
// ---------------------------------------------------------------------------

static void AXInit(CPUState* cpu) {
    (void)cpu;
    if (s_ax_inited) return;

    // Store the guest arena pointer for the audio callback
    s_mem = MEM;

    // Reset all voice slots
    for (uint32_t i = 0; i < AX_VOICE_COUNT; i++) {
        s_voices[i].active.store(false);
        s_voices[i].playing.store(false);
    }

#if defined(HAVE_MINIAUDIO)
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = 48000;
    cfg.dataCallback      = ax_audio_callback;

    if (ma_device_init(nullptr, &cfg, &s_ma_device) != MA_SUCCESS) {
        fprintf(stderr, "[snd_core] miniaudio: failed to open playback device\n");
    } else if (ma_device_start(&s_ma_device) != MA_SUCCESS) {
        fprintf(stderr, "[snd_core] miniaudio: failed to start playback device\n");
        ma_device_uninit(&s_ma_device);
    } else {
        fprintf(stderr, "[snd_core] AXInit: miniaudio stereo 48000 Hz device opened\n");
    }
#else
    fprintf(stderr, "[snd_core] AXInit: no audio back-end (build with HAVE_MINIAUDIO)\n");
#endif

    s_ax_inited = true;
}

static void AXQuit(CPUState* cpu) {
    (void)cpu;
#if defined(HAVE_MINIAUDIO)
    if (s_ax_inited) {
        ma_device_stop(&s_ma_device);
        ma_device_uninit(&s_ma_device);
    }
#endif
    s_ax_inited = false;
}

static void AXIsInit(CPUState* cpu)                { RET = s_ax_inited ? 1u : 0u; }
static void AXGetInputSamplesPerSec(CPUState* cpu)  { RET = 48000u; }
static void AXGetInputSamplesPerFrame(CPUState* cpu){ RET = 96u; }

static void AXRegisterAppFrameCallback(CPUState* cpu)   { RET = 0; }
static void AXDeregisterAppFrameCallback(CPUState* cpu) { RET = 0; }
static void AXRegisterFrameCallback(CPUState* cpu)      { (void)cpu; }
static void AXDeregisterFrameCallback(CPUState* cpu)    { (void)cpu; }

// ---------------------------------------------------------------------------
// Voice management
// ---------------------------------------------------------------------------

// Acquire a free voice slot.  Returns handle or 0 if pool is exhausted.
static uint32_t ax_acquire(uint32_t priority) {
    (void)priority;
    for (uint32_t i = 0; i < AX_VOICE_COUNT; i++) {
        bool expected = false;
        if (s_voices[i].active.compare_exchange_strong(expected, true)) {
            AXVoiceState& vs  = s_voices[i];
            vs.playing.store(false);
            vs.format    = 0;
            vs.pcm_addr  = 0;
            vs.loop_offs = 0;
            vs.end_offs  = 0;
            vs.pos_int.store(0);
            vs.pos_frac  = 0.0f;
            vs.src_ratio = 1.0f;
            vs.vol       = 1.0f;
            vs.vol_delta = 0.0f;
            vs.mix_l     = 1.0f;
            vs.mix_r     = 1.0f;
            vs.loop      = false;
            // Clear ADPCM state
            memset(vs.adpcm_coefs, 0, sizeof(vs.adpcm_coefs));
            vs.adpcm_hist1 = vs.adpcm_hist2 = 0;
            vs.adpcm_loop_hist1 = vs.adpcm_loop_hist2 = 0;
            vs.adpcm_coefs_valid = false;
            vs.adpcm_ready.store(false);
            free(vs.adpcm_buf); vs.adpcm_buf = nullptr; vs.adpcm_len = 0;
            return voice_handle(i);
        }
    }
    fprintf(stderr, "[snd_core] AXAcquireVoice: voice pool exhausted\n");
    return 0;
}

static void AXAcquireVoice(CPUState* cpu) {
    RET = ax_acquire(ARG0);
}

static void AXAcquireVoiceEx(CPUState* cpu) {
    RET = ax_acquire(ARG0);
}

static void AXFreeVoice(CPUState* cpu) {
    int i = voice_index(ARG0);
    if (i < 0) return;
    s_voices[i].playing.store(false);
    s_voices[i].active.store(false);
}

static void AXIsVoiceRunning(CPUState* cpu) {
    int i = voice_index(ARG0);
    RET = (i >= 0 && s_voices[i].playing.load()) ? 1u : 0u;
}

// AXSetVoiceState(voice, state)  state: 0=stopped, 1=running
static void AXSetVoiceState(CPUState* cpu) {
    int i = voice_index(ARG0);
    if (i < 0) return;
    s_voices[i].playing.store(ARG1 == AX_VOICE_STATE_RUNNING);
}

static void AXSetVoiceType(CPUState* cpu) { (void)cpu; }

// AXSetVoiceOffsets(voice, AXVoiceOffsets*)
// Reads the offset struct from guest memory and updates the voice.
static void AXSetVoiceOffsets(CPUState* cpu) {
    int i = voice_index(ARG0);
    uint32_t offs_ptr = ARG1;
    if (i < 0 || !offs_ptr) return;

    AXVoiceState& vs = s_voices[i];
    // Pause before modifying to avoid mid-update artifacts in the callback
    bool was_playing = vs.playing.exchange(false);

    vs.format    = (rbrew_read32(MEM, offs_ptr + AXVO_FORMAT) >> 16) & 0xFFu;
    // The format field: upper 16 bits hold format in some AX versions.
    // Try reading as uint16 at offset 0 instead (big-endian u16):
    {
        uint16_t fmt_raw = (uint16_t)(rbrew_read32(MEM, offs_ptr) >> 16);
        vs.format = fmt_raw & 0xFFu;
        // Normalise: 0=ADPCM, 0x0A=PCM8, 0x1A=PCM16
        if (fmt_raw == 0x00) vs.format = AX_FMT_ADPCM;
        else if (fmt_raw == 0x0A) vs.format = AX_FMT_PCM8;
        else vs.format = AX_FMT_PCM16;
    }

    uint16_t loop_flag  = (uint16_t)(rbrew_read32(MEM, offs_ptr) & 0xFFFFu);
    vs.loop      = (loop_flag != 0);
    vs.loop_offs = rbrew_read32(MEM, offs_ptr + AXVO_LOOP_OFFSET);
    vs.end_offs  = rbrew_read32(MEM, offs_ptr + AXVO_END_OFFSET);
    uint32_t cur = rbrew_read32(MEM, offs_ptr + AXVO_CUR_OFFSET);
    vs.pos_int.store(cur);
    vs.pos_frac  = 0.0f;
    vs.pcm_addr  = rbrew_read32(MEM, offs_ptr + AXVO_DATA);

    // If this is an ADPCM voice and coefficients are already set, pre-decode now.
    // The voice is stopped (was_playing exchanged false above) so the callback
    // won't be reading the buffer during decode.
    if (vs.format == AX_FMT_ADPCM && vs.pcm_addr && vs.end_offs && vs.adpcm_coefs_valid)
        ax_adpcm_decode(vs);

    vs.playing.store(was_playing);
}

// AXSetVoiceSrcType(voice, type) — type 0=none, 1=linear, 2=4-tap
static void AXSetVoiceSrcType(CPUState* cpu) { (void)cpu; }

// AXSetVoiceSrc(voice, AXVoiceSrc*)  — ratio is Q16.16 fixed point
static void AXSetVoiceSrc(CPUState* cpu) {
    int i = voice_index(ARG0);
    uint32_t src_ptr = ARG1;
    if (i < 0 || !src_ptr) return;
    uint32_t ratio_fixed = rbrew_read32(MEM, src_ptr + AXSRC_RATIO);
    // Q16.16 → float, then express as source_samples_per_output_sample
    float ratio = (float)ratio_fixed / 65536.0f;
    if (ratio <= 0.0f) ratio = 1.0f;
    s_voices[i].src_ratio = ratio;
}

// AXSetVoiceSrcRatio(voice, float_ratio)
static void AXSetVoiceSrcRatio(CPUState* cpu) {
    int i = voice_index(ARG0);
    if (i < 0) return;
    // f1 holds the ratio as a float (PPC float arg convention)
    float ratio = (float)cpu->f[1];
    if (ratio <= 0.0f) ratio = 1.0f;
    s_voices[i].src_ratio = ratio;
}

// AXSetVoiceVe(voice, AXVoiceVe*)  — Q15 volume + per-sample delta
static void AXSetVoiceVe(CPUState* cpu) {
    int i = voice_index(ARG0);
    uint32_t ve_ptr = ARG1;
    if (i < 0 || !ve_ptr) return;
    uint32_t words = rbrew_read32(MEM, ve_ptr + AXVE_VOLUME);
    uint16_t vol_q15  = (uint16_t)(words >> 16);
    int16_t  delta_q  = (int16_t)(words & 0xFFFFu);
    s_voices[i].vol       = (float)vol_q15 / 32768.0f;
    s_voices[i].vol_delta = (float)delta_q / 32768.0f;
}

static void AXSetVoiceVeDelta(CPUState* cpu) {
    int i = voice_index(ARG0);
    if (i < 0) return;
    int16_t d = (int16_t)(ARG1 & 0xFFFFu);
    s_voices[i].vol_delta = (float)d / 32768.0f;
}

// AXSetVoiceAdpcm(voice, AXVoiceAdpcm*)
// AXVoiceAdpcm guest struct layout (big-endian, all int16):
//   +0x00..+0x1F : 16 x int16  coefficients (8 pairs: c1[0],c2[0],...,c1[7],c2[7])
//   +0x20        : int16  gain        (ignored here)
//   +0x22        : int16  pred_scale  (initial predictor/scale byte, ignored)
//   +0x24        : int16  yn1         (initial history sample h1)
//   +0x26        : int16  yn2         (initial history sample h2)
static void AXSetVoiceAdpcm(CPUState* cpu) {
    int i = voice_index(ARG0);
    uint32_t adpcm_ptr = ARG1;
    if (i < 0 || !adpcm_ptr) return;

    AXVoiceState& vs = s_voices[i];

    // Read 16 big-endian int16 coefficients
    for (int k = 0; k < 16; k++) {
        uint32_t word = rbrew_read32(MEM, adpcm_ptr + (uint32_t)(k & ~1) * 2u);
        vs.adpcm_coefs[k] = (k & 1)
            ? (int16_t)(word & 0xFFFFu)
            : (int16_t)(word >> 16);
    }

    // Read initial history (yn1/yn2 at +0x24/+0x26)
    uint32_t hist_word = rbrew_read32(MEM, adpcm_ptr + 0x24u);
    vs.adpcm_hist1 = (int16_t)(hist_word >> 16);
    vs.adpcm_hist2 = (int16_t)(hist_word & 0xFFFFu);
    vs.adpcm_coefs_valid = true;

    // If offsets already configured, trigger decode now
    if (vs.format == AX_FMT_ADPCM && vs.pcm_addr && vs.end_offs)
        ax_adpcm_decode(vs);
}

// AXSetVoiceAdpcmLoop(voice, AXVoiceAdpcmLoopData*)
// AXVoiceAdpcmLoopData guest struct (big-endian):
//   +0x00 : int16  loop_pred_scale  (ignored)
//   +0x02 : int16  loop_yn1
//   +0x04 : int16  loop_yn2
static void AXSetVoiceAdpcmLoop(CPUState* cpu) {
    int i = voice_index(ARG0);
    uint32_t loop_ptr = ARG1;
    if (i < 0 || !loop_ptr) return;

    AXVoiceState& vs = s_voices[i];

    // pred_scale at +0x00 (upper 16 bits), loop_yn1 at +0x02 (lower 16 bits)
    uint32_t word0 = rbrew_read32(MEM, loop_ptr + 0x00u);
    uint32_t word1 = rbrew_read32(MEM, loop_ptr + 0x04u);  // yn2 at +0x04
    // word0: [pred_scale(16) | loop_yn1(16)]
    vs.adpcm_loop_hist1 = (int16_t)(word0 & 0xFFFFu);       // loop_yn1
    vs.adpcm_loop_hist2 = (int16_t)(word1 >> 16);           // loop_yn2
}

static void AXSetVoiceLoop(CPUState* cpu) {
    int i = voice_index(ARG0);
    if (i >= 0) s_voices[i].loop = (ARG1 != 0);
}

static void AXSetVoiceLoopOffset(CPUState* cpu) {
    int i = voice_index(ARG0);
    if (i >= 0) s_voices[i].loop_offs = ARG1;
}

static void AXSetVoiceEndOffset(CPUState* cpu) {
    int i = voice_index(ARG0);
    if (i >= 0) s_voices[i].end_offs = ARG1;
}

static void AXSetVoiceCurrentOffset(CPUState* cpu) {
    int i = voice_index(ARG0);
    if (i >= 0) {
        s_voices[i].pos_int.store(ARG1);
        s_voices[i].pos_frac = 0.0f;
    }
}

static void AXGetVoiceCurrentOffsetEx(CPUState* cpu) {
    int i = voice_index(ARG0);
    uint32_t cur = (i >= 0) ? s_voices[i].pos_int.load() : 0u;
    if (ARG1) rbrew_write32(MEM, ARG1, cur);
    RET = 0;
}

// AXSetVoiceMix(voice, AXVoiceMix*)
// AXVoiceMix has per-bus, per-channel volume words; we extract bus-0 L/R.
static void AXSetVoiceMix(CPUState* cpu) {
    int i = voice_index(ARG0);
    uint32_t mix_ptr = ARG1;
    if (i < 0 || !mix_ptr) return;
    // Bus 0: +0x00 = left vol (Q15 uint16), +0x02 = left delta (int16),
    //        +0x04 = right vol (Q15 uint16), +0x06 = right delta (int16)
    uint32_t lr = rbrew_read32(MEM, mix_ptr);
    s_voices[i].mix_l = (float)(lr >> 16) / 32768.0f;
    s_voices[i].mix_r = (float)(lr & 0xFFFFu) / 32768.0f;
}

// AXSetVoiceDeviceMix(voice, device, deviceId, AXVoiceDeviceMixData*)
// Per-device mix — extract first channel volume as mono approximation.
static void AXSetVoiceDeviceMix(CPUState* cpu) {
    int i = voice_index(ARG0);
    uint32_t mix_ptr = ARG3;
    if (i < 0 || !mix_ptr) return;
    // AXVoiceDeviceMixData: [vol(u16), delta(s16), ...] per channel
    uint32_t word = rbrew_read32(MEM, mix_ptr);
    float v = (float)(word >> 16) / 32768.0f;
    // Apply same volume to both channels as a conservative default
    s_voices[i].mix_l = v;
    s_voices[i].mix_r = v;
}

static void AXSetVoicePriority(CPUState* cpu) { (void)cpu; }

// Remote (Wii Remote) audio — ignore
static void AXSetVoiceRmtOn(CPUState* cpu)  { (void)cpu; }
static void AXSetVoiceRmtMix(CPUState* cpu) { (void)cpu; }
static void AXSetVoiceRmtIIR(CPUState* cpu) { (void)cpu; }
static void AXSetVoiceRmtSrc(CPUState* cpu) { (void)cpu; }

// ---------------------------------------------------------------------------
// Aux / master volume
// ---------------------------------------------------------------------------

static void AXGetMasterVolume(CPUState* cpu) { RET = 0x8000u; }
static void AXSetMasterVolume(CPUState* cpu) { (void)cpu; }
static void AXGetOutputMode(CPUState* cpu)   { RET = 1u; } // stereo
static void AXSetOutputMode(CPUState* cpu)   { (void)cpu; }

static void AXRegisterAuxCallback(CPUState* cpu)   { RET = 0; }
static void AXDeregisterAuxCallback(CPUState* cpu) { RET = 0; }
static void AXSetAuxReturnVolume(CPUState* cpu)    { (void)cpu; }

// ---------------------------------------------------------------------------
// AI (Audio Interface) — no-ops, handled by AX on Wii U
// ---------------------------------------------------------------------------

static void AIInit(CPUState* cpu)            { (void)cpu; }
static void AIQuit(CPUState* cpu)            { (void)cpu; }
static void AIGetDMALength(CPUState* cpu)    { RET = 0; }
static void AIGetDMABytesLeft(CPUState* cpu) { RET = 0; }
static void AIStartDMA(CPUState* cpu)        { (void)cpu; }
static void AIStopDMA(CPUState* cpu)         { (void)cpu; }
static void AICheckInit(CPUState* cpu)       { RET = 1; }
static void AIGetStatus(CPUState* cpu)       { RET = 0; }

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "snd_core_addrs.h"

void snd_core_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(AXInit);
    REG(AXQuit);
    REG(AXRegisterAppFrameCallback);
    REG(AXDeregisterAppFrameCallback);
    REG(AXAcquireVoiceEx);
    REG(AXFreeVoice);
    REG(AXSetVoiceState);
    REG(AXSetVoiceType);
    REG(AXSetVoiceOffsets);
    REG(AXSetVoiceSrcType);
    REG(AXSetVoiceSrc);
    REG(AXSetVoiceSrcRatio);
    REG(AXSetVoiceVe);
    REG(AXSetVoiceAdpcm);
    REG(AXSetVoiceAdpcmLoop);
    REG(AXSetVoiceLoop);
    REG(AXGetVoiceCurrentOffsetEx);
    REG(AXSetVoiceDeviceMix);
    REG(AXSetVoicePriority);
    REG(AXSetVoiceRmtOn);
    REG(AXSetVoiceRmtIIR);
    REG(AXRegisterAuxCallback);
    REG(AXSetAuxReturnVolume);
#undef REG
}
