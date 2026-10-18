#include "gx2_write_watch.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
#include "../../runtime/rebrewu_memory_observer.h"
#include "../../runtime/rbrew_diagnostics.h"

namespace {
constexpr uint64_t kPage = 4096;
std::mutex s_lock;
const uint8_t* s_mem = nullptr;
std::vector<uint32_t> s_page_generation;  // guest page -> generation of its newest collected write
std::vector<uint64_t> s_page_collected;   // bit per guest page: s_page_generation != 0
uint32_t s_generation = 0;
std::vector<void*> s_written;
uint64_t s_queries=0,s_checks=0,s_reused=0;
std::vector<Gx2WriteRange*> s_ranges;

// Batch epochs (render thread only). Every command of a render batch was
// queued before the batch started, so every write that preceded its issue
// also precedes any collection made during the batch: a page collected in
// the current epoch is not asked again until the next batch. Writes landing
// later in the batch stay dirty in the OS and appear in the next epoch.
// The renderer's own guest writes (readback, stream-out, query results)
// pass through the write observer and un-collect their pages.
// RBREW_WRITE_WATCH_REUSE=0 asks the OS on every query again.
std::vector<uint32_t> s_page_epoch;  // guest page -> epoch of its last collection
uint32_t s_epoch = 1;
thread_local bool t_epoch_reuse = false;
thread_local const uint8_t* t_observed_mem = nullptr;

// Diagnostic copies only: RBREW_WRITE_WATCH_DISABLE=1 reports every range
// untracked, so all geometry/uniform caches re-read guest memory each use.
bool tracking_disabled() {
    static const bool disabled = [] { const char* v = rbrew_diagnostic_env("RBREW_WRITE_WATCH_DISABLE"); return v && v[0] == '1'; }();
    return disabled;
}
bool reuse_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("RBREW_WRITE_WATCH_REUSE");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

void select_arena(const uint8_t* mem) {
    if (mem != s_mem) {  // a new arena starts with an empty history
        s_mem = mem;
        s_page_generation.assign(size_t(1) << 20, 0);  // 4 GiB guest space / 4 KiB pages
        s_page_collected.assign((size_t(1) << 20) / 64, 0);
        s_page_epoch.assign(size_t(1) << 20, 0);
        s_generation = 0;
    }
}
void forget_collected(uint32_t address, uint32_t size, void*) noexcept {
    if (!size) return;
    std::lock_guard lock(s_lock);
    if (s_page_epoch.empty()) return;
    const uint64_t last = std::min<uint64_t>((uint64_t(address) + size - 1) / kPage, s_page_epoch.size() - 1);
    for (uint64_t page = address / kPage; page <= last; ++page) s_page_epoch[page] = 0;
}
// Caller holds s_lock with the arena selected.
bool collected_this_epoch(const uint8_t* mem, uint64_t first, uint64_t last) {
    if (!t_epoch_reuse || mem != t_observed_mem) return false;
    for (uint64_t page = first; page <= last; ++page)
        if (s_page_epoch[page] != s_epoch) return false;
    ++s_reused;
    return true;
}
void observe_render_writes(const uint8_t* mem) {
    if (!t_epoch_reuse || t_observed_mem == mem) return;
    // Installed outside s_lock: the observer callback takes it.
    rbrew_set_write_observer({const_cast<uint8_t*>(mem), forget_collected, nullptr});
    t_observed_mem = mem;
}
bool collect_pages(const uint8_t* mem,uint64_t first,uint64_t last) {
    // Room for every page of the range: a reset must not drop unreported pages.
    s_written.resize(size_t(last - first + 1));
    ULONG_PTR count = s_written.size();
    DWORD granularity = 0;
    ++s_queries;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, const_cast<uint8_t*>(mem) + first * kPage,
                      SIZE_T((last - first + 1) * kPage), s_written.data(), &count, &granularity) != 0)
        return false;
    if (count) {
        ++s_generation;
        for (ULONG_PTR i = 0; i < count; ++i) {
            const size_t page = size_t((static_cast<const uint8_t*>(s_written[i]) - mem) / kPage);
            s_page_generation[page] = s_generation;
            s_page_collected[page / 64] |= 1ull << (page % 64);
        }
    }
    for (uint64_t page = first; page <= last; ++page) s_page_epoch[page] = s_epoch;
    return true;
}
uint64_t newest(uint64_t first,uint64_t last) {
    uint32_t generation = 0;
    for (uint64_t page = first; page <= last; ++page) generation = std::max(generation, s_page_generation[page]);
    return generation;
}
}

uint64_t gx2_write_generation(const uint8_t* mem,uint32_t addr,uint32_t size) {
    if(!mem || !size)return 0;
    if(uint64_t(addr)+size>(1ull<<32) || tracking_disabled())return kGx2WriteUntracked;
    const uint64_t first=addr/kPage,last=(uint64_t(addr)+size-1)/kPage;
    observe_render_writes(mem);
    std::lock_guard lock(s_lock);select_arena(mem);++s_checks;
    if(collected_this_epoch(mem,first,last))return newest(first,last);
    return collect_pages(mem,first,last)?newest(first,last):kGx2WriteUntracked;
}
void gx2_write_generations(const uint8_t* mem,Gx2WriteRange* ranges,uint32_t count) {
    if(!ranges || !count)return;
    if(mem)observe_render_writes(mem);
    std::lock_guard lock(s_lock);
    s_ranges.clear();
    for(uint32_t i=0;i<count;++i) {
        auto& range=ranges[i];range.generation=0;
        if(!mem || !range.size)continue;
        if(uint64_t(range.address)+range.size>(1ull<<32) || tracking_disabled()){range.generation=kGx2WriteUntracked;continue;}
        s_ranges.push_back(&range);++s_checks;
    }
    if(s_ranges.empty())return;
    select_arena(mem);
    std::sort(s_ranges.begin(),s_ranges.end(),[](const auto* a,const auto* b){return a->address<b->address;});
    for(size_t first_range=0;first_range<s_ranges.size();) {
        const auto* first=s_ranges[first_range];
        const uint64_t first_page=first->address/kPage;
        uint64_t last_page=(uint64_t(first->address)+first->size-1)/kPage;
        size_t end=first_range+1;
        // Merge only overlapping/adjacent pages: never reset unrequested gaps.
        while(end<s_ranges.size() && s_ranges[end]->address/kPage<=last_page+1) {
            last_page=std::max(last_page,(uint64_t(s_ranges[end]->address)+s_ranges[end]->size-1)/kPage);++end;
        }
        const bool tracked=collected_this_epoch(mem,first_page,last_page) || collect_pages(mem,first_page,last_page);
        for(size_t i=first_range;i<end;++i) {
            auto& range=*s_ranges[i];
            range.generation=tracked?newest(range.address/kPage,(uint64_t(range.address)+range.size-1)/kPage):kGx2WriteUntracked;
        }
        first_range=end;
    }
}
void gx2_write_watch_begin_batch() {
    if(!reuse_enabled())return;
    t_epoch_reuse=true;
    std::lock_guard lock(s_lock);
    if(++s_epoch==0)s_epoch=1;  // 0 means "never collected"
}
uint64_t gx2_write_query_count(){std::lock_guard lock(s_lock);return s_queries;}
Gx2WriteWatchStats gx2_write_watch_stats(){std::lock_guard lock(s_lock);return {s_checks,s_queries,s_reused};}
bool gx2_write_watch_pending(const uint8_t* mem,uint32_t addr,uint32_t size) {
    if(!mem || !size)return false;
    const uint64_t first=addr/kPage,last=(uint64_t(addr)+size-1)/kPage;
    std::vector<void*> pages(size_t(last-first+1));ULONG_PTR count=pages.size();DWORD granularity=0;
    if(GetWriteWatch(0,const_cast<uint8_t*>(mem)+first*kPage,SIZE_T((last-first+1)*kPage),pages.data(),&count,&granularity)!=0)return false;
    return count!=0;
}
bool gx2_write_watch_written_pages(const uint8_t* mem,uint64_t size,std::vector<uint64_t>& pages) {
    const uint64_t page_count=(size+kPage-1)/kPage;
    pages.assign(size_t((page_count+63)/64),0);
    if(!mem || !size)return false;
    // The OS query of a whole guest arena takes milliseconds (~4 ms for 4 GiB)
    // and runs outside s_lock: the render thread checks write generations at
    // every draw and must not wait for a display-list sweep. It is also split
    // into slices, because the kernel serializes write-watch queries of the
    // process and a single whole-arena call delayed those checks as well.
    // No page is lost between the two halves: a collection takes s_lock before
    // it resets a page and marks it collected before releasing the lock, so a
    // page reset before this query saw it is marked once the lock is acquired.
    constexpr uint64_t kSlicePages=uint64_t(1)<<16;   // 256 MiB
    auto written=std::make_unique_for_overwrite<void*[]>(static_cast<size_t>(std::min(page_count,kSlicePages)));
    for(uint64_t first=0;first<page_count;first+=kSlicePages) {
        const uint64_t slice=std::min(kSlicePages,page_count-first);
        ULONG_PTR count=static_cast<ULONG_PTR>(slice);
        DWORD granularity=0;
        if(GetWriteWatch(0,const_cast<uint8_t*>(mem)+first*kPage,SIZE_T(slice*kPage),written.get(),&count,&granularity)!=0)return false;
        for(ULONG_PTR i=0;i<count;++i) {
            const uint64_t page=uint64_t(static_cast<const uint8_t*>(written[i])-mem)/kPage;
            if(page<page_count)pages[page/64]|=1ull<<(page%64);
        }
    }
    std::lock_guard lock(s_lock);
    if(mem==s_mem) {
        const size_t words=std::min(pages.size(),s_page_collected.size());
        for(size_t w=0;w<words;++w)pages[w]|=s_page_collected[w];
        if(page_count%64 && words==pages.size())pages.back()&=(1ull<<(page_count%64))-1;
    }
    return true;
}
#else
bool gx2_write_watch_written_pages(const uint8_t*,uint64_t size,std::vector<uint64_t>& pages){pages.assign(size_t((size/4096+64)/64),0);return false;}
void gx2_write_watch_begin_batch() {}
uint64_t gx2_write_generation(const uint8_t*, uint32_t, uint32_t) { return kGx2WriteUntracked; }
bool gx2_write_watch_pending(const uint8_t*, uint32_t, uint32_t) { return false; }
uint64_t gx2_write_query_count(){return 0;}
Gx2WriteWatchStats gx2_write_watch_stats(){return {};}
void gx2_write_generations(const uint8_t* mem,Gx2WriteRange* ranges,uint32_t count) {
    if(!ranges)return;
    for(uint32_t i=0;i<count;++i)ranges[i].generation=gx2_write_generation(mem,ranges[i].address,ranges[i].size);
}
#endif
