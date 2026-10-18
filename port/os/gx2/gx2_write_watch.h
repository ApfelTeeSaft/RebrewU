// gx2_write_watch.h - page-precise detection of CPU writes to guest memory.
//
// The native guest arena is reserved with Windows write watch (main.cpp).
// The renderer caches decoded textures and vertex buffers; instead of hashing
// or re-uploading guest bytes after every broad GX2Invalidate, it asks which
// pages the CPU (guest code, file reads, native HLE) wrote since it last
// looked. Writes collected while answering one range are kept in a per-page
// generation table, so pages shared with another cached resource stay dirty
// for it as well.
#pragma once
#include <cstdint>
#include <vector>

// Answer for arenas without write watch (tests, other platforms): callers
// must compare contents instead.
constexpr uint64_t kGx2WriteUntracked = ~0ull;

// Generation of the newest CPU write to any page of [addr, addr + size),
// after collecting writes made since the previous query of those pages.
// Larger means written later. Query before reading the bytes: a write that
// lands while they are read appears in the next query.
uint64_t gx2_write_generation(const uint8_t* mem, uint32_t addr, uint32_t size);

struct Gx2WriteRange { uint32_t address=0,size=0; uint64_t generation=0; };
// Collect a group of ranges at the same consumption point. Results retain
// input order and the same per-page history as individual queries.
void gx2_write_generations(const uint8_t* mem,Gx2WriteRange* ranges,uint32_t count);
uint64_t gx2_write_query_count(); // diagnostic: actual platform write-watch calls
// Render thread, before running a batch it took from the queue: pages already
// collected during this batch are answered from history, without asking the
// OS again (every command in the batch was queued before it started). Only
// the calling thread reuses; other callers query every time.
void gx2_write_watch_begin_batch();
struct Gx2WriteWatchStats { uint64_t ranges=0,queries=0,reused=0; };
Gx2WriteWatchStats gx2_write_watch_stats();
// Pages of [mem, mem + size) written at least once since the arena was
// reserved: pages the OS still reports written plus pages this module already
// collected. One bit per 4 KiB page (bit i = page i). Does not reset
// anything. False when the range has no write watch (tests, other platforms).
bool gx2_write_watch_written_pages(const uint8_t* mem, uint64_t size, std::vector<uint64_t>& pages);
// Diagnostic: the OS still reports a write to [addr, addr + size) that no
// query has collected yet. Resets nothing.
bool gx2_write_watch_pending(const uint8_t* mem, uint32_t addr, uint32_t size);
