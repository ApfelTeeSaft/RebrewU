#pragma once
#include "../../runtime/rebrewu_runtime.h"
#include <cstdint>

// Native console settings, selected independently of the title's region.
struct RbrewUcSystemProfile {
    uint32_t language;
    uint32_t country_registration;
    bool operator==(const RbrewUcSystemProfile&) const = default;
};
void rbrew_uc_configure_profile(CPUState*,const RbrewUcSystemProfile&);
void rbrew_uc_forget_arena(const CPUState*);
void rbrew_uc_open(CPUState*);
void rbrew_uc_close(CPUState*);
void rbrew_uc_read_sys_config(CPUState*);
