#pragma once
#include <chrono>
#include <thread>
#include <cstdint>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace rbrew_host_timing {
struct Timer {
    // Windows 10 1803+: a private, auto-reset, high-resolution timer.
    // No global timeBeginPeriod setting is needed. The caller yields during
    // the final millisecond so a rounded kernel wait cannot miss the deadline.
    HANDLE handle=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_MODIFY_STATE|SYNCHRONIZE);
    ~Timer() { if(handle)CloseHandle(handle); }
    Timer()=default;
    Timer(const Timer&)=delete;
};
}
#endif

inline void rbrew_host_sleep_until(std::chrono::steady_clock::time_point deadline) {
#if defined(_WIN32)
    thread_local rbrew_host_timing::Timer timer;
    if(timer.handle) {
        for(auto now=std::chrono::steady_clock::now();now<deadline;now=std::chrono::steady_clock::now()) {
            const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-now).count();
            constexpr int64_t tail_ns=1000000;
            if(ns<=tail_ns){std::this_thread::yield();continue;}
            LARGE_INTEGER due{};due.QuadPart=-((ns-tail_ns+99)/100); // relative deadline in 100 ns units
            if(!SetWaitableTimer(timer.handle,&due,0,nullptr,nullptr,FALSE) ||
               WaitForSingleObject(timer.handle,INFINITE)!=WAIT_OBJECT_0)break;
        }
        if(std::chrono::steady_clock::now()>=deadline)return;
    }
#endif
    std::this_thread::sleep_until(deadline);
}
inline void rbrew_host_sleep_for(uint64_t microseconds) {
    rbrew_host_sleep_until(std::chrono::steady_clock::now()+std::chrono::microseconds(microseconds));
}
