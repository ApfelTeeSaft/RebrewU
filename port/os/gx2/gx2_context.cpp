// GX2 context and initialization
// GX2Init, GX2Shutdown, GX2SetContextState, GX2DrawDone, swap chain
//
// Opens an SDL2 window with an OpenGL 4.1 core context.
// TV / GamePad view toggle with simulated touch input.
// gx2_render_init() loads GL functions + stub shader after context creation;
// GX2SwapScanBuffers blits the game FBO to the window.

#include "../os_common.h"
#include "../../runtime/screen_mode.h"
#include "gx2_render.h"
#include <stdio.h>

// Definitions for shared screen-mode state (declared in screen_mode.h)
ScreenMode g_screen_mode  = ScreenMode::TV;
TouchState g_touch_state  = { false, 0, 0 };

// SDL2 and OpenGL are optional at this phase — compile to stubs if not available
#if defined(HAVE_SDL2)
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

static SDL_Window*   s_window    = nullptr;
static SDL_GLContext s_glctx     = nullptr;
static bool          s_tv_enabled = true;

// GamePad screen resolution (hardware fixed)
static constexpr int DRC_W = 854;
static constexpr int DRC_H = 480;

// Update the window title to reflect the current view mode
static void update_window_title() {
    if (!s_window) return;
    if (g_screen_mode == ScreenMode::TV)
        SDL_SetWindowTitle(s_window, "Gambit  [TV View]  (Tab = switch | F11 = fullscreen | WASD+mouse)");
    else
        SDL_SetWindowTitle(s_window, "Gambit  [Gamepad View]  (Tab = switch | F11 = fullscreen | click = touch)");
}

// Switch between TV and Gamepad view, adjusting mouse capture accordingly
static void toggle_screen_mode() {
    if (g_screen_mode == ScreenMode::TV) {
        g_screen_mode = ScreenMode::Gamepad;
        SDL_SetRelativeMouseMode(SDL_FALSE); // free the cursor for touch sim
        g_touch_state = { false, 0, 0 };
    } else {
        g_screen_mode = ScreenMode::TV;
        SDL_SetRelativeMouseMode(SDL_TRUE);  // capture cursor for camera
        g_touch_state = { false, 0, 0 };
    }
    update_window_title();
}

// Map a window-space mouse position to GamePad touch coordinates [0,853] x [0,479]
static void map_mouse_to_touch(int mx, int my) {
    int ww, wh;
    SDL_GetWindowSize(s_window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;

    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= ww) mx = ww - 1;
    if (my >= wh) my = wh - 1;

    g_touch_state.x = (uint16_t)((mx * (DRC_W - 1)) / (ww - 1));
    g_touch_state.y = (uint16_t)((my * (DRC_H - 1)) / (wh - 1));
}

static bool gx2_platform_open_window() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[gx2] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    s_window = SDL_CreateWindow("Gambit  [TV View]  (Tab = switch)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1920, 1080, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!s_window) {
        fprintf(stderr, "[gx2] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    s_glctx = SDL_GL_CreateContext(s_window);
    if (!s_glctx) {
        fprintf(stderr, "[gx2] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(s_window, s_glctx);
    SDL_GL_SetSwapInterval(1); // vsync

    // Start in TV view with mouse captured for camera control.
    // Raise the window first so it has focus, then flush any spurious events
    // (e.g. SDL_QUIT) that SDL may have queued during window creation or
    // SDL_SetRelativeMouseMode before the window was visible.
    SDL_RaiseWindow(s_window);
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    SDL_SetRelativeMouseMode(SDL_TRUE);
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    g_screen_mode = ScreenMode::TV;

    // Initialise the GL 3.x+ rendering back-end now that the context exists
    gx2_render_init();
    return true;
}
#endif // HAVE_SDL2

// ---------------------------------------------------------------------------
// GX2Init(attribs) → TRUE
// ---------------------------------------------------------------------------
static void GX2Init(CPUState* cpu) {
#if defined(HAVE_SDL2)
    if (!s_window) {
        gx2_platform_open_window();
    }
#else
    fprintf(stderr, "[gx2] GX2Init called (SDL2 not available, stub)\n");
#endif
    RET = 1;
}

// GX2Shutdown()
static void GX2Shutdown(CPUState* cpu) {
#if defined(HAVE_SDL2)
    if (s_glctx) { SDL_GL_DeleteContext(s_glctx); s_glctx = nullptr; }
    if (s_window) { SDL_DestroyWindow(s_window);   s_window = nullptr; }
    SDL_Quit();
#endif
}

// GX2SetContextState(state*)
static void GX2SetContextState(CPUState* cpu) {
    (void)cpu;
}

// GX2GetContextStateDisplayList(state, outPtr, outSize)
static void GX2GetContextStateDisplayList(CPUState* cpu) {
    if (ARG1) rbrew_write32(MEM, ARG1, 0u);
    if (ARG2) rbrew_write32(MEM, ARG2, 0u);
}

// GX2DrawDone()
static void GX2DrawDone(CPUState* cpu) { (void)cpu; }

// GX2Flush()
static void GX2Flush(CPUState* cpu) { (void)cpu; }

// GX2WaitForVsync() — vsync handled by SDL_GL_SetSwapInterval(1)
static void GX2WaitForVsync(CPUState* cpu) { (void)cpu; }

// GX2CopyColorBufferToScanBuffer(colorBuffer*, scanTarget)
// scanTarget: 1 = GX2_SCAN_TARGET_TV, 4 = GX2_SCAN_TARGET_DRC
static void GX2CopyColorBufferToScanBuffer(CPUState* cpu) {
    gx2_render_copy_to_scan(ARG0, ARG1, MEM);
}

// GX2SwapScanBuffers()
// Called every frame — pump SDL events, blit game FBO to window, swap buffers.
static void GX2SwapScanBuffers(CPUState* cpu) {
#if defined(HAVE_SDL2)
    static uint32_t s_swap_count = 0;
    if (++s_swap_count == 1)
        fprintf(stderr, "[gx2] GX2SwapScanBuffers called for the first time\n");
    if (s_window) {
        // Pump SDL events first
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                fprintf(stderr, "[gx2] Window close requested\n");
                exit(0);

            case SDL_KEYDOWN:
                if (ev.key.keysym.scancode == SDL_SCANCODE_TAB) {
                    toggle_screen_mode();
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_F11) {
                    // Toggle fullscreen / windowed
                    bool is_full = (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                    SDL_SetWindowFullscreen(s_window,
                        is_full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                break;

            case SDL_CONTROLLERDEVICEADDED:
                // SDL2 will make the new controller visible to SDL_IsGameController;
                // VPADRead re-scans every frame so it picks it up automatically.
                fprintf(stderr, "[vpad] Controller connected (index %d)\n", ev.cdevice.which);
                break;

            case SDL_CONTROLLERDEVICEREMOVED:
                fprintf(stderr, "[vpad] Controller disconnected (instance %d)\n", ev.cdevice.which);
                // VPADRead detects SDL_GameControllerGetAttached==false and re-scans.
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (g_screen_mode == ScreenMode::Gamepad &&
                    ev.button.button == SDL_BUTTON_LEFT) {
                    map_mouse_to_touch(ev.button.x, ev.button.y);
                    g_touch_state.touched = true;
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (g_screen_mode == ScreenMode::Gamepad &&
                    ev.button.button == SDL_BUTTON_LEFT) {
                    g_touch_state.touched = false;
                }
                break;

            case SDL_MOUSEMOTION:
                if (g_screen_mode == ScreenMode::Gamepad && g_touch_state.touched) {
                    map_mouse_to_touch(ev.motion.x, ev.motion.y);
                }
                break;

            default:
                break;
            }
        }

        // Blit game FBO to window and swap
        bool gamepad = (g_screen_mode == ScreenMode::Gamepad);
        gx2_render_present(s_window, gamepad);
    }
#endif
    (void)cpu;
}

// GX2SetTVEnable(enable)
static void GX2SetTVEnable(CPUState* cpu) {
#if defined(HAVE_SDL2)
    s_tv_enabled = (ARG0 != 0);
#else
    (void)cpu;
#endif
}

// GX2SetDRCEnable(enable)
static void GX2SetDRCEnable(CPUState* cpu) { (void)cpu; }

// GX2GetSystemTVScanMode() → GX2_TV_SCAN_MODE_1080P (4)
static void GX2GetSystemTVScanMode(CPUState* cpu) { RET = 4; }

// GX2GetSystemDRCScanMode() → GX2_DRC_SCAN_MODE_DISABLED (0)
static void GX2GetSystemDRCScanMode(CPUState* cpu) { RET = 0; }

static void GX2SetTVGamma(CPUState* cpu)  { (void)cpu; }
static void GX2SetDRCScale(CPUState* cpu) { (void)cpu; }
static void GX2GetLastFrame(CPUState* cpu){ (void)cpu; }

static void GX2GetLastFrameGamma(CPUState* cpu) { cpu->f[1] = 2.2; }

static void GX2SetDefaultState(CPUState* cpu) { (void)cpu; }

static void GX2CalcContextStateSize(CPUState* cpu) {
    if (ARG0) rbrew_write32(MEM, ARG0, 0x1000u);
}

static void GX2SetupContextStateEx(CPUState* cpu) { (void)cpu; }

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "gx2_addrs.h"

void gx2_context_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(GX2Init);
    REG(GX2SetContextState);
    REG(GX2GetContextStateDisplayList);
    REG(GX2DrawDone);
    REG(GX2Flush);
    REG(GX2WaitForVsync);
    REG(GX2CopyColorBufferToScanBuffer);
    REG(GX2SwapScanBuffers);
    REG(GX2SetTVEnable);
    REG(GX2SetDRCEnable);
    REG(GX2SetTVGamma);
    REG(GX2SetDRCScale);
    REG(GX2GetLastFrame);
    REG(GX2SetDefaultState);
    REG(GX2SetupContextStateEx);
#undef REG
}