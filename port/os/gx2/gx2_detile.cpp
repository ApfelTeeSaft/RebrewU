#include "Standalone.h"  // Cemu scalar types for LatteAddrLib
#include "gx2_detile.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace {
// Multi-core detiling: large tiled surfaces are split into row bands that the
// caller and helper threads convert concurrently. Each output row is written
// by exactly one band from the read-only source, and the LatteAddrLib address
// functions are pure, so the image is byte-identical to the serial loop.
// RBREW_DETILE_WORKERS=N helpers (default 3, capped at cores-1; 0 = serial).
class BandPool {
public:
    static BandPool& get() { static auto* pool = new BandPool; return *pool; }   // lives until exit
    unsigned workers() const { return unsigned(threads_.size()); }
    void set_workers(unsigned n) {   // tests only, with no job running
        std::lock_guard run(run_mutex_);
        stop();
        start(n);
    }
    // Runs fn(0..bands-1) on the caller and the helpers; returns when all are done.
    void run(unsigned bands, const std::function<void(unsigned)>& fn) {
        std::lock_guard run(run_mutex_);
        {
            std::lock_guard lock(mutex_);
            fn_ = &fn; bands_ = bands; next_ = 0; remaining_ = bands; ++generation_;
        }
        ready_.notify_all();
        work();
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return remaining_ == 0; });
        fn_ = nullptr;
    }
private:
    BandPool() {
        unsigned n = 3;
        if (const char* v = std::getenv("RBREW_DETILE_WORKERS")) n = unsigned(std::strtoul(v, nullptr, 10));
        const unsigned cores = std::thread::hardware_concurrency();
        if (cores > 1) n = std::min(n, cores - 1); else n = 0;
        start(n);
    }
    void start(unsigned n) {
        exit_ = false;
        for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { loop(); });
    }
    void stop() {
        { std::lock_guard lock(mutex_); exit_ = true; ++generation_; }
        ready_.notify_all();
        for (auto& t : threads_) t.join();
        threads_.clear();
    }
    void loop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return exit_ || generation_ != seen; });
                if (exit_) return;
                seen = generation_;
            }
            work();
        }
    }
    void work() {
        for (;;) {
            const std::function<void(unsigned)>* fn;
            unsigned band;
            {
                std::lock_guard lock(mutex_);
                if (!fn_ || next_ >= bands_) return;
                fn = fn_; band = next_++;
            }
            (*fn)(band);
            std::lock_guard lock(mutex_);
            if (--remaining_ == 0) done_.notify_all();
        }
    }
    std::mutex run_mutex_, mutex_;
    std::condition_variable ready_, done_;
    std::vector<std::thread> threads_;
    const std::function<void(unsigned)>* fn_ = nullptr;
    unsigned bands_ = 0, next_ = 0, remaining_ = 0;
    uint64_t generation_ = 0;
    bool exit_ = false;
};
constexpr uint64_t kParallelElements = 128 * 128;   // below this a band hand-off costs more than it saves

bool valid_desc(const Gx2DetileDesc& d) {
    return d.width && d.height && d.pitch >= d.width && d.tile_mode <= 16 &&
        (d.bits_per_element == 8 || d.bits_per_element == 16 || d.bits_per_element == 32 ||
         d.bits_per_element == 64 || d.bits_per_element == 128) &&
        (!d.surface_height || d.surface_height >= d.height);
}
uint32_t element_address(const Gx2DetileDesc& d, uint32_t x, uint32_t y) {
    const auto mode = static_cast<Latte::E_HWTILEMODE>(d.tile_mode);
    const uint32_t height = d.surface_height ? d.surface_height : d.height;
    if (d.tile_mode <= 1 || d.tile_mode == 16)
        return LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(
            x,y,d.slice,0,d.bits_per_element,d.pitch,height,1);
    if (d.tile_mode <= 3)
        return LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(
            x,y,d.slice,d.bits_per_element,d.pitch,height,mode,d.is_depth);
    return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(
        x,y,d.slice,0,d.bits_per_element,d.pitch,height,1,mode,d.is_depth,
        (d.swizzle >> 8) & 1,(d.swizzle >> 9) & 3);
}
}

bool gx2_detile_mip0(const Gx2DetileDesc& d, const uint8_t* tiled, size_t tiled_size, uint8_t* out) {
    const uint32_t bytes = d.bits_per_element / 8;
    if (!tiled || !out || !valid_desc(d))
        return false;
    if(d.tile_mode<=1 || d.tile_mode==16) {
        const uint64_t pitch=uint64_t(d.pitch)*bytes;
        const uint64_t row=uint64_t(d.width)*bytes;
        const uint64_t first_row=uint64_t(d.slice)*(d.surface_height?d.surface_height:d.height);
        if(row>tiled_size || first_row+d.height-1>(tiled_size-row)/pitch)return false;
        const uint64_t first=first_row*pitch;
        for(uint32_t y=0;y<d.height;++y)
            std::memcpy(out+size_t(y)*row,tiled+first+uint64_t(y)*pitch,size_t(row));
        return true;
    }
    // On failure the caller discards `out` (header contract), so bands that
    // already converted rows before another band failed change nothing.
    auto rows = [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = 0; x < d.width; ++x) {
                const uint32_t address = element_address(d,x,y);
                if (uint64_t(address) + bytes > tiled_size) return false;
                std::memcpy(out + (size_t(y) * d.width + x) * bytes, tiled + address, bytes);
            }
        }
        return true;
    };
    BandPool& pool = BandPool::get();
    if (!pool.workers() || uint64_t(d.width) * d.height < kParallelElements || d.height < 2)
        return rows(0, d.height);
    const unsigned bands = std::min<unsigned>(d.height, (pool.workers() + 1) * 4);
    std::atomic<bool> ok{true};
    pool.run(bands, [&](unsigned band) {
        const uint32_t y0 = uint32_t(uint64_t(d.height) * band / bands), y1 = uint32_t(uint64_t(d.height) * (band + 1) / bands);
        if (ok.load(std::memory_order_relaxed) && !rows(y0, y1)) ok.store(false, std::memory_order_relaxed);
    });
    return ok.load();
}

// Tests: change the helper count (0 = the serial loop).
void gx2_detile_set_workers_for_test(unsigned workers) { BandPool::get().set_workers(workers); }
unsigned gx2_detile_workers() { return BandPool::get().workers(); }

bool gx2_retile_mip0(const Gx2DetileDesc& d, const uint8_t* linear,
                    uint8_t* tiled, size_t tiled_size) {
    if (!linear || !tiled || !valid_desc(d)) return false;
    const uint32_t bytes = d.bits_per_element / 8;
    if(d.tile_mode<=1 || d.tile_mode==16) {
        const uint64_t pitch=uint64_t(d.pitch)*bytes;
        const uint64_t row=uint64_t(d.width)*bytes;
        const uint64_t first_row=uint64_t(d.slice)*(d.surface_height?d.surface_height:d.height);
        if(row>tiled_size || first_row+d.height-1>(tiled_size-row)/pitch)return false;
        const uint64_t first=first_row*pitch;
        for(uint32_t y=0;y<d.height;++y)
            std::memcpy(tiled+first+uint64_t(y)*pitch,linear+size_t(y)*row,size_t(row));
        return true;
    }
    for (uint32_t y=0;y<d.height;++y)for(uint32_t x=0;x<d.width;++x)
        if (uint64_t(element_address(d,x,y))+bytes > tiled_size) return false;
    for (uint32_t y=0;y<d.height;++y)for(uint32_t x=0;x<d.width;++x)
        std::memcpy(tiled+element_address(d,x,y),linear+(size_t(y)*d.width+x)*bytes,bytes);
    return true;
}
