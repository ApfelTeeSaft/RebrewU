#pragma once
// Native GX2 display lists. On the console GX2BeginDisplayListEx points the
// core's command writer at a game buffer and every GX2 command call appends
// packets until GX2EndDisplayList. Here the GX2 command calls themselves are
// recorded (function + argument registers) per core and replayed on
// GX2CallDisplayList / GX2DirectCallDisplayList - native calls, no packets.
// Lists that were not recorded (pre-built in game files) fall back to the
// read-only static decoder (gx2_pm4_state).
#include "rebrewu_runtime.h"
#include <string_view>
#include <vector>
#include <span>

using Gx2CommandFn = void (*)(CPUState*);
using Gx2DescriptorFn = void (*)(CPUState*, const uint8_t*);

// True when GX2 command calls should be recorded instead of executed: the
// calling core has an open display list, or commands are posted to the
// asynchronous render stream (gx2_native_post).
bool gx2_dl_recording();
// True only for an open display list on the calling core.
bool gx2_dl_list_open();
// Store a command call (r3..r10, f1..f8) in the open list of this core.
void gx2_dl_record(Gx2CommandFn fn, const CPUState* cpu);
// Snapshot a descriptor consumed while recording. Pointed-to image storage
// remains guest-owned and is read when the native command executes.
void gx2_dl_record_descriptor(Gx2DescriptorFn fn, const CPUState* cpu, uint32_t bytes);
void gx2_dl_record_snapshot(Gx2DescriptorFn fn,const CPUState* cpu,std::span<const uint8_t> bytes);
// GX2Set{Vertex,Pixel}UniformReg while a list is open: the console copies
// the values into the list, so record a snapshot (native-order words).
void gx2_dl_record_uniform_words(bool is_pixel, uint32_t index, std::vector<uint32_t> words);
void gx2_dl_call(CPUState* cpu, uint32_t list, uint32_t size);
uint32_t gx2_dl_create_snapshot(CPUState* cpu, uint32_t buffer, uint32_t capacity,
                                Gx2DescriptorFn fn, std::span<const uint8_t> snapshot);

// GX2 calls that check for an open list themselves and then only append a
// snapshot to it (uniform values, or a descriptor consumed at record time).
constexpr bool gx2_records_itself(std::string_view name) {
    for (std::string_view snapshot : {"GX2SetVertexUniformReg", "GX2SetPixelUniformReg",
                                      "GX2SetColorBuffer", "GX2SetDepthBuffer", "GX2SetTargetChannelMasksReg",
                                      "GX2SetDepthStencilControlReg", "GX2SetPolygonControlReg",
                                      "GX2SetColorControlReg", "GX2SetBlendControlReg",
                                      "GX2SetAlphaTestReg",
                                      "GX2SetPixelSampler", "GX2SetVertexSampler", "GX2SetGeometrySampler",
                                      "GX2SetViewportReg", "GX2SetScissorReg", "GX2SetPolygonOffsetReg",
                                      "GX2SetStencilMaskReg", "GX2SetBlendConstantColorReg", "GX2SetDepthStencilControl",
                                      "GX2CopySurface", "GX2ConvertDepthBufferToTextureSurface",
                                      "GX2SetPolygonControl",
                                      "GX2SetPixelTexture", "GX2SetVertexTexture", "GX2SetGeometryTexture",
                                      "GX2SetStreamOutBuffer", "GX2SetStreamOutContext", "GX2SaveStreamOutContext", "GX2QueryBegin",
                                      "GX2ClearColor", "GX2ClearDepthStencilEx", "GX2ClearBuffersEx"})
        if (name == snapshot) return true;
    return false;
}

// GX2 calls that write command packets on the console (and so belong in a
// display list). Queries/Calc/Init and display-controller settings run
// immediately even while a list is open.
// Commands whose callers need their effects at once (return values, guest
// memory writes, per-core state): recorded only into real display lists.
constexpr bool gx2_sync_command(std::string_view name) {
    return name == "GX2DrawDone" || name == "GX2SetContextState" || name == "GX2SetupContextStateEx";
}

constexpr bool gx2_is_command(std::string_view name) {
    if (gx2_records_itself(name)) return false;
    for (std::string_view immediate : {"GX2SetTVEnable", "GX2SetDRCEnable", "GX2SetTVGamma",
                                       "GX2SetTVBuffer", "GX2SetDRCBuffer",
                                       "GX2SetDRCScale", "GX2SetSwapInterval"})
        if (name == immediate) return false;
    for (std::string_view prefix : {"GX2Set", "GX2Draw", "GX2Clear", "GX2CopySurface",
                                    "GX2QueryBegin", "GX2QueryEnd",
                                    "GX2CallDisplayList", "GX2DirectCallDisplayList"})
        if (name.substr(0, prefix.size()) == prefix) return true;
    return false;
}

// GPU-free wrapper (builds without SDL): record while a list is open, else run.
template<Gx2CommandFn Function, bool Command> void gx2_command_call(CPUState* cpu) {
    if constexpr (Command) {
        if (gx2_dl_recording()) { gx2_dl_record(Function, cpu); return; }
    }
    Function(cpu);
}

void gx2_display_list_register(CPUState* cpu);
// Diagnostic count for lossless cold-list archive I/O.
uint64_t gx2_dl_archive_seek_count();
// Diagnostic count of context snapshots that reused an identical recording.
uint64_t gx2_dl_snapshot_reuse_count();
