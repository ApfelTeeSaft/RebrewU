#pragma once
#include <cstdint>

// One shared 60000/1001-Hz scan-out clock. Calls/cores do not consume
// individual vblanks: every waiter at the same time targets the same event.
constexpr uint64_t gx2_vblank_tick(uint64_t index,uint32_t timer_frequency) {
    const uint64_t cycle=uint64_t(timer_frequency)*1001;
    return (index/60000)*cycle+(index%60000)*cycle/60000;
}
constexpr uint64_t gx2_next_vblank_tick(uint64_t now,uint32_t timer_frequency) {
    const uint64_t cycle=uint64_t(timer_frequency)*1001;
    uint64_t index=(now/cycle)*60000+(now%cycle)*60000/cycle+1;
    uint64_t target=gx2_vblank_tick(index,timer_frequency);
    if(target<=now)target=gx2_vblank_tick(index+1,timer_frequency);
    return target;
}
