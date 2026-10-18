#pragma once
// Native GX2 calls and the single Vulkan render queue.
//
// The game issues hundreds of GX2 calls per frame from several threads, and
// its frame thread busy-waits on guest memory for jobs the other threads run.
// Making the context current around every call cost more than a frame, and a
// context left current on the last caller starves a thread that never blocks.
// So once the window exists (gx2_native_start_render_thread) one native render
// thread keeps the context and runs every call that touches the renderer
// while the calling guest thread waits. It never runs guest code, so it can
// always make progress. Before that, and in tests, a call runs on its caller
// under the renderer lock and borrows the context for the call.
#include "gx2_display_list.h"
#include "gx2_perf.h"
#include <string_view>
#include <type_traits>
void gx2_native_set_context(void* window, void* context);  // current on the caller
void* gx2_native_window();   // window given to gx2_native_set_context
void gx2_native_start_render_thread();
void gx2_native_stop_render_thread();  // the context returns to the caller
void gx2_native_enter(bool gpu = true);
void gx2_native_leave();
void gx2_native_gpu_flush();
bool gx2_native_gpu_finish();
uint64_t gx2_native_gpu_flush_count();
uint64_t gx2_native_gpu_finish_count();
struct GX2NativeScope {
    explicit GX2NativeScope(bool gpu = true) { gx2_native_enter(gpu); }
    ~GX2NativeScope() { gx2_native_leave(); }
    GX2NativeScope(const GX2NativeScope&) = delete;
};

// Run task on the render thread, or here under the renderer lock (with the
// context when gpu). Exceptions reach the caller.
using Gx2Task = void (*)(void*);
#if defined(HAVE_SDL2)
void gx2_native_execute(bool gpu, Gx2Task task, void* context);
#else
inline void gx2_native_execute(bool, Gx2Task task, void* context) { task(context); }
#endif
template<class F> void gx2_native_run(bool gpu, F&& f) {
    gx2_native_execute(gpu, [](void* p) { (*static_cast<std::remove_reference_t<F>*>(p))(); }, &f);
}

// Asynchronous command stream. On the console GX2 command calls only append
// packets that the GPU consumes later, so the CPU never waits for them. Work
// posted here runs on the render thread in submission order, interleaved
// with gx2_native_execute tasks (which still wait), and the poster continues
// at once. Only available while the render thread runs (Vulkan back-end);
// RBREW_GX2_ASYNC=0 restores the fully synchronous model.
struct Gx2AsyncWork {
    virtual ~Gx2AsyncWork() = default;
    virtual void run() = 0;
};
#if defined(HAVE_SDL2)
bool gx2_native_async_active();               // this thread may post
void gx2_native_post(Gx2AsyncWork* work);     // takes ownership
#else
// Builds without a window (front-end unit tests) run everything on the caller.
inline bool gx2_native_async_active() { return false; }
inline void gx2_native_post(Gx2AsyncWork* work) { work->run(); delete work; }
#endif

// What a GX2 entry point needs besides guest memory. None: display-list
// bookkeeping, Calc/Init/Get queries, vsync waits and window events - runs
// on the caller. State: renderer bookkeeping without GPU work. Gpu: the rest.
enum class Gx2Access { None, State, Gpu };
constexpr Gx2Access gx2_access(std::string_view name) {
    if (name == "GX2InitFetchShaderEx") return Gx2Access::State;
    // GX2Invalidate posts its texture work itself; depth expansion has none.
    if (name == "GX2Invalidate" || name == "GX2ExpandDepthBuffer") return Gx2Access::None;
    for (std::string_view free : {"GX2BeginDisplayListEx", "GX2EndDisplayList", "GX2PatchDisplayList",
                                  "GX2WaitForVsync", "GX2DrawDone", "GX2Flush", "GX2TempGetGPUVersion",
                                  "GX2CopyColorBufferToScanBuffer",
                                  "GX2SwapScanBuffers"})
        if (name == free) return Gx2Access::None;
    for (std::string_view query : {"GX2Calc", "GX2Init", "GX2Get"})
        if (name.substr(0, query.size()) == query) return Gx2Access::None;
    return Gx2Access::Gpu;
}

// Command calls made while this core has an open display list are recorded
// for replay (gx2_display_list) instead of being executed; snapshot calls
// record themselves. Neither touches the renderer.
template<void(*Function)(struct CPUState*), bool Command = false, bool RecordsItself = false,
         Gx2Access Access = Gx2Access::Gpu, bool Sync = false>
void gx2_native_call(CPUState* cpu) {
    if constexpr (Command || RecordsItself) {
        if (Sync ? gx2_dl_list_open() : gx2_dl_recording()) {
            if constexpr (Command) gx2_dl_record(Function, cpu);
            else Function(cpu);
            return;
        }
    }
    if constexpr (Access == Gx2Access::None) {
        Function(cpu);
    } else {
        const uint64_t start = gx2_perf_enabled() ? gx2_perf_now() : 0;
        gx2_native_execute(Access == Gx2Access::Gpu, [](void* c) { Function(static_cast<CPUState*>(c)); }, cpu);
        if (start) gx2_perf_function(1, reinterpret_cast<const void*>(Function), gx2_perf_now() - start);
    }
}
#define GX2_NATIVE_CALL(name) \
    gx2_native_call<name, gx2_is_command(#name), gx2_records_itself(#name), gx2_access(#name), gx2_sync_command(#name)>
