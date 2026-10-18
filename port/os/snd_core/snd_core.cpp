#include "../../runtime/rbrew_diagnostics.h"
// snd_core / AX audio
//
// Native snd_core: 96 voices, 96-sample frames at 32 kHz, and miniaudio
// stereo output. Guest voice headers and offsets retain the Cafe ABI.
//
// Nintendo DSP ADPCM (GameCube/Wii/Wii U):
//   Frame layout: 1 header byte + 7 data bytes = 14 4-bit samples per frame.
//   Header byte: bits[7:4] = coefficient-pair index (0-7),
//                bits[3:0] = shift amount (scale).
//   Decode: samp = (nibble_signed << scale) + (c1*h1 + c2*h2 + 1024) >> 11
//   Coefficients are signed Q1.11 fixed-point (as in the Wii U AX API).
//
// ADPCM is decoded incrementally: Cafe offsets count nibbles, including the
// two header nibbles per frame. Loop histories are restored at the loop point.
//
// Without miniaudio: voice state is tracked but no audio is output.

#include "../os_common.h"
#include "../gx2/gx2_perf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <array>
#include <mutex>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <filesystem>
#include <string>

// ---------------------------------------------------------------------------
// miniaudio — optional header-only back-end
// ---------------------------------------------------------------------------
#if defined(HAVE_MINIAUDIO)
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#endif

// ---------------------------------------------------------------------------
// Voice format constants (AXVoiceFormat)
// ---------------------------------------------------------------------------
static constexpr uint32_t AX_FMT_ADPCM  = 0x00u;
static constexpr uint32_t AX_FMT_PCM8   = 0x19u;
static constexpr uint32_t AX_FMT_PCM16  = 0x0Au;

// AX voice state constants
static constexpr uint32_t AX_VOICE_STATE_STOPPED = 0;
static constexpr uint32_t AX_VOICE_STATE_RUNNING  = 1;

// ---------------------------------------------------------------------------
// AXVoiceOffsets guest struct layout (big-endian)
//   +0x00  uint16  format   (AX_FMT_*)
//   +0x02  uint16  loopFlag (0=no loop, 1=loop)
//   +0x04  uint32  loopOffset  (in samples)
//   +0x08  uint32  endOffset   (inclusive; ADPCM offsets count nibbles)
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
static uint32_t s_voice_base = 0;
static constexpr uint32_t AX_VOICE_HANDLE_STRIDE  = 0x80u; // includes the 0x58-byte AXVoice

struct AXVoiceState {
    std::atomic<bool> active{false};   // acquired (not free)
    std::atomic<bool> playing{false};  // currently producing audio
    bool reserved=false; // allocation callback in progress; not available to another acquire

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
    float mix_l        = 0.0f;
    float mix_r        = 0.0f;
    float mix_delta_l  = 0.0f;
    float mix_delta_r  = 0.0f;
    uint32_t priority = 0, callback = 0, user_context = 0, loop_count = 0;
    uint32_t src_type = 1, type = 0;
    float device_gain[3][4][6][4]{};
    float device_delta[3][4][6][4]{};
    bool  loop         = false;

    // Nintendo DSP ADPCM state
    int16_t adpcm_coefs[16]   = {};  // 8 coefficient pairs (c1, c2)
    int16_t adpcm_hist1       = 0;   // yn1 — set from AXSetVoiceAdpcm
    int16_t adpcm_hist2       = 0;   // yn2
    int16_t adpcm_loop_hist1  = 0;   // yn1 at loop start
    int16_t adpcm_loop_hist2  = 0;   // yn2 at loop start
    bool    adpcm_coefs_valid = false;

    uint16_t adpcm_pred_scale = 0, adpcm_loop_pred_scale = 0;
    float adpcm_sample = 0;
    bool adpcm_sample_valid = false;
};

static AXVoiceState s_voices[AX_VOICE_COUNT];
static uint8_t*     s_mem = nullptr;       // guest arena (set at AXInit time)
static uint64_t s_memory_size = 0;
static uint32_t s_dropped_voices = 0;
static std::atomic<bool> s_ax_inited{false};
static std::recursive_mutex s_audio_mutex;
static std::recursive_mutex s_render_audio_mutex;
static std::mutex s_callback_mutex;
static std::array<uint32_t,64> s_app_callbacks{};
static std::array<uint32_t,2> s_final_callbacks{};
static uint32_t s_exceed_callback=0;
static CPUState s_callback_cpu{};
static uint32_t s_audio_storage=0;
static std::array<float,96*2> s_frame_output{};
static float s_device_output[3][4][6][96]{};
static float s_aux_input[3][4][3][6][96]{},s_aux_previous[3][4][3][6][96]{};
struct AuxState {uint32_t callback=0,context=0;float volume=1;};
static AuxState s_aux[3][4][3]{};
static uint32_t s_frame_cursor=96;
static std::atomic<uint32_t> s_audio_core{1};
uint32_t coreinit_alloc_system_block(uint32_t size);

// AXInit (snd_core, rather than snd_core2's selectable renderer) consumes
// 96 input samples per 3 ms at 32 kHz. The host device resamples as needed.
static void ax_frame_callbacks() {
    for(unsigned i=0;i<s_app_callbacks.size();++i) {
        uint32_t callback;
        { std::lock_guard lock(s_callback_mutex); callback=s_app_callbacks[i]; }
        if(callback) { s_callback_cpu.lr=0; rbrew_dispatch(&s_callback_cpu,callback); }
    }
}

// ---------------------------------------------------------------------------
// Streaming samples and Cafe voice headers.
// ---------------------------------------------------------------------------
static int voice_index(uint32_t handle) {
    if (!s_voice_base || handle < s_voice_base ||
        (handle-s_voice_base)%AX_VOICE_HANDLE_STRIDE) return -1;
    const uint32_t index=(handle-s_voice_base)/AX_VOICE_HANDLE_STRIDE;
    return index<AX_VOICE_COUNT?int(index):-1;
}
static uint32_t voice_handle(uint32_t index) {
    return s_voice_base+index*AX_VOICE_HANDLE_STRIDE;
}
static bool audio_bytes(uint64_t address,uint64_t count) {
    return address<=s_memory_size && count<=s_memory_size-address &&
           rbrew_memory_accessible(s_mem,uint32_t(address),count);
}
static void publish_voice(unsigned index) {
    auto& v=s_voices[index]; const uint32_t handle=voice_handle(index);
    rbrew_write32(s_mem,handle+4,v.playing.load()?1:0);
    rbrew_write16(s_mem,handle+0x34,uint16_t(v.format));
    rbrew_write16(s_mem,handle+0x36,v.loop?1:0);
    rbrew_write32(s_mem,handle+0x38,v.loop_offs);
    rbrew_write32(s_mem,handle+0x3C,v.end_offs);
    rbrew_write32(s_mem,handle+0x40,v.pos_int.load());
    rbrew_write32(s_mem,handle+0x44,v.pcm_addr);
}
static uint32_t next_offset(const AXVoiceState& v,uint32_t pos) {
    return v.format==AX_FMT_ADPCM && ((v.pcm_addr*2u+pos)&15)==15?pos+3:pos+1;
}
// Cafe Ex APIs can rebase onto the upcoming stream buffer while the decoder
// still reads the preceding one. Relative offsets wrap, so add in guest
// address units before widening for host bounds checks.
static uint32_t sample_address(const AXVoiceState& v,uint32_t pos) {
    if(v.format==AX_FMT_ADPCM) return (v.pcm_addr*2u+pos)>>1;
    return v.pcm_addr+pos*(v.format==AX_FMT_PCM16?2u:1u);
}
static float decode_adpcm(const AXVoiceState& v,uint32_t pos,int16_t& h1,int16_t& h2,uint16_t& predictor) {
    const uint64_t address=sample_address(v,pos);
    if (!audio_bytes(address,1)) return 0;
    if (((v.pcm_addr*2u+pos)&15)==2) {
        if (!audio_bytes(address-1,1)) return 0;
        predictor=s_mem[address-1];
    }
    const uint8_t packed=s_mem[address];
    int nibble=(pos&1)?packed&15:packed>>4;
    if(nibble&8) nibble-=16;
    const unsigned coefficient=(predictor>>4)&7;
    const int64_t predicted=int64_t(v.adpcm_coefs[coefficient*2])*h1+
                            int64_t(v.adpcm_coefs[coefficient*2+1])*h2+1024;
    const int64_t raw=int64_t(nibble)*(int64_t(1)<<(predictor&15))+(predicted>>11);
    const int16_t value=int16_t(std::clamp<int64_t>(raw,-32768,32767));
    h2=h1;h1=value;
    return float(value)/32768.0f;
}
static float sample_at(AXVoiceState& v) {
    const uint32_t pos=v.pos_int.load();
    if(v.format==AX_FMT_ADPCM) {
        if(!v.adpcm_sample_valid) {
            v.adpcm_sample=decode_adpcm(v,pos,v.adpcm_hist1,v.adpcm_hist2,v.adpcm_pred_scale);
            v.adpcm_sample_valid=true;
        }
        return v.adpcm_sample;
    }
    const uint64_t address=sample_address(v,pos);
    if(v.format==AX_FMT_PCM16 && audio_bytes(address,2))
        return float(int16_t((uint16_t(s_mem[address])<<8)|s_mem[address+1]))/32768.0f;
    if(v.format==AX_FMT_PCM8 && audio_bytes(address,1))
        return float(int8_t(s_mem[address]))/128.0f;
    return 0;
}
static float next_sample(AXVoiceState& v,float current) {
    uint32_t next=next_offset(v,v.pos_int.load());
    int16_t h1=v.adpcm_hist1,h2=v.adpcm_hist2;uint16_t predictor=v.adpcm_pred_scale;
    if(v.pos_int.load()==v.end_offs) {
        if(!v.loop) return current;
        next=v.loop_offs;
        if(!v.type) {h1=v.adpcm_loop_hist1;h2=v.adpcm_loop_hist2;}
        predictor=v.adpcm_loop_pred_scale;
    }
    if(v.format==AX_FMT_ADPCM) return decode_adpcm(v,next,h1,h2,predictor);
    const uint64_t address=sample_address(v,next);
    if(v.format==AX_FMT_PCM16 && audio_bytes(address,2))
        return float(int16_t((uint16_t(s_mem[address])<<8)|s_mem[address+1]))/32768.0f;
    if(v.format==AX_FMT_PCM8 && audio_bytes(address,1)) return float(int8_t(s_mem[address]))/128.0f;
    return 0;
}
static void advance_sample(AXVoiceState& v) {
    // Decode skipped source samples as well, so ratios >1 keep ADPCM history.
    if(v.format==AX_FMT_ADPCM) (void)sample_at(v);
    uint32_t next=next_offset(v,v.pos_int.load());
    if(v.pos_int.load()==v.end_offs) {
        if(!v.loop) {v.playing=false;return;}
        next=v.loop_offs;++v.loop_count;
        if(!v.type) {v.adpcm_hist1=v.adpcm_loop_hist1;v.adpcm_hist2=v.adpcm_loop_hist2;}
        v.adpcm_pred_scale=v.adpcm_loop_pred_scale;
    }
    v.pos_int=next;v.adpcm_sample_valid=false;
}
static void ax_mix_voices(float* out,uint32_t frames) {
    std::fill_n(out,frames*2,0.0f);
    std::memset(s_device_output,0,sizeof(s_device_output));
    std::memset(s_aux_input,0,sizeof(s_aux_input));
    if(!s_mem) return;
    for(unsigned index=0;index<AX_VOICE_COUNT;++index) {
        auto& v=s_voices[index];
        if(!v.active.load() || !v.playing.load() || !v.pcm_addr) continue;
        struct Route {float* output;float* gain;float delta;};
        std::array<Route,72> routes{};unsigned route_count=0;
        for(unsigned device=0;device<3;++device) {
            const unsigned count=device==0?1:device==1?2:4;
            const unsigned channels=device==0?6:device==1?4:1;
            for(unsigned id=0;id<count;++id) for(unsigned c=0;c<channels;++c) for(unsigned bus=0;bus<4;++bus) {
                float& gain=v.device_gain[device][id][c][bus];
                const float delta=v.device_delta[device][id][c][bus];
                if(gain!=0 || delta!=0) routes[route_count++]={bus?s_aux_input[device][id][bus-1][c]:
                    s_device_output[device][id][c],&gain,delta};
            }
        }
        for(unsigned f=0;f<frames && v.playing.load();++f) {
            const float current=sample_at(v);
            const float sample=v.src_type && v.pos_frac>0?
                current+(next_sample(v,current)-current)*v.pos_frac:current;
            for(unsigned r=0;r<route_count;++r) {
                auto& route=routes[r];route.output[f]+=sample*v.vol*(*route.gain);
                *route.gain=std::clamp(*route.gain+route.delta,0.0f,65535.0f/32768.0f);
            }
            v.vol=std::clamp(v.vol+v.vol_delta,0.0f,65535.0f/32768.0f);
            v.mix_l=std::clamp(v.mix_l+v.mix_delta_l,0.0f,1.0f);
            v.mix_r=std::clamp(v.mix_r+v.mix_delta_r,0.0f,1.0f);
            v.pos_frac+=v.src_ratio;
            while(v.pos_frac>=1 && v.playing.load()) {
                v.pos_frac-=1;advance_sample(v);
            }
        }
        publish_voice(index);
    }
    for(unsigned f=0;f<frames;++f) {
        out[f*2]=s_device_output[0][0][0][f];out[f*2+1]=s_device_output[0][0][1][f];
    }
    for(unsigned f=0;f<frames*2;++f) out[f]=std::clamp(out[f],-1.0f,1.0f);
}

static int32_t mix_pcm(float sample) {
    return int32_t(std::clamp(double(sample)*32768.0,double(INT32_MIN),double(INT32_MAX)));
}
static void ax_aux_callbacks() {
    const uint32_t pointers=s_audio_storage+0x9000,info=pointers+0x20,samples=pointers+0x40;
    for(unsigned device=0;device<3;++device) {
        const unsigned count=device==0?1:device==1?2:4,channels=device==0?6:device==1?4:1;
        for(unsigned id=0;id<count;++id) for(unsigned bus=0;bus<3;++bus) {
            AuxState state;
            {std::lock_guard lock(s_callback_mutex);state=s_aux[device][id][bus];}
            for(unsigned c=0;c<channels;++c) for(unsigned n=0;n<96;++n) {
                if(state.callback) s_device_output[device][id][c][n]+=s_aux_previous[device][id][bus][c][n]*state.volume;
                else s_aux_previous[device][id][bus][c][n]=0;
            }
            if(!state.callback) continue;
            rbrew_write32(s_mem,info,channels);rbrew_write32(s_mem,info+4,96);
            for(unsigned c=0;c<channels;++c) {
                rbrew_write32(s_mem,pointers+c*4,samples+c*96*4);
                for(unsigned n=0;n<96;++n) rbrew_write32(s_mem,samples+(c*96+n)*4,
                    uint32_t(mix_pcm(s_aux_input[device][id][bus][c][n])));
            }
            s_callback_cpu.r[3]=pointers;s_callback_cpu.r[4]=state.context;s_callback_cpu.r[5]=info;s_callback_cpu.lr=0;
            rbrew_dispatch(&s_callback_cpu,state.callback);
            for(unsigned c=0;c<channels;++c) for(unsigned n=0;n<96;++n)
                s_aux_previous[device][id][bus][c][n]=float(int32_t(rbrew_read32(s_mem,samples+(c*96+n)*4)))/32768.0f;
        }
    }
}

static void ax_final_mix_callbacks(float* output) {
    // snd_core.rpl 02005b44 / 020065xx: pointer array, u16 input channels,
    // sample count, device count, and output channels (12 bytes, big-endian).
    // TV: six channels, one device. DRC: four channels, two devices.
    for(unsigned device=0;device<2;++device) {
        uint32_t callback;
        { std::lock_guard lock(s_callback_mutex); callback=s_final_callbacks[device]; }
        if(!callback) {
            // Guest PCM buffers are consumed only by a registered callback.
            // Keep the same truncation/clipping for native TV playback while
            // avoiding a round trip through guest RAM (and unused DRC data).
            if(device==0)for(unsigned n=0;n<96;++n)for(unsigned c=0;c<2;++c)
                output[n*2+c]=std::clamp(float(mix_pcm(s_device_output[0][0][c][n]))/32768.0f,-1.0f,1.0f);
            continue;
        }
        const uint32_t info=s_audio_storage+0x1000+device*0x2000;
        const unsigned channels=device?4:6, devices=device?2:1;
        const uint32_t pointers=info+0x10, samples=info+0x40;
        rbrew_write32(s_mem,info,pointers);
        rbrew_write16(s_mem,info+4,uint16_t(channels));
        rbrew_write16(s_mem,info+6,96);
        rbrew_write16(s_mem,info+8,uint16_t(devices));
        rbrew_write16(s_mem,info+10,uint16_t(channels));
        for(unsigned c=0;c<channels*devices;++c) {
            const uint32_t buffer=samples+c*96*4;
            rbrew_write32(s_mem,pointers+c*4,buffer);
            for(unsigned n=0;n<96;++n) {
                // Existing native mixer produces TV stereo. Unrouted channels
                // start silent; callbacks may populate or downmix them.
                const float sample=s_device_output[device][c/channels][c%channels][n];
                const int32_t pcm=mix_pcm(sample);
                rbrew_write32(s_mem,buffer+n*4,uint32_t(pcm));
            }
        }
        s_callback_cpu.r[3]=info;s_callback_cpu.lr=0;
        rbrew_dispatch(&s_callback_cpu,callback);
        if(device==0) {
            const unsigned count=rbrew_read16(s_mem,info+10);
            if(count>channels) throw std::runtime_error("AX final mix output channel count exceeds input channels");
            for(unsigned n=0;n<96;++n)for(unsigned c=0;c<2;++c) {
                const unsigned source=count==1?0:c;
                const int32_t pcm=source<count?int32_t(rbrew_read32(s_mem,samples+(source*96+n)*4)):0;
                output[n*2+c]=std::clamp(float(pcm)/32768.0f,-1.0f,1.0f);
            }
        }
    }
}

// Shared by the host device and deterministic tests; callback boundaries do
// not depend on the arbitrary buffer size requested by the audio driver.
void snd_core_render_frames(float* output,uint32_t frames) {
    Gx2PerfScope audio_scope(kPerfAudioRender);
    // Guest callbacks can acquire guest locks or wait for threads that call AX.
    // Hold the voice-state mutex only for mixing, never across guest execution.
    std::lock_guard render_lock(s_render_audio_mutex);
    if(!s_ax_inited) { std::fill_n(output,frames*2,0.0f);return; }
    struct CallbackScope {
        uint32_t thread=rbrew_system_thread;
        std::atomic<uint32_t>* core=rbrew_thread_core_slot;
        CallbackScope(){rbrew_system_thread=s_audio_storage;rbrew_thread_core_slot=&s_audio_core;}
        ~CallbackScope(){rbrew_system_thread=thread;rbrew_thread_core_slot=core;}
    } scope;
    while(frames) {
        if(s_frame_cursor==96) {
            {Gx2PerfScope scope(kPerfAudioCallbacks);ax_frame_callbacks();}
            if(!s_ax_inited.load()) {std::fill_n(output,frames*2,0.0f);return;}
            {std::lock_guard voice_lock(s_audio_mutex);Gx2PerfScope scope(kPerfAudioMix);ax_mix_voices(s_frame_output.data(),96);}
            {Gx2PerfScope scope(kPerfAudioCallbacks);ax_aux_callbacks();ax_final_mix_callbacks(s_frame_output.data());}
            s_frame_cursor=0;
        }
        const uint32_t count=(std::min)(uint32_t(frames),uint32_t(96-s_frame_cursor));
        std::copy_n(s_frame_output.data()+s_frame_cursor*2,count*2,output);
        output+=count*2;frames-=count;s_frame_cursor+=count;
    }
}

#if defined(HAVE_MINIAUDIO)
static ma_device s_ma_device;
static ma_context s_ma_context;
static bool s_context_open=false;
static bool s_device_open=false;
static std::atomic<uint64_t> s_device_frames{0},s_device_signal_frames{0};
static void ax_audio_callback(ma_device*,void* output,const void*,ma_uint32 frames) {
    snd_core_render_frames(static_cast<float*>(output),frames);
    s_device_frames.fetch_add(frames,std::memory_order_relaxed);
    const auto* samples=static_cast<const float*>(output);
    for(uint32_t n=0;n<frames;++n) if(samples[n*2]!=0 || samples[n*2+1]!=0)
        s_device_signal_frames.fetch_add(1,std::memory_order_relaxed);
}
#endif // HAVE_MINIAUDIO

int snd_core_list_playback_devices() {
#if defined(HAVE_MINIAUDIO)
    ma_context context;
    if(ma_context_init(nullptr,0,nullptr,&context)!=MA_SUCCESS) return 1;
    ma_device_info* devices=nullptr;ma_uint32 count=0;
    const auto result=ma_context_get_devices(&context,&devices,&count,nullptr,nullptr);
    if(result==MA_SUCCESS) for(ma_uint32 i=0;i<count;++i)
        RBREW_PRINT("%u default=%u %s\n",i,unsigned(devices[i].isDefault),devices[i].name);
    ma_context_uninit(&context);return result==MA_SUCCESS?0:2;
#else
    return 1;
#endif
}

// Native diagnostics also used by the authored device smoke test.
bool snd_core_playback_status(uint64_t* frames,uint64_t* signal_frames) {
#if defined(HAVE_MINIAUDIO)
    if(frames) *frames=s_device_frames.load();
    if(signal_frames) *signal_frames=s_device_signal_frames.load();
    return s_device_open && s_ma_device.pContext->backend!=ma_backend_null;
#else
    if(frames) *frames=0;if(signal_frames) *signal_frames=0;return false;
#endif
}
static void ax_shutdown() {
#if defined(HAVE_MINIAUDIO)
    if(s_device_open) {
        ma_device_stop(&s_ma_device);ma_device_uninit(&s_ma_device);s_device_open=false;
    }
    if(s_context_open) {ma_context_uninit(&s_ma_context);s_context_open=false;}
#endif
    std::lock_guard render_lock(s_render_audio_mutex);
    std::lock_guard voice_lock(s_audio_mutex);s_ax_inited=false;
}

// ---------------------------------------------------------------------------
// AX lifecycle
// ---------------------------------------------------------------------------

static void AXInit(CPUState* cpu) {
    (void)cpu;
    if (s_ax_inited) return;
    static bool shutdown_registered=false;
    if(!shutdown_registered) {std::atexit(ax_shutdown);shutdown_registered=true;}

    // Store the guest arena pointer for the audio callback
    s_mem = MEM;
    s_memory_size=cpu->guest_memory_size;
    s_audio_storage=coreinit_alloc_system_block(0x18000);
    if(!s_audio_storage) throw std::runtime_error("AX callback storage allocation failed");
    s_voice_base=s_audio_storage+0x5000;s_dropped_voices=0;
    if(!audio_bytes(s_voice_base,AX_VOICE_COUNT*AX_VOICE_HANDLE_STRIDE))
        throw std::runtime_error("AX voice storage exceeds guest arena");
    s_callback_cpu=*cpu;
    s_audio_core.store(rbrew_current_core());
    s_callback_cpu.r[1]=(s_audio_storage+0x18000-16)&~15u;
    rbrew_write32(s_mem,s_callback_cpu.r[1],0);
    rbrew_write32(s_mem,s_audio_storage+0x10,2); // native audio thread running
    s_app_callbacks.fill(0);s_final_callbacks.fill(0);s_exceed_callback=0;
    for(auto& device:s_aux) for(auto& id:device) for(auto& bus:id) bus=AuxState{};
    std::memset(s_aux_previous,0,sizeof(s_aux_previous));
    s_frame_cursor=96;s_ax_inited=true;

    // Reset all voice slots
    for (uint32_t i = 0; i < AX_VOICE_COUNT; i++) {
        s_voices[i].active.store(false);
        s_voices[i].playing.store(false);
    }

#if defined(HAVE_MINIAUDIO)
    s_device_frames=0;s_device_signal_frames=0;
    if(ma_context_init(nullptr,0,nullptr,&s_ma_context)!=MA_SUCCESS) {
        RBREW_LOGF(stderr,"[snd_core] Failed to initialize playback backend\n");return;
    }
    s_context_open=true;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = 32000;
    cfg.dataCallback      = ax_audio_callback;

    std::string requested;
    if(const char* device=std::getenv("RBREW_AUDIO_DEVICE")) requested=device;
    else {
        std::filesystem::path folder=std::filesystem::current_path();
#if defined(_WIN32)
        wchar_t executable[32768]{};
        if(GetModuleFileNameW(nullptr,executable,32768)) folder=std::filesystem::path(executable).parent_path();
#endif
        std::ifstream settings(folder/"audio_device.cfg");std::getline(settings,requested);
        if(!requested.empty() && requested.back()=='\r') requested.pop_back();
    }
    ma_device_id selected{};
    if(!requested.empty()) {
        ma_device_info* devices=nullptr;ma_uint32 count=0;bool found=false;
        if(ma_context_get_devices(&s_ma_context,&devices,&count,nullptr,nullptr)==MA_SUCCESS)
            for(ma_uint32 i=0;i<count;++i) if(requested==devices[i].name) {
                selected=devices[i].id;found=true;break;
            }
        if(!found) {
            RBREW_LOGF(stderr,"[snd_core] Requested playback device is unavailable: %s\n",requested.c_str());
            ma_context_uninit(&s_ma_context);s_context_open=false;return;
        }
        cfg.playback.pDeviceID=&selected;
    }

    if (ma_device_init(&s_ma_context, &cfg, &s_ma_device) != MA_SUCCESS) {
        RBREW_LOGF(stderr, "[snd_core] miniaudio: failed to open playback device\n");
    } else if (ma_device_start(&s_ma_device) != MA_SUCCESS) {
        RBREW_LOGF(stderr, "[snd_core] miniaudio: failed to start playback device\n");
        ma_device_uninit(&s_ma_device);
    } else {
        s_device_open=true;
        RBREW_LOGF(stderr, "[snd_core] AXInit: miniaudio stereo 32000 Hz, 96-sample AX frames\n");
        RBREW_LOGF(stderr,"[snd_core] Playback device: %s\n",s_ma_device.playback.name);
    }
#else
    RBREW_LOGF(stderr, "[snd_core] AXInit: no audio back-end (build with HAVE_MINIAUDIO)\n");
#endif

    s_ax_inited = true;
}

static void AXQuit(CPUState* cpu) {
    (void)cpu;
    ax_shutdown();
}

static void AXIsInit(CPUState* cpu)                { RET = s_ax_inited ? 1u : 0u; }
static void AXGetInputSamplesPerSec(CPUState* cpu)  { RET = 32000u; }
static void AXGetInputSamplesPerFrame(CPUState* cpu){ RET = 96u; }

static void AXRegisterAppFrameCallback(CPUState* cpu) {
    const uint32_t callback=ARG0;
    if(!callback){RET=uint32_t(-17);return;}
    std::unique_lock lock(s_callback_mutex,std::try_to_lock);
    if(!lock.owns_lock()){RET=uint32_t(-14);return;}
    auto slot=std::find(s_app_callbacks.begin(),s_app_callbacks.end(),0u);
    if(slot==s_app_callbacks.end()){RET=uint32_t(-15);return;}
    *slot=callback;RET=0;
}
static void AXDeregisterAppFrameCallback(CPUState* cpu) {
    const uint32_t callback=ARG0;
    if(!callback){RET=uint32_t(-17);return;}
    std::unique_lock lock(s_callback_mutex,std::try_to_lock);
    if(!lock.owns_lock()){RET=uint32_t(-14);return;}
    auto slot=std::find(s_app_callbacks.begin(),s_app_callbacks.end(),callback);
    if(slot==s_app_callbacks.end()){RET=uint32_t(-16);return;}
    *slot=0;RET=0;
}
static void AXRegisterDeviceFinalMixCallback(CPUState* cpu) {
    const uint32_t device=ARG0,callback=ARG1;
    std::lock_guard lock(s_callback_mutex);
    if(device>=2){RET=uint32_t(-1);return;}
    s_final_callbacks[device]=callback;RET=0;
}
static void AXGetDeviceFinalMixCallback(CPUState* cpu) {
    const uint32_t device=ARG0,out=ARG1;
    std::lock_guard lock(s_callback_mutex);
    if(device>=2){RET=uint32_t(-1);return;}
    rbrew_write32(MEM,out,s_final_callbacks[device]);RET=0;
}
static void AXRegisterExceedCallback(CPUState* cpu) {
    std::lock_guard lock(s_callback_mutex);s_exceed_callback=ARG0;
}
static void AXRegisterFrameCallback(CPUState* cpu)      { (void)cpu; }
static void AXDeregisterFrameCallback(CPUState* cpu)    { (void)cpu; }

// ---------------------------------------------------------------------------
// Voice management
// ---------------------------------------------------------------------------

// Acquire a Cafe voice header in owned guest storage. The title reads its
// index directly and uses it to select its own per-voice playback state.
static void reset_voice(unsigned index) {
    auto& v=s_voices[index];
    v.playing=false;v.format=0;v.pcm_addr=0;v.loop_offs=0;v.end_offs=0;
    v.pos_int=0;v.pos_frac=0;v.src_ratio=1;v.src_type=1;v.type=0;
    v.vol=1;v.vol_delta=0;v.mix_l=v.mix_r=0;v.mix_delta_l=v.mix_delta_r=0;
    v.loop=false;v.loop_count=0;v.adpcm_sample_valid=false;v.adpcm_coefs_valid=false;
    std::memset(v.device_gain,0,sizeof(v.device_gain));std::memset(v.device_delta,0,sizeof(v.device_delta));
    std::fill_n(v.adpcm_coefs,16,int16_t(0));
    v.adpcm_hist1=v.adpcm_hist2=v.adpcm_loop_hist1=v.adpcm_loop_hist2=0;
    v.adpcm_pred_scale=v.adpcm_loop_pred_scale=0;
    std::memset(s_mem+voice_handle(index),0,AX_VOICE_HANDLE_STRIDE);
    rbrew_write32(s_mem,voice_handle(index),index);
}
static uint32_t ax_acquire(CPUState* cpu,uint32_t priority,uint32_t callback,uint32_t context) {
    std::unique_lock voice_lock(s_audio_mutex);
    int index=-1;
    for(unsigned i=0;i<AX_VOICE_COUNT;++i) if(!s_voices[i].active.load() && !s_voices[i].reserved) {index=int(i);break;}
    if(index<0) {
        for(unsigned i=0;i<AX_VOICE_COUNT;++i)
            if(!s_voices[i].reserved && s_voices[i].priority<priority &&
               (index<0 || s_voices[i].priority<s_voices[index].priority)) index=int(i);
        if(index<0) return 0;
        auto& previous=s_voices[index];++s_dropped_voices;
        if(previous.callback) {
            const uint32_t displaced_callback=previous.callback,displaced_context=previous.user_context;
            previous.reserved=true;previous.playing=false;
            rbrew_write32(s_mem,voice_handle(unsigned(index))+4,0);
            const uint32_t a=cpu->r[3],b=cpu->r[4],c=cpu->r[5],lr=cpu->lr;
            cpu->r[3]=voice_handle(index);cpu->r[4]=displaced_context;cpu->r[5]=0;
            voice_lock.unlock();
            try {rbrew_dispatch(cpu,displaced_callback);} catch(...) {
                voice_lock.lock();previous.reserved=false;throw;
            }
            voice_lock.lock();previous.reserved=false;
            cpu->r[3]=a;cpu->r[4]=b;cpu->r[5]=c;cpu->lr=lr;
        }
    }
    reset_voice(unsigned(index));
    auto& v=s_voices[index];v.active=true;v.priority=priority;v.callback=callback;v.user_context=context;
    const uint32_t handle=voice_handle(index);
    rbrew_write32(s_mem,handle+0x1C,priority);
    rbrew_write32(s_mem,handle+0x24,context);
    rbrew_write32(s_mem,handle+0x48,callback);
    return handle;
}
static void AXAcquireVoiceEx(CPUState* cpu) {RET=ax_acquire(cpu,ARG0,ARG1,ARG2);}
static void AXFreeVoice(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i<0) return;
    s_voices[i].playing=false;s_voices[i].active=false;s_voices[i].priority=0;
    rbrew_write32(MEM,ARG0+4,0);rbrew_write32(MEM,ARG0+0x1C,0);
}
static void AXSetVoiceState(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i<0) return;
    s_voices[i].playing=ARG1==AX_VOICE_STATE_RUNNING;publish_voice(unsigned(i));
}
static void AXSetVoiceType(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i>=0) s_voices[i].type=uint16_t(ARG1);
}
static void AXSetVoiceOffsets(CPUState* cpu) {
    const int i=voice_index(ARG0);const uint32_t address=ARG1;
    if(i<0 || !address || !audio_bytes(address,0x14)) return;
    auto& v=s_voices[i];
    const uint16_t format=rbrew_read16(MEM,address);
    if(format!=AX_FMT_ADPCM && format!=AX_FMT_PCM8 && format!=AX_FMT_PCM16) {
        v.playing=false;publish_voice(unsigned(i));return;
    }
    v.format=format;v.loop=rbrew_read16(MEM,address+2)!=0;
    v.loop_offs=rbrew_read32(MEM,address+4);v.end_offs=rbrew_read32(MEM,address+8);
    v.pos_int=rbrew_read32(MEM,address+12);v.pcm_addr=rbrew_read32(MEM,address+16);
    v.pos_frac=0;v.adpcm_sample_valid=false;
    const uint64_t last=uint64_t(v.pcm_addr)+(format==AX_FMT_ADPCM?v.end_offs/2:
        uint64_t(v.end_offs)*(format==AX_FMT_PCM16?2:1));
    if(!v.pcm_addr || !audio_bytes(last,format==AX_FMT_PCM16?2:1)) v.playing=false;
    publish_voice(unsigned(i));
}
static void AXGetVoiceOffsets(CPUState* cpu) {
    const int i=voice_index(ARG0);const uint32_t address=ARG1;
    if(i<0 || !address || !audio_bytes(address,0x14)) return;
    publish_voice(unsigned(i));
    std::memmove(MEM+address,MEM+voice_handle(unsigned(i))+0x34,0x14);
}
static void AXGetVoiceCurrentOffsetEx(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i<0) {RET=0;return;}
    const auto& v=s_voices[i];
    const int64_t difference=int64_t(v.pcm_addr)-int64_t(ARG1);
    const int64_t offset=v.format==AX_FMT_ADPCM?difference*2:
        v.format==AX_FMT_PCM16?difference/2:difference;
    RET=uint32_t(int64_t(v.pos_int.load())+offset);
}
static void AXGetVoiceLoopCount(CPUState* cpu) {
    const int i=voice_index(ARG0);RET=i>=0?s_voices[i].loop_count:0;
}
// snd_core.rpl 0201013C: DSP stream header (0x60 bytes), requested sample
// count, and an output PCM16 buffer. This supports the game's stream seek
// helper, which reconstructs decoder history from the final decoded samples.
static void AXDecodeAdpcmData(CPUState* cpu) {
    const uint32_t data=ARG0,header=ARG1,count=ARG2,output=ARG3;
    RET=0;
    if(!data || !header || !output || !count || !audio_bytes(header,0x4A) ||
       !audio_bytes(output,uint64_t(count)*2)) return;
    const uint32_t samples=rbrew_read32(MEM,header),nibbles=rbrew_read32(MEM,header+4);
    const uint64_t expected=uint64_t(samples/14)*16+(samples%14?samples%14+2:0);
    if(!samples || expected!=nibbles || rbrew_read16(MEM,header+12)>1 ||
       rbrew_read16(MEM,header+14)!=0) return;
    AXVoiceState v;
    v.pcm_addr=data;v.format=AX_FMT_ADPCM;v.loop=rbrew_read16(MEM,header+12)!=0;
    v.loop_offs=rbrew_read32(MEM,header+16);v.end_offs=rbrew_read32(MEM,header+20);
    uint32_t pos=rbrew_read32(MEM,header+24);
    if(v.loop_offs>v.end_offs || pos>v.end_offs || !audio_bytes(uint64_t(data)+(v.end_offs>>1),1)) return;
    for(unsigned i=0;i<16;++i) v.adpcm_coefs[i]=int16_t(rbrew_read16(MEM,header+0x1C+i*2));
    v.adpcm_pred_scale=rbrew_read16(MEM,header+0x3E);
    v.adpcm_hist1=int16_t(rbrew_read16(MEM,header+0x40));v.adpcm_hist2=int16_t(rbrew_read16(MEM,header+0x42));
    v.adpcm_loop_pred_scale=rbrew_read16(MEM,header+0x44);
    v.adpcm_loop_hist1=int16_t(rbrew_read16(MEM,header+0x46));v.adpcm_loop_hist2=int16_t(rbrew_read16(MEM,header+0x48));
    uint32_t decoded=0;
    while(decoded<count) {
        if((pos&15)==0) pos+=2;
        if(pos>v.end_offs) break;
        const float sample=decode_adpcm(v,pos,v.adpcm_hist1,v.adpcm_hist2,v.adpcm_pred_scale);
        rbrew_write16(MEM,output+decoded*2,uint16_t(int16_t(sample*32768.0f)));++decoded;
        pos=next_offset(v,pos);
        if(pos>v.end_offs && v.loop) {
            pos=v.loop_offs;v.adpcm_hist1=v.adpcm_loop_hist1;v.adpcm_hist2=v.adpcm_loop_hist2;
            v.adpcm_pred_scale=v.adpcm_loop_pred_scale;
        }
    }
    RET=decoded;
}
static void AXSetVoiceSrcType(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i>=0) s_voices[i].src_type=ARG1;
}
static void AXSetVoiceSrc(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i<0 || !ARG1 || !audio_bytes(ARG1,14)) return;
    s_voices[i].src_ratio=float(rbrew_read32(MEM,ARG1))/65536.0f;
    s_voices[i].pos_frac=float(rbrew_read16(MEM,ARG1+4))/65536.0f;
}
static void AXSetVoiceSrcRatio(CPUState* cpu) {
    const int i=voice_index(ARG0);const float ratio=float(cpu->f[1]);
    if(i<0) {RET=uint32_t(-1);return;}
    if(!std::isfinite(ratio) || ratio<0) {RET=uint32_t(-1);return;}
    if(ratio>8) {RET=uint32_t(-2);return;}
    s_voices[i].src_ratio=ratio;RET=0;
}
static void AXSetVoiceVe(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i<0 || !ARG1 || !audio_bytes(ARG1,4)) return;
    s_voices[i].vol=float(rbrew_read16(MEM,ARG1))/32768.0f;
    s_voices[i].vol_delta=float(int16_t(rbrew_read16(MEM,ARG1+2)))/32768.0f;
}
static void AXSetVoiceAdpcm(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i<0 || !ARG1 || !audio_bytes(ARG1,0x28)) return;
    auto& v=s_voices[i];
    for(unsigned c=0;c<16;++c) v.adpcm_coefs[c]=int16_t(rbrew_read16(MEM,ARG1+c*2));
    v.adpcm_pred_scale=rbrew_read16(MEM,ARG1+0x22);
    v.adpcm_hist1=int16_t(rbrew_read16(MEM,ARG1+0x24));
    v.adpcm_hist2=int16_t(rbrew_read16(MEM,ARG1+0x26));
    v.adpcm_coefs_valid=true;v.adpcm_sample_valid=false;
}
static void AXSetVoiceAdpcmLoop(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i<0 || !ARG1 || !audio_bytes(ARG1,6)) return;
    auto& v=s_voices[i];v.adpcm_loop_pred_scale=rbrew_read16(MEM,ARG1);
    // snd_core 0200E038 only updates loop histories for normal voices.
    // Streaming voices keep their decoder history between consecutive blocks.
    if(!v.type) {
        v.adpcm_loop_hist1=int16_t(rbrew_read16(MEM,ARG1+2));
        v.adpcm_loop_hist2=int16_t(rbrew_read16(MEM,ARG1+4));
    }
}
static void AXSetVoiceLoop(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i>=0) {s_voices[i].loop=ARG1!=0;publish_voice(unsigned(i));}
}
static void set_sample_base(AXVoiceState& v,uint32_t base) {
    const int64_t bytes=int64_t(v.pcm_addr)-int64_t(base);
    const int64_t delta=v.format==AX_FMT_ADPCM?bytes*2:v.format==AX_FMT_PCM16?bytes/2:bytes;
    v.pos_int=uint32_t(int64_t(v.pos_int.load())+delta);
    v.end_offs=uint32_t(int64_t(v.end_offs)+delta);
    v.loop_offs=uint32_t(int64_t(v.loop_offs)+delta);
    v.pcm_addr=base;
}
static void AXSetVoiceLoopOffsetEx(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i<0) return;
    auto& v=s_voices[i];set_sample_base(v,ARG2);v.loop_offs=ARG1;publish_voice(unsigned(i));
}
static void AXSetVoiceEndOffsetEx(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i<0) return;
    auto& v=s_voices[i];set_sample_base(v,ARG2);v.end_offs=ARG1;publish_voice(unsigned(i));
}
static void AXSetVoiceDeviceMix(CPUState* cpu) {
    const int i=voice_index(ARG0);const uint32_t device=ARG1,device_index=ARG2,address=ARG3;
    if(i<0) {RET=uint32_t(-4);return;}
    if(!address) {RET=uint32_t(-3);return;}
    if(device>2) {RET=uint32_t(-1);return;}
    if(device_index>=(device==0?1u:device==1?2u:4u)) {RET=uint32_t(-2);return;}
    const unsigned channels=device==0?6:device==1?4:1;
    if(!audio_bytes(address,channels*16)) {RET=uint32_t(-3);return;}
    // Four independent buses per channel and per device. DRC updates never
    // overwrite TV gains; wet buses pass through the guest's aux processors.
    auto& v=s_voices[i];
    for(unsigned c=0;c<channels;++c) for(unsigned bus=0;bus<4;++bus) {
        v.device_gain[device][device_index][c][bus]=float(rbrew_read16(MEM,address+c*16+bus*4))/32768.0f;
        v.device_delta[device][device_index][c][bus]=float(int16_t(rbrew_read16(MEM,address+c*16+bus*4+2)))/32768.0f;
    }
    RET=0;
}
static void AXSetVoicePriority(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i>=0) {s_voices[i].priority=ARG1;rbrew_write32(MEM,ARG0+0x1C,ARG1);}
}
static void AXSetVoiceMixerSelect(CPUState* cpu) {
    const int i=voice_index(ARG0);if(i<0 || ARG1>2) {RET=uint32_t(-1);return;}
    rbrew_write32(MEM,ARG0+12,ARG1);RET=0;
}
// There is no AXUserBegin/End import in this title. No native user-protection
// transaction is active; the native mixer lock handles host synchronization.
static void AXUserIsProtected(CPUState* cpu) {RET=0;}
static void AXGetNumVoices(CPUState* cpu) {
    uint32_t count=0;for(const auto& v:s_voices) if(v.active.load() && v.playing.load()) ++count;
    RET=count;
}
static void AXGetDroppedVoiceCount(CPUState* cpu) {RET=s_dropped_voices;}
static void AXGetDspLoad(CPUState* cpu) {cpu->f[1]=0;} // mixing executes on the native CPU, no DSP
static void AXGetDeviceMode(CPUState* cpu) {
    if(ARG0>=3 || !ARG1 || !audio_bytes(ARG1,4)) {RET=uint32_t(-1);return;}
    rbrew_write32(MEM,ARG1,ARG0==2?5:0);RET=0; // snd_core stereo TV/DRC, mono Remote
}

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

static int aux_index_error(uint32_t device,uint32_t id,uint32_t bus) {
    if(device>2) return -1;
    if(id>=(device==0?1u:device==1?2u:4u)) return -2;
    return bus>2?-5:0;
}
static void AXRegisterAuxCallback(CPUState* cpu) {
    const uint32_t device=ARG0,id=ARG1,bus=ARG2,callback=ARG3,context=ARG4;
    if(const int error=aux_index_error(device,id,bus)){RET=uint32_t(error);return;}
    std::lock_guard lock(s_callback_mutex);
    s_aux[device][id][bus].callback=callback;s_aux[device][id][bus].context=context;RET=0;
}
static void AXGetAuxCallback(CPUState* cpu) {
    const uint32_t device=ARG0,id=ARG1,bus=ARG2,callback=ARG3,context=ARG4;
    if(const int error=aux_index_error(device,id,bus)){RET=uint32_t(error);return;}
    if(!callback || !context || !audio_bytes(callback,4) || !audio_bytes(context,4)){RET=uint32_t(-3);return;}
    std::lock_guard lock(s_callback_mutex);
    rbrew_write32(MEM,callback,s_aux[device][id][bus].callback);
    rbrew_write32(MEM,context,s_aux[device][id][bus].context);RET=0;
}
static void AXSetAuxReturnVolume(CPUState* cpu) {
    const uint32_t device=ARG0,id=ARG1,bus=ARG2,volume=ARG3;
    if(const int error=aux_index_error(device,id,bus)){RET=uint32_t(error);return;}
    std::lock_guard lock(s_callback_mutex);
    s_aux[device][id][bus].volume=float(uint16_t(volume))/32768.0f;RET=0;
}

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

// AXGetSwapProfile(AXPROFILE* profile, u32 max) returns how many DSP
// swap-profile records it copied. This port produces no DSP profiling
// records, so none are available (decaf sndcore2_config: returns 0). A blank
// stub left r3 = the buffer pointer, which Gambit FUN_02C304F0 used as a
// record count and looped ~0x28000000 times in FUN_02C30254.
static void AXGetSwapProfile(CPUState* cpu) { RET = 0; }

template<HostFunc Function> static void audio_voice_call(CPUState* cpu) {
    Gx2PerfScope wait(kPerfAudioVoiceWait);
    std::lock_guard lock(s_audio_mutex);
    wait.stop();
    Function(cpu);
}

void snd_core_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(AXInit);
    REG(AXGetSwapProfile);
    REG(AXQuit);
    REG(AXRegisterAppFrameCallback);
    REG(AXDeregisterAppFrameCallback);
    REG(AXRegisterDeviceFinalMixCallback);
    REG(AXGetDeviceFinalMixCallback);
    REG(AXRegisterExceedCallback);
    REG(AXGetAuxCallback);
    REG(AXAcquireVoiceEx);
#undef REG
#define REG(name) rbrew_register_func(cpu, ADDR_##name, audio_voice_call<name>)
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
    REG(AXGetVoiceOffsets);
    REG(AXGetVoiceCurrentOffsetEx);
    REG(AXGetVoiceLoopCount);
    REG(AXDecodeAdpcmData);
    REG(AXSetVoiceLoopOffsetEx);
    REG(AXSetVoiceEndOffsetEx);
    REG(AXSetVoiceDeviceMix);
    REG(AXSetVoiceMixerSelect);
    REG(AXSetVoicePriority);
    REG(AXUserIsProtected);
    REG(AXGetNumVoices);
    REG(AXGetDroppedVoiceCount);
    REG(AXGetDspLoad);
    REG(AXGetDeviceMode);
    REG(AXSetVoiceRmtOn);
    REG(AXSetVoiceRmtIIR);
    REG(AXRegisterAuxCallback);
    REG(AXSetAuxReturnVolume);
#undef REG
}
