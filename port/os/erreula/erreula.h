#pragma once
#include "rebrewu_runtime.h"

// Native implementation of the proven inactive ErrEula lifecycle. Active error
// dialogs/HomeNix/Jump require a UI provider and explicitly fail before mutation.
// Guest thunk addresses belong to the native HLE namespace, not original RPX.
void erreula_register(CPUState* cpu, uint32_t thunk_begin = 0x0CF00000u);
struct RbrewErrEulaSnapshot {
    uint32_t version{}, work_memory{}, region{}, language{}, fs_client{}, controller{};
    bool initialized{}, home_nix{}, play_appear_sound{};
    uint32_t viewer_state{};
    int32_t result_code{-1}, selected_button{-1};
};
RbrewErrEulaSnapshot erreula_snapshot(const CPUState* cpu);
void erreula_forget_arena(const CPUState* cpu);
