#include "../../runtime/rbrew_diagnostics.h"
// VPAD (Virtual Pad — Wii U GamePad)
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
// Controller (any SDL2 game controller, buttons by position — see
// gx2_context.cpp, which turns off SDL's Nintendo label layout):
//   south/east/west/north → B/A/Y/X, exactly where they sit on a GamePad
//   LB/RB → L/R, LT/RT (analog or digital ZL/ZR) → ZL/ZR
//   stick clicks → L3/R3, Start/Back/Guide → +/−/Home
//
// In Gamepad View (Tab), the mouse is freed for touch-screen simulation.

#include "../os_common.h"
#include "../../runtime/screen_mode.h"
#include "../../runtime/native_control_defaults.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// SDL2 types are needed wherever HAVE_SDL2 code uses SDL_GameController, etc.
#if defined(HAVE_SDL2)
#include <SDL2/SDL.h>
#endif

// VPADStatus (big-endian, 0xAC bytes; matches the sample stride the game's
// VPADRead caller uses, 16 samples at device+0x14):
//   +0x00 hold, +0x04 trigger, +0x08 release
//   +0x0C leftStick (x, y), +0x14 rightStick (x, y)   — y > 0 is up
//   +0x1C acc (x, y, z), +0x28 accMagnitude, +0x2C accAcceleration,
//   +0x30 accXY (vertical, 2 floats)
//   +0x38 gyro (x, y, z), +0x44 angle (x, y, z), +0x50 s8 error
//   +0x52 tpNormal, +0x5A tpFiltered1, +0x62 tpFiltered2 (VPADTouchData:
//         u16 x, y, touched, validity)
//   +0x6C direction (3 x vec3: x axis, y axis, z axis), +0x90 headphones
//   +0x94 magnetometer, +0xA0 slideVolume, +0xA1 battery, +0xA2 mic,
//   +0xA3 slideVolumeEx
static constexpr uint32_t kVpadStatusSize = 0xAC;
static constexpr uint32_t kVpadTpNormal = 0x52, kVpadTpFiltered1 = 0x5A, kVpadTpFiltered2 = 0x62;
static constexpr uint32_t kVpadAccXY = 0x30, kVpadDirection = 0x6C;
static constexpr uint32_t kVpadBattery = 0xA1, kVpadSlideVolume = 0xA0, kVpadSlideVolumeEx = 0xA3;

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
#define VPAD_BUTTON_STICK_R 0x00020000u
#define VPAD_BUTTON_STICK_L 0x00040000u
// Stick direction emulation bits (hold/trigger/release like buttons).
#define VPAD_STICK_R_EMULATION_DOWN  0x00800000u
#define VPAD_STICK_R_EMULATION_UP    0x01000000u
#define VPAD_STICK_R_EMULATION_RIGHT 0x02000000u
#define VPAD_STICK_R_EMULATION_LEFT  0x04000000u
#define VPAD_STICK_L_EMULATION_DOWN  0x08000000u
#define VPAD_STICK_L_EMULATION_UP    0x10000000u
#define VPAD_STICK_L_EMULATION_RIGHT 0x20000000u
#define VPAD_STICK_L_EMULATION_LEFT  0x40000000u

#define VPAD_TP_VALID      0u
#define VPAD_TP_INVALID_XY 3u

// Mouse sensitivity for right-stick camera: stick_delta = pixel_delta * sensitivity.
// ~200 px swipe across the screen ≈ full right-stick deflection at this value.
static constexpr float KBM_MOUSE_SENSITIVITY = 0.005f;

// Keep the tutorial's working virtual-motion camera, driven by right stick
// or mouse. The active scene lookup switches to motion off / sensitivity 0
// outside the tutorial, including Plaza. Saved preferences stay off / 0;
// no progression or tutorial-completion flags are changed.
void vpad_apply_motion_policy(uint8_t* mem) {
    rbrew_control_defaults::apply_live(mem);
}

// Write a VPADTouchData entry (8 bytes, big-endian u16 fields) at 'base'.
static void write_touch_data(uint8_t* mem, uint32_t base,
                              uint16_t x, uint16_t y,
                              uint16_t touched, uint16_t validity) {
    rbrew_write16(mem, base + 0, x);
    rbrew_write16(mem, base + 2, y);
    rbrew_write16(mem, base + 4, touched);
    rbrew_write16(mem, base + 6, validity);
}
static void write_float(uint8_t* mem, uint32_t address, float value) {
    uint32_t bits; memcpy(&bits, &value, 4); rbrew_write32(mem, address, bits);
}

// GamePad touch panel: raw 12-bit samples, calibrated to 1280x720 by
// VPADGetTPCalibratedPoint (same default calibration Cemu applies).
static constexpr float kTpOffsetX = 92.0f, kTpOffsetY = 254.0f, kTpRangeX = 3883.0f, kTpRangeY = 3694.0f;
uint16_t vpad_touch_raw_x(uint16_t screen_x) { return uint16_t(std::lround(screen_x * kTpRangeX / 854.0f + kTpOffsetX)); }
uint16_t vpad_touch_raw_y(uint16_t screen_y) { return uint16_t(std::lround(4095.0f - kTpOffsetY - screen_y * kTpRangeY / 480.0f)); }

// Radial deadzone with rescale; result clamped to the unit circle.
static void shape_stick(float& x, float& y) {
    constexpr float kDeadzone = 0.10f;
    const float length = std::sqrt(x * x + y * y);
    if (length <= kDeadzone) { x = y = 0.0f; return; }
    const float scaled = std::fmin(1.0f, (length - kDeadzone) / (1.0f - kDeadzone));
    x = x / length * scaled; y = y / length * scaled;
}
static uint32_t stick_emulation(float x, float y, uint32_t left, uint32_t right, uint32_t up, uint32_t down) {
    constexpr float kThreshold = 0.5f;
    uint32_t bits = 0;
    if (x <= -kThreshold) bits |= left;
    if (x >= kThreshold) bits |= right;
    if (y >= kThreshold) bits |= up;
    if (y <= -kThreshold) bits |= down;
    return bits;
}

// GamePad motion driven by the right stick. A PC controller has no GamePad
// gyro, yet some sections (the first tutorial's "Move GamePad to aim") aim
// with the motion sensors whatever the Motion Controls option says. The
// stick turns a virtual GamePad: yaw about the world up axis, pitch about
// the pad's X axis (Cemu's mouse-motion convention: R = Ry(yaw)·Rx(pitch),
// dir.x/y/z = R's columns, negative pitch tilts up). Angular velocity
// (revolutions/s, device axes), integrated angle and gravity stay consistent
// with that orientation. While the option is off the game aims with the
// stick and does not read these values.
struct Mat3 { float m[3][3]; };  // m[row][column]
static Mat3 mat_mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
static Mat3 rotation_x(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}
static Mat3 rotation_y(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}
struct VirtualMotion {
    Mat3 base{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};  // orientation at the last VPADSetGyroDirection
    float yaw = 0, pitch = 0;                        // degrees from base
    float angle[3] = {};                             // revolutions, device axes
    uint64_t last_ns = 0;
};
static std::mutex s_motion_lock;
static VirtualMotion s_motion;
static constexpr float kPi = 3.14159265358979f;
static constexpr float kYawRate = 240.0f, kPitchRate = 150.0f, kPitchLimit = 80.0f;  // degrees(/s)

// Advances the virtual GamePad by dt seconds of right-stick input and writes
// acc/gyro/angle/direction into the VPADStatus at buf.
void vpad_update_motion(uint8_t* mem, uint32_t buf, float rx, float ry, float dt) {
    std::lock_guard lock(s_motion_lock);
    VirtualMotion& v = s_motion;
    // Squared response keeps small deflections precise.
    const float yaw_rate = -rx * std::fabs(rx) * kYawRate, pitch_rate = -ry * std::fabs(ry) * kPitchRate;
    v.yaw = std::fmod(v.yaw + yaw_rate * dt, 360.0f);
    const float pitch = std::fmax(-kPitchLimit, std::fmin(kPitchLimit, v.pitch + pitch_rate * dt));
    const float applied_pitch_rate = dt > 0 ? (pitch - v.pitch) / dt : 0.0f;
    v.pitch = pitch;
    const Mat3 r = mat_mul(mat_mul(rotation_y(v.yaw * kPi / 180), v.base), rotation_x(v.pitch * kPi / 180));
    // World angular velocity: yaw about world Y plus pitch about the pad's X axis.
    const Mat3 yawed = mat_mul(rotation_y(v.yaw * kPi / 180), v.base);
    float world[3];
    for (int i = 0; i < 3; ++i) world[i] = yawed.m[i][0] * applied_pitch_rate + (i == 1 ? yaw_rate : 0.0f);
    float gyro[3], gravity[3];
    for (int axis = 0; axis < 3; ++axis) {
        gyro[axis] = (r.m[0][axis] * world[0] + r.m[1][axis] * world[1] + r.m[2][axis] * world[2]) / 360.0f;
        gravity[axis] = -r.m[1][axis];  // R^T (0, -1, 0)
        v.angle[axis] += gyro[axis] * dt;
    }
    for (int axis = 0; axis < 3; ++axis) {
        write_float(mem, buf + 0x1C + 4 * axis, gravity[axis]);
        write_float(mem, buf + 0x38 + 4 * axis, gyro[axis]);
        write_float(mem, buf + 0x44 + 4 * axis, v.angle[axis]);
        for (int component = 0; component < 3; ++component)  // dir.x/y/z = columns of R
            write_float(mem, buf + kVpadDirection + 12 * axis + 4 * component, r.m[component][axis]);
    }
    write_float(mem, buf + 0x28, 1.0f);  // accMagnitude: gravity only
    write_float(mem, buf + kVpadAccXY, 1.0f);
}
static float motion_seconds() {
    const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    std::lock_guard lock(s_motion_lock);
    const uint64_t last = std::exchange(s_motion.last_ns, now);
    return last ? std::fmin(0.1f, float(now - last) * 1e-9f) : 0.0f;
}
// VPADSetGyroDirection(chan, const VPADDirection* dir): the pad now faces dir.
static void VPADSetGyroDirection(CPUState* cpu) {
    const uint32_t dir = ARG1;
    if (!dir || !rbrew_memory_accessible(MEM, dir, 36)) return;
    std::lock_guard lock(s_motion_lock);
    for (int axis = 0; axis < 3; ++axis)
        for (int component = 0; component < 3; ++component)
            s_motion.base.m[component][axis] = float(rbrew_read_f32(MEM, dir + 12 * axis + 4 * component));
    s_motion.yaw = s_motion.pitch = 0;
}

// Edge state for VPADStatus trigger/release (the console reports presses and
// releases once; reporting the whole hold as "trigger" repeats every press).
static std::atomic<uint32_t> s_previous_hold{0};
uint32_t vpad_update_edges(uint32_t hold, uint32_t& trigger, uint32_t& release) {
    const uint32_t previous = s_previous_hold.exchange(hold);
    trigger = hold & ~previous;
    release = previous & ~hold;
    return previous;
}

#if defined(HAVE_SDL2)
static SDL_GameController* open_controller() {
    static SDL_GameController* s_gc = nullptr;
    if (s_gc && SDL_GameControllerGetAttached(s_gc)) return s_gc;
    if (s_gc) { SDL_GameControllerClose(s_gc); s_gc = nullptr; }
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i) || !(s_gc = SDL_GameControllerOpen(i))) continue;
        char* mapping = SDL_GameControllerMapping(s_gc);
        RBREW_LOGF(stderr, "[vpad] using controller '%s' type=%d vid=%04X pid=%04X mapping=%s\n",
                SDL_GameControllerName(s_gc), int(SDL_GameControllerGetType(s_gc)),
                SDL_GameControllerGetVendor(s_gc), SDL_GameControllerGetProduct(s_gc), mapping ? mapping : "-");
        SDL_free(mapping);
        break;
    }
    return s_gc;
}
#endif

// ---------------------------------------------------------------------------
// VPADRead(chan, buffers, count, error) → samplesRead
// ---------------------------------------------------------------------------
static void VPADRead(CPUState* cpu) {
    // ARG0 = channel (0=GamePad), ARG1 = VPADStatus* buf, ARG2 = count, ARG3 = error*
    uint32_t buf   = ARG1;
    uint32_t count = ARG2;
    uint32_t errp  = ARG3;

    if (errp) rbrew_write32(MEM, errp, 0u); // VPAD_READ_SUCCESS
    vpad_apply_motion_policy(MEM);

    if (!buf || count == 0 || !rbrew_memory_accessible(MEM, buf, kVpadStatusSize)) {
        RET = 0;
        return;
    }

    static const uint8_t kZero[kVpadStatusSize] = {};
    rbrew_write_memory(MEM, buf, kZero, kVpadStatusSize);

    uint32_t buttons = 0;
    float lx = 0.0f, ly = 0.0f; // left stick (movement), y > 0 = up
    float rx = 0.0f, ry = 0.0f; // right stick (camera)

#if defined(HAVE_SDL2)
    if (SDL_GameController* gc = open_controller()) {
        auto btn = [&](SDL_GameControllerButton b) {
            return SDL_GameControllerGetButton(gc, b) != 0;
        };
        // SDL's A/B/X/Y are positions (south/east/west/north); the GamePad
        // has B south, A east, Y west, X north.
        if (btn(SDL_CONTROLLER_BUTTON_A))             buttons |= VPAD_BUTTON_B;
        if (btn(SDL_CONTROLLER_BUTTON_B))             buttons |= VPAD_BUTTON_A;
        if (btn(SDL_CONTROLLER_BUTTON_X))             buttons |= VPAD_BUTTON_Y;
        if (btn(SDL_CONTROLLER_BUTTON_Y))             buttons |= VPAD_BUTTON_X;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT))     buttons |= VPAD_BUTTON_LEFT;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    buttons |= VPAD_BUTTON_RIGHT;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP))       buttons |= VPAD_BUTTON_UP;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN))     buttons |= VPAD_BUTTON_DOWN;
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  buttons |= VPAD_BUTTON_L;
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) buttons |= VPAD_BUTTON_R;
        if (btn(SDL_CONTROLLER_BUTTON_START))         buttons |= VPAD_BUTTON_PLUS;
        if (btn(SDL_CONTROLLER_BUTTON_BACK))          buttons |= VPAD_BUTTON_MINUS;
        if (btn(SDL_CONTROLLER_BUTTON_GUIDE))         buttons |= VPAD_BUTTON_HOME;
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSTICK))     buttons |= VPAD_BUTTON_STICK_L;
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK))    buttons |= VPAD_BUTTON_STICK_R;

        // Left trigger → ZL, right trigger → ZR (digital triggers report 32767)
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 16384) buttons |= VPAD_BUTTON_ZL;
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384) buttons |= VPAD_BUTTON_ZR;

        // SDL axes: [-32768, 32767] with y > 0 = down; the GamePad reports y > 0 = up.
        lx =  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX)  / 32767.0f;
        ly = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)  / 32767.0f;
        rx =  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
        ry = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
        shape_stick(lx, ly);
        shape_stick(rx, ry);
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
        if (keys[SDL_SCANCODE_W]) kb_ly += 1.0f; // +Y = up on the GamePad stick
        if (keys[SDL_SCANCODE_S]) kb_ly -= 1.0f;
        if (kb_lx != 0.0f && kb_ly != 0.0f) { kb_lx *= 0.7071f; kb_ly *= 0.7071f; } // diag norm
        if (kb_lx != 0.0f || kb_ly != 0.0f) { lx = kb_lx; ly = kb_ly; } // override controller

        // Mouse relative motion → right stick (camera / aim); moving the
        // mouse up looks up.
        int mdx = 0, mdy = 0;
        SDL_GetRelativeMouseState(&mdx, &mdy);
        float mrx = (float)mdx * KBM_MOUSE_SENSITIVITY;
        float mry = -(float)mdy * KBM_MOUSE_SENSITIVITY;
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

    // --- Simulated touch from mouse in Gamepad View ---
    {
        const bool touched = g_touch_state.touched;
        const uint16_t tx = touched ? vpad_touch_raw_x(g_touch_state.x) : 0;
        const uint16_t ty = touched ? vpad_touch_raw_y(g_touch_state.y) : 0;
        const uint16_t validity = touched ? VPAD_TP_VALID : VPAD_TP_INVALID_XY;
        write_touch_data(MEM, buf + kVpadTpNormal, tx, ty, touched, validity);
        write_touch_data(MEM, buf + kVpadTpFiltered1, tx, ty, touched, validity);
        write_touch_data(MEM, buf + kVpadTpFiltered2, tx, ty, touched, validity);
    }
#else
    write_touch_data(MEM, buf + kVpadTpNormal, 0, 0, 0, VPAD_TP_INVALID_XY);
    write_touch_data(MEM, buf + kVpadTpFiltered1, 0, 0, 0, VPAD_TP_INVALID_XY);
    write_touch_data(MEM, buf + kVpadTpFiltered2, 0, 0, 0, VPAD_TP_INVALID_XY);
#endif

    buttons |= stick_emulation(lx, ly, VPAD_STICK_L_EMULATION_LEFT, VPAD_STICK_L_EMULATION_RIGHT,
                               VPAD_STICK_L_EMULATION_UP, VPAD_STICK_L_EMULATION_DOWN);
    buttons |= stick_emulation(rx, ry, VPAD_STICK_R_EMULATION_LEFT, VPAD_STICK_R_EMULATION_RIGHT,
                               VPAD_STICK_R_EMULATION_UP, VPAD_STICK_R_EMULATION_DOWN);
    write_float(MEM, buf + 0x0C, lx);
    write_float(MEM, buf + 0x10, ly);
    write_float(MEM, buf + 0x14, rx);
    write_float(MEM, buf + 0x18, ry);

    vpad_update_motion(MEM, buf, rx, ry, motion_seconds());
    rbrew_write8(MEM, buf + kVpadSlideVolume, 0xFF);
    rbrew_write8(MEM, buf + kVpadSlideVolumeEx, 0xFF);
    rbrew_write8(MEM, buf + kVpadBattery, 0xC0); // full

    uint32_t trigger = 0, release = 0;
    vpad_update_edges(buttons, trigger, release);
    rbrew_write32(MEM, buf + 0x00, buttons);
    rbrew_write32(MEM, buf + 0x04, trigger);
    rbrew_write32(MEM, buf + 0x08, release);

    RET = 1; // 1 sample read
}

// VPADGetTPCalibratedPoint(chan, VPADTouchData* calibrated, const VPADTouchData* raw):
// raw 12-bit panel samples -> 1280x720 screen coordinates; touch/validity copied.
void vpad_calibrate_touch(uint16_t raw_x, uint16_t raw_y, uint16_t& x, uint16_t& y) {
    const float fx = std::fmax(0.0f, raw_x - kTpOffsetX) * (1280.0f / kTpRangeX);
    const float fy = std::fmax(0.0f, 4095.0f - raw_y - kTpOffsetY) * (720.0f / kTpRangeY);
    x = uint16_t(std::fmin(1279.0f, std::round(fx)));
    y = uint16_t(std::fmin(719.0f, std::round(fy)));
}
static void VPADGetTPCalibratedPoint(CPUState* cpu) {
    const uint32_t outp = ARG1, inp = ARG2;
    if (outp && inp && rbrew_memory_accessible(MEM, inp, 8) && rbrew_memory_accessible(MEM, outp, 8)) {
        uint16_t x = 0, y = 0;
        vpad_calibrate_touch(rbrew_read16(MEM, inp), rbrew_read16(MEM, inp + 2), x, y);
        const uint16_t touched = rbrew_read16(MEM, inp + 4), validity = rbrew_read16(MEM, inp + 6);
        write_touch_data(MEM, outp, x, y, touched, validity);
    }
    RET = 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "vpad_addrs.h"

void vpad_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(VPADRead);
    REG(VPADGetTPCalibratedPoint);
    REG(VPADSetGyroDirection);
#undef REG
}
