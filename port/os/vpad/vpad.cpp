// VPAD (Virtual Pad — Wii U GamePad) stubs
// VPADRead maps SDL2 gamepad/keyboard/mouse input to VPADStatus.
//
// Keyboard + mouse layout (Splatoon / twin-stick-shooter style):
//   W/A/S/D          → left stick (move)
//   Mouse move (TV)  → right stick (aim / camera — relative mode)
//   LMB              → ZR  (fire / shoot)
//   RMB              → ZL  (squid form / aim / swim in ink)
//   Space            → B   (jump)
//   Q                → X   (sub weapon)
//   E                → Y   (special weapon)
//   R                → R   (reset camera / unused)
//   F                → +   (map screen / pause)
//   G                → −   (options)
//   Escape           → Home
//
// Controller (Xbox-layout connected via SDL2):
//   Left stick       → move           Right stick → camera
//   A→B, B→A, X→Y, Y→X (Nintendo swap)
//   LT → ZL          RT → ZR          LB → L      RB → R
//
// In Gamepad View (Tab), the mouse is freed for touch-screen simulation.

#include "../os_common.h"
#include "../../runtime/screen_mode.h"
#include <string.h>
#include <stdio.h>

// SDL2 types are needed wherever HAVE_SDL2 code uses SDL_GameController, etc.
#if defined(HAVE_SDL2)
#include <SDL2/SDL.h>
#endif

// VPADStatus layout (big-endian, 0x68 bytes):
//   +0x00  uint32  buttons held
//   +0x04  uint32  buttons triggered (newly pressed)
//   +0x08  uint32  buttons released
//   +0x0C  VPADVec2D leftStick   (2x float32, x then y)
//   +0x14  VPADVec2D rightStick  (2x float32)
//   +0x1C  acclX, acclY, acclZ  (3x float32)
//   +0x28  tpNormal    VPADTouchData (x u16, y u16, touched u16, validity u16)
//   +0x30  tpFiltered1 VPADTouchData
//   +0x38  tpFiltered2 VPADTouchData
//   …

#define VPAD_BUTTON_A      0x00008000u
#define VPAD_BUTTON_B      0x00004000u
#define VPAD_BUTTON_X      0x00002000u
#define VPAD_BUTTON_Y      0x00001000u
#define VPAD_BUTTON_LEFT   0x00000800u
#define VPAD_BUTTON_RIGHT  0x00000400u
#define VPAD_BUTTON_UP     0x00000200u
#define VPAD_BUTTON_DOWN   0x00000100u
#define VPAD_BUTTON_ZL     0x00000080u
#define VPAD_BUTTON_ZR     0x00000040u
#define VPAD_BUTTON_L      0x00000020u
#define VPAD_BUTTON_R      0x00000010u
#define VPAD_BUTTON_PLUS   0x00000008u
#define VPAD_BUTTON_MINUS  0x00000004u
#define VPAD_BUTTON_HOME   0x00000002u

// Mouse sensitivity for right-stick camera: stick_delta = pixel_delta * sensitivity.
// ~200 px swipe across the screen ≈ full right-stick deflection at this value.
static constexpr float KBM_MOUSE_SENSITIVITY = 0.005f;

// Write a VPADTouchData entry (8 bytes, big-endian) at 'base' in guest memory
static void write_touch_data(uint8_t* mem, uint32_t base,
                              uint16_t x, uint16_t y,
                              uint16_t touched, uint16_t validity) {
    // Each field is a big-endian uint16
    mem[base+0] = (uint8_t)(x       >> 8); mem[base+1] = (uint8_t)(x       & 0xFF);
    mem[base+2] = (uint8_t)(y       >> 8); mem[base+3] = (uint8_t)(y       & 0xFF);
    mem[base+4] = (uint8_t)(touched >> 8); mem[base+5] = (uint8_t)(touched & 0xFF);
    mem[base+6] = (uint8_t)(validity>> 8); mem[base+7] = (uint8_t)(validity& 0xFF);
}

// ---------------------------------------------------------------------------
// VPADRead(chan, buffers, count, error) → samplesRead
// ---------------------------------------------------------------------------
static void VPADRead(CPUState* cpu) {
    // ARG0 = channel (0=GamePad), ARG1 = VPADStatus* buf, ARG2 = count, ARG3 = error*
    uint32_t buf   = ARG1;
    uint32_t count = ARG2;
    uint32_t errp  = ARG3;

    if (errp) rbrew_write32(MEM, errp, 0u); // VPAD_READ_SUCCESS

    if (!buf || count == 0) {
        RET = 0;
        return;
    }

    // Clear the first VPADStatus entry (0x68 bytes)
    for (uint32_t i = 0; i < 0x68; i += 4)
        rbrew_write32(MEM, buf + i, 0u);

    uint32_t buttons = 0;
    float lx = 0.0f, ly = 0.0f; // left stick (movement)
    float rx = 0.0f, ry = 0.0f; // right stick (camera)

#if defined(HAVE_SDL2)
    // -----------------------------------------------------------------------
    // Controller (gamepad) input — open first available SDL2 game controller
    // -----------------------------------------------------------------------
    static SDL_GameController* s_gc = nullptr;
    // Refresh controller slot if disconnected or none yet found
    if (!s_gc || !SDL_GameControllerGetAttached(s_gc)) {
        if (s_gc) { SDL_GameControllerClose(s_gc); s_gc = nullptr; }
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) {
                s_gc = SDL_GameControllerOpen(i);
                if (s_gc) break;
            }
        }
    }
    SDL_GameController* gc = s_gc;

    if (gc) {
        auto btn = [&](SDL_GameControllerButton b) {
            return SDL_GameControllerGetButton(gc, b) != 0;
        };
        if (btn(SDL_CONTROLLER_BUTTON_A))            buttons |= VPAD_BUTTON_B; // Nintendo layout swap
        if (btn(SDL_CONTROLLER_BUTTON_B))            buttons |= VPAD_BUTTON_A;
        if (btn(SDL_CONTROLLER_BUTTON_X))            buttons |= VPAD_BUTTON_Y;
        if (btn(SDL_CONTROLLER_BUTTON_Y))            buttons |= VPAD_BUTTON_X;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT))    buttons |= VPAD_BUTTON_LEFT;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT))   buttons |= VPAD_BUTTON_RIGHT;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP))      buttons |= VPAD_BUTTON_UP;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN))    buttons |= VPAD_BUTTON_DOWN;
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  buttons |= VPAD_BUTTON_L;
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) buttons |= VPAD_BUTTON_R;
        if (btn(SDL_CONTROLLER_BUTTON_START))        buttons |= VPAD_BUTTON_PLUS;
        if (btn(SDL_CONTROLLER_BUTTON_BACK))         buttons |= VPAD_BUTTON_MINUS;
        if (btn(SDL_CONTROLLER_BUTTON_GUIDE))        buttons |= VPAD_BUTTON_HOME;

        // Left trigger → ZL, right trigger → ZR
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 16384) buttons |= VPAD_BUTTON_ZL;
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384) buttons |= VPAD_BUTTON_ZR;

        // Analog sticks: normalize from [-32768,32767] to [-1.0, 1.0]
        lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX)  / 32767.0f;
        ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)  / 32767.0f;
        rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
        ry = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
    }

    // -----------------------------------------------------------------------
    // Keyboard + mouse input (Splatoon layout, active in TV view)
    // KB+M additively overlays controller: pressing a key overrides that axis.
    // -----------------------------------------------------------------------
    if (g_screen_mode == ScreenMode::TV) {
        const uint8_t* keys = SDL_GetKeyboardState(nullptr);

        // WASD → left stick movement
        float kb_lx = 0.0f, kb_ly = 0.0f;
        if (keys[SDL_SCANCODE_D]) kb_lx += 1.0f;
        if (keys[SDL_SCANCODE_A]) kb_lx -= 1.0f;
        if (keys[SDL_SCANCODE_S]) kb_ly += 1.0f; // +Y = down on Wii U stick
        if (keys[SDL_SCANCODE_W]) kb_ly -= 1.0f;
        if (kb_lx != 0.0f && kb_ly != 0.0f) { kb_lx *= 0.7071f; kb_ly *= 0.7071f; } // diag norm
        if (kb_lx != 0.0f || kb_ly != 0.0f) { lx = kb_lx; ly = kb_ly; } // override controller

        // Mouse relative motion → right stick (camera / aim)
        int mdx = 0, mdy = 0;
        SDL_GetRelativeMouseState(&mdx, &mdy);
        float mrx = (float)mdx * KBM_MOUSE_SENSITIVITY;
        float mry = (float)mdy * KBM_MOUSE_SENSITIVITY;
        if (mrx < -1.0f) mrx = -1.0f; if (mrx > 1.0f) mrx = 1.0f;
        if (mry < -1.0f) mry = -1.0f; if (mry > 1.0f) mry = 1.0f;
        // Blend: mouse delta is added to controller stick, clamped
        rx += mrx; if (rx < -1.0f) rx = -1.0f; if (rx > 1.0f) rx = 1.0f;
        ry += mry; if (ry < -1.0f) ry = -1.0f; if (ry > 1.0f) ry = 1.0f;

        // Mouse buttons — ZR = fire, ZL = squid / aim
        int mx_dummy, my_dummy;
        uint32_t mstate = (uint32_t)SDL_GetMouseState(&mx_dummy, &my_dummy);
        if (mstate & SDL_BUTTON(SDL_BUTTON_LEFT))  buttons |= VPAD_BUTTON_ZR;
        if (mstate & SDL_BUTTON(SDL_BUTTON_RIGHT)) buttons |= VPAD_BUTTON_ZL;

        // Keyboard buttons
        if (keys[SDL_SCANCODE_SPACE])  buttons |= VPAD_BUTTON_B;   // jump
        if (keys[SDL_SCANCODE_Q])      buttons |= VPAD_BUTTON_X;   // sub weapon
        if (keys[SDL_SCANCODE_E])      buttons |= VPAD_BUTTON_Y;   // special weapon
        if (keys[SDL_SCANCODE_R])      buttons |= VPAD_BUTTON_R;   // R (centre cam / unused)
        if (keys[SDL_SCANCODE_F])      buttons |= VPAD_BUTTON_PLUS;  // map / pause
        if (keys[SDL_SCANCODE_G])      buttons |= VPAD_BUTTON_MINUS; // options
        if (keys[SDL_SCANCODE_ESCAPE]) buttons |= VPAD_BUTTON_HOME;  // home menu
    }

    // Write analog sticks (set by controller and/or KB+M above)
    {
        uint32_t lx_b, ly_b, rx_b, ry_b;
        memcpy(&lx_b, &lx, 4); memcpy(&ly_b, &ly, 4);
        memcpy(&rx_b, &rx, 4); memcpy(&ry_b, &ry, 4);
        rbrew_write32(MEM, buf + 0x0C, lx_b);
        rbrew_write32(MEM, buf + 0x10, ly_b);
        rbrew_write32(MEM, buf + 0x14, rx_b);
        rbrew_write32(MEM, buf + 0x18, ry_b);
    }

    // --- Simulated touch from mouse in Gamepad View ---
    {
        uint16_t tx       = g_touch_state.x;
        uint16_t ty       = g_touch_state.y;
        uint16_t touched  = g_touch_state.touched ? 1u : 0u;
        uint16_t validity = touched ? 1u : 0u; // VPAD_TP_VALID = 1

        // Write tpNormal, tpFiltered1, tpFiltered2 (same value — no real filter)
        write_touch_data(MEM, buf + 0x28, tx, ty, touched, validity);
        write_touch_data(MEM, buf + 0x30, tx, ty, touched, validity);
        write_touch_data(MEM, buf + 0x38, tx, ty, touched, validity);
    }
#endif

    rbrew_write32(MEM, buf + 0x00, buttons);
    rbrew_write32(MEM, buf + 0x04, buttons); // trigger same as held for simplicity
    rbrew_write32(MEM, buf + 0x08, 0u);      // released

    RET = 1; // 1 sample read
}

// VPADGetTPCalibratedPoint — touchpad, return (0,0)
static void VPADGetTPCalibratedPoint(CPUState* cpu) {
    uint32_t outp = ARG2;
    if (outp) {
        rbrew_write32(MEM, outp + 0, 0); // x
        rbrew_write32(MEM, outp + 4, 0); // y
    }
    RET = 0;
}

static void VPADGetTPCalibratedPointEx(CPUState* cpu) {
    VPADGetTPCalibratedPoint(cpu);
}

// VPADSetAccParam / VPADSetBtnRepeat — no-op
static void VPADSetAccParam(CPUState* cpu)  { (void)cpu; }
static void VPADSetBtnRepeat(CPUState* cpu) { (void)cpu; }

// VPADDisableGyroZeroDrift — no-op
static void VPADDisableGyroZeroDrift(CPUState* cpu) { (void)cpu; }

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "vpad_addrs.h"

void vpad_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(VPADRead);
    REG(VPADGetTPCalibratedPoint);
    // REG(VPADGetTPCalibratedPointEx); // not in this game's PLT
    // REG(VPADSetAccParam); // not in this game's PLT
    // REG(VPADSetBtnRepeat); // not in this game's PLT
    // REG(VPADDisableGyroZeroDrift); // not in this game's PLT
#undef REG
}
