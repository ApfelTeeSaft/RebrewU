// padscore — Classic Controller / Wii Remote stubs
// KPADInit, KPADRead, WPADProbe, etc.

#include "../os_common.h"

static void KPADInit(CPUState* cpu)      { (void)cpu; }
static void KPADInitEx(CPUState* cpu)    { (void)cpu; }
static void KPADShutdown(CPUState* cpu)  { (void)cpu; }

// KPADRead(chan, buffers, count) → samplesRead
static void KPADRead(CPUState* cpu) {
    // Just clear the buffer and report 0 samples
    uint32_t buf   = ARG1;
    uint32_t count = ARG2;
    if (buf && count > 0) {
        // KPADStatus is ~0xF0 bytes; clear first entry
        for (uint32_t i = 0; i < 0xF0; i += 4)
            rbrew_write32(MEM, buf + i, 0u);
    }
    RET = 0;
}

// KPADReadEx — same
static void KPADReadEx(CPUState* cpu) { KPADRead(cpu); }

// WPADProbe(chan, type*) → not connected
static void WPADProbe(CPUState* cpu) {
    if (ARG1) rbrew_write32(MEM, ARG1, 0u);
    RET = -1; // WPAD_ERR_NOT_READY
}

static void WPADIsEnabled(CPUState* cpu)          { RET = 0; }
static void WPADGetBatteryLevel(CPUState* cpu)    { RET = 0; }
static void WPADControlMotor(CPUState* cpu)       { (void)cpu; }
static void WPADGetInfoAsync(CPUState* cpu)       { (void)cpu; }
static void WPADIsMplsAttached(CPUState* cpu)     { RET = 0; }

static void KPADGetUnifiedWpadStatus(CPUState* cpu) { RET = 0; }
static void KPADSetPosPlayMode(CPUState* cpu)       { (void)cpu; }
static void KPADEnableDPD(CPUState* cpu)            { (void)cpu; }
static void KPADDisableDPD(CPUState* cpu)           { (void)cpu; }

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "padscore_addrs.h"

void padscore_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    // REG(KPADInit); // not in this game's PLT
    REG(KPADInitEx);
    // REG(KPADShutdown); // not in this game's PLT
    // REG(KPADRead); // not in this game's PLT
    REG(KPADReadEx);
    REG(WPADProbe);
    // REG(WPADIsEnabled); // not in this game's PLT
    REG(WPADGetBatteryLevel);
    REG(WPADControlMotor);
    // REG(WPADGetInfoAsync); // not in this game's PLT
    REG(WPADIsMplsAttached);
    // REG(KPADGetUnifiedWpadStatus); // not in this game's PLT
    // REG(KPADSetPosPlayMode); // not in this game's PLT
    REG(KPADEnableDPD);
    REG(KPADDisableDPD);
#undef REG
}
