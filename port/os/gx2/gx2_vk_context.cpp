// gx2_vk_context.cpp - native GX2 calls and the single Vulkan render thread.
//
// Once the window exists one native render thread runs every call that
// touches the renderer. Vulkan objects are not bound to a thread, but keeping
// a single owner preserves the renderer's lock-free state model and the
// guest-visible call ordering. Before the thread starts, and in tests, a
// call runs on its caller under the renderer lock.
//
// The render thread drains one FIFO. Command calls (draws, state, display
// list calls) are posted to it and the guest continues, like the console's
// CPU appending packets for the GPU; every other renderer call is queued
// behind them and its caller waits for it (and therefore for everything
// posted before it), which keeps results and guest-memory publication in
// order.
#include "gx2_native_context.h"
#include "gx2_perf.h"
#include "gx2_render.h"
#include "gx2_write_watch.h"
#include "../os_common.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <immintrin.h>
#include <memory>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <utility>
#include <vector>

void gx2_render_vk_flush();
bool gx2_render_vk_finish();

namespace {
std::recursive_mutex s_renderer;
void* s_window = nullptr;
// Serializes waiting callers so a single error slot serves them.
std::mutex s_submit;

struct Item {
    Gx2AsyncWork* work = nullptr;          // posted: owned, no waiter
    Gx2Task run = nullptr; void* context = nullptr;  // waited task
    std::atomic<uint32_t>* core = nullptr;
    uint32_t ticket = 0;
};
std::mutex s_queue_mutex;
std::vector<Item> s_queue;
size_t s_queue_size = 0;                     // guarded by s_queue_mutex
uint32_t s_next_ticket = 0;                  // guarded by s_queue_mutex
std::atomic<uint32_t> s_pushed{0}, s_done{0};
std::atomic<bool> s_sleeping{false};
std::exception_ptr s_task_error, s_async_error;
std::atomic<bool> s_has_async_error{false};
std::mutex s_async_error_mutex;
std::atomic<bool> s_running{false}, s_started{false}, s_exit{false};
std::atomic<uint64_t> s_flushes{0}, s_finishes{0};
thread_local bool t_render_thread = false;
// Bound how far the guest can run ahead of the renderer (items, not frames;
// frame pacing is bounded separately at present).
constexpr size_t kMaxQueued = 1u << 16;
#if defined(RBREW_GX2_QUEUE_TESTS)
std::atomic<void(*)()> s_idle_hook{nullptr};
#endif

bool async_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("RBREW_GX2_ASYNC");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

bool reached(uint32_t done, uint32_t ticket) { return int32_t(done - ticket) >= 0; }

template<class Ready>
void await(const std::atomic<uint32_t>& value, Ready ready, std::chrono::microseconds spin) {
    uint32_t now = value.load(std::memory_order_acquire);
    if (ready(now)) return;
    const auto until = std::chrono::steady_clock::now() + spin;
    for (unsigned n = 1;; ++n) {
        now = value.load(std::memory_order_acquire);
        if (ready(now)) return;
        if (n % 64 == 0 && std::chrono::steady_clock::now() >= until) break;
        _mm_pause();
    }
    for (;;) {
        value.wait(now, std::memory_order_acquire);
        now = value.load(std::memory_order_acquire);
        if (ready(now)) return;
    }
}

void wake_render_thread() {
    s_pushed.fetch_add(1, std::memory_order_seq_cst);
    if (s_sleeping.load(std::memory_order_seq_cst)) s_pushed.notify_one();
}

void render_thread() {
    t_render_thread = true;
#if defined(_WIN32)
    // The game thread waits on this queue mid-frame (queries, context
    // switches): do not let guest workers or audio preempt it.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    s_running = true;
    s_started = true;
    s_started.notify_all();
    std::vector<Item> batch;
    for (;;) {
        uint32_t seen;
        {
            std::lock_guard lock(s_queue_mutex);
            batch.swap(s_queue);
            s_queue_size = 0;
            // Observe publication while checking the queue. A producer that
            // posts after this empty check must change this counter; taking
            // the snapshot after unlocking could sleep over queued work.
            seen = s_pushed.load(std::memory_order_seq_cst);
        }
        if (batch.empty()) {
            if (s_exit.load(std::memory_order_relaxed)) break;
#if defined(RBREW_GX2_QUEUE_TESTS)
            if (auto hook=s_idle_hook.load(std::memory_order_acquire)) hook();
#endif
            // Spin briefly (GX2 calls come in bursts), then sleep on the
            // push counter; a poster that sees s_sleeping wakes us.
            bool queued = false;
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(4000);
            for (unsigned n = 1; !queued; ++n) {
                if (s_pushed.load(std::memory_order_acquire) != seen) queued = true;
                else if (n % 64 == 0 && std::chrono::steady_clock::now() >= until) break;
                else _mm_pause();
            }
            if (!queued) {
                s_sleeping.store(true, std::memory_order_seq_cst);
                if (s_pushed.load(std::memory_order_seq_cst) == seen) s_pushed.wait(seen, std::memory_order_acquire);
                s_sleeping.store(false, std::memory_order_relaxed);
            }
            continue;
        }
        Gx2PerfScope busy(kPerfRenderBusy);
        gx2_write_watch_begin_batch();  // all of `batch` was queued before this point
        for (Item& item : batch) {
            // The caller's core selects its display list, as if it ran there.
            rbrew_thread_core_slot = item.core;
            if (item.work) {
                try { item.work->run(); }
                catch (...) {
                    std::lock_guard error_lock(s_async_error_mutex);
                    if (!s_async_error) s_async_error = std::current_exception();
                    s_has_async_error.store(true, std::memory_order_release);
                }
                delete item.work;
            } else {
                try { item.run(item.context); } catch (...) { s_task_error = std::current_exception(); }
                s_done.store(item.ticket, std::memory_order_release);
                s_done.notify_all();
            }
            rbrew_thread_core_slot = nullptr;
        }
        busy.stop();
        batch.clear();
    }
    s_running.store(false,std::memory_order_release);
    s_running.notify_all();
}

void submit(Gx2Task run, void* context) {
    const uint64_t start = gx2_perf_enabled() ? gx2_perf_now() : 0;
    std::lock_guard lock(s_submit);
    uint32_t ticket;
    {
        std::lock_guard queue_lock(s_queue_mutex);
        ticket = ++s_next_ticket;
        s_queue.push_back({nullptr, run, context, rbrew_thread_core_slot, ticket});
        ++s_queue_size;
    }
    wake_render_thread();
    await(s_done, [ticket](uint32_t done) { return reached(done, ticket); }, std::chrono::microseconds(4000));
    if (start) {
        const uint64_t ns = gx2_perf_now() - start;
        t_gx2_perf_wait_ns += ns;
        g_gx2_perf_ns[kPerfSubmitWait] += ns;
        ++g_gx2_perf_calls[kPerfSubmitWait];
    }
    if (auto error = std::exchange(s_task_error, nullptr)) std::rethrow_exception(error);
    if (s_has_async_error.load(std::memory_order_acquire)) {
        std::exception_ptr error;
        {
            std::lock_guard error_lock(s_async_error_mutex);
            error = std::exchange(s_async_error, nullptr);
            s_has_async_error = false;
        }
        if (error) std::rethrow_exception(error);
    }
}
}  // namespace

#if defined(RBREW_GX2_QUEUE_TESTS)
void gx2_native_idle_hook_for_test(void(*hook)()) { s_idle_hook.store(hook,std::memory_order_release); }
#endif

// The Vulkan build has no API context: `context` is ignored.
void gx2_native_set_context(void* window, void*) {
    std::lock_guard lock(s_renderer);
    s_window = window;
}
void* gx2_native_window() { return s_window; }

void gx2_native_start_render_thread() {
    if (s_running) return;
    s_exit = false; s_started = false;
    std::thread(render_thread).detach();
    s_started.wait(false);
}

void gx2_native_stop_render_thread() {
    if (!s_running) return;
    gx2_native_run(true, [] { s_exit = true; });
    // Readiness for a restart means the old consumer has actually exited,
    // not merely that its stop task returned to the waiting caller.
    while(s_running.load(std::memory_order_acquire))s_running.wait(true,std::memory_order_acquire);
}

void gx2_native_execute(bool gpu, Gx2Task task, void* context) {
    if (t_render_thread) { task(context); return; }
    if (s_running.load(std::memory_order_acquire)) { submit(task, context); return; }
    GX2NativeScope scope(gpu);
    task(context);
}

bool gx2_native_async_active() {
    return !t_render_thread && s_running.load(std::memory_order_acquire) && async_enabled();
}

void gx2_native_post(Gx2AsyncWork* work) {
    if (!gx2_native_async_active()) {
        std::unique_ptr<Gx2AsyncWork> owned(work);
        gx2_native_run(true, [&] { owned->run(); });
        return;
    }
    size_t queued;
    {
        std::lock_guard lock(s_queue_mutex);
        s_queue.push_back({work, nullptr, nullptr, rbrew_thread_core_slot, 0});
        queued = ++s_queue_size;
    }
    wake_render_thread();
    // A runaway producer waits for the renderer to catch up.
    if (queued >= kMaxQueued) gx2_native_run(true, [] {});
}

// GX2Flush: submit recorded work  and poll query completion.
void gx2_native_gpu_flush() {
    if (gx2_native_async_active()) {
        struct Flush final : Gx2AsyncWork { void run() override { gx2_render_vk_flush(); } };
        gx2_native_post(new Flush);
    } else {
        gx2_native_run(true, [] { gx2_render_vk_flush(); });
    }
    s_flushes.fetch_add(1, std::memory_order_relaxed);
}

// GX2DrawDone: wait for all submitted GPU work .
bool gx2_native_gpu_finish() {
    bool completed = false;
    gx2_native_run(true, [&] {
        completed = gx2_render_vk_finish();
        if (completed) s_finishes.fetch_add(1, std::memory_order_relaxed);
    });
    return completed;
}

uint64_t gx2_native_gpu_flush_count() { return s_flushes.load(std::memory_order_relaxed); }
uint64_t gx2_native_gpu_finish_count() { return s_finishes.load(std::memory_order_relaxed); }

void gx2_native_enter(bool) { s_renderer.lock(); }
void gx2_native_leave() { s_renderer.unlock(); }
