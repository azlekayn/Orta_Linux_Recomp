/* In-game settings menu (Dear ImGui over the OpenGL backbuffer) -- Linux.
 * See overlay.h for what it does.
 *
 * This is the GL/SDL2 sibling of overlay.cpp (the D3D11/Win32 one). The
 * Windows design needs an event queue and a lock because its window procedure
 * runs on a different thread from the present. Here everything happens on the
 * game thread: the GL backend calls ovl_present() from dev_Present(), and
 * delivers raw SDL events through g_sdlevent_hook. So there is no queue and no
 * locking -- the same logic, much less machinery.
 *
 * Pausing. The GL backend calls g_present_pause_hook after the game has drawn
 * its frame; while it says yes the backend loops (redrawing this menu over a
 * snapshot of that frame) instead of returning to the game, which is exactly
 * "held at its frame flip". */

#define NOMINMAX
#include <SDL.h>
#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <unistd.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "overlay.h"
#include "launcher.h"

/* ── Runtime hooks and live settings (src/d3d, src/apu) ── */
extern "C" {
extern void (*g_present_overlay_hook)(void *dev, void *ctx, void *rtv, unsigned w, unsigned h);
extern int  (*g_present_pause_hook)(void);
extern void (*g_sdlevent_hook)(const SDL_Event *ev);
extern volatile long  g_present_vsync;        /* -1: settings.ini */
extern volatile float g_present_crop[2];      /* -1: settings.ini */
extern SDL_Window *xbox_D3D8Window(void);
extern volatile float g_apu_live_gain;        /* -1: settings.ini */
extern volatile long  g_apu_mute;
/* recomp_manual.c */
extern volatile float g_rumble_scale;         /* -1: settings.ini */
void orta_rumble_stop_now(void);
void orta_rumble_pulse(float strength);
}

/* ── Settings ────────────────────────────────────────────────────────── */
typedef struct {
    int res;            /* render resolution choice, see launcher.h */
    int widescreen;
    int fullscreen;
    int winh;           /* window height when not fullscreen */
    int vsync;
    int fps_cap;
    int volume;         /* 0..100 */
    int sfx;            /* 0..100, effects relative to music */
    int cropx, cropy;   /* 0..20 percent per side */
    int rumble;
    int rumble_str;     /* 0..100 */
} Cfg;

static Cfg s_cfg, s_saved;

static int env_int(const char *name, int def)
{
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

static int env_pct(const char *name, int def)    /* "0.50" -> 50 */
{
    const char *e = getenv(name);
    return e && *e ? (int)floor(atof(e) * 100.0 + 0.5) : def;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int sdl_is_fullscreen(void)
{
    SDL_Window *w = xbox_D3D8Window();
    if (!w) return 0;
    return (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
}

static void cfg_load(void)
{
    {
        const char *rc = getenv("RECOMP_RES_CHOICE");
        char b[16];
        if (!rc) {             /* scale set directly (test runs): its height */
            snprintf(b, sizeof b, "%d", clampi(env_int("RECOMP_GPU_SCALE", 1), 1, 6) * 480);
            rc = b;
        }
        s_cfg.res = orta_res_index(rc);
    }
    s_cfg.widescreen = env_int("RECOMP_WIDESCREEN", 1) != 0;
    s_cfg.fullscreen = sdl_is_fullscreen();
    s_cfg.winh       = clampi(env_int("RECOMP_WINDOW_HEIGHT", 720), 240, 4320);
    s_cfg.vsync      = env_int("RECOMP_VSYNC", 0) != 0;
    s_cfg.fps_cap    = env_int("RECOMP_FPS_CAP", 60);
    s_cfg.volume     = clampi(env_pct("RECOMP_APU_GAIN", 50), 0, 100);
    s_cfg.sfx        = clampi(env_pct("RECOMP_APU_SFX", 70), 0, 100);
    s_cfg.cropx      = clampi(env_int("RECOMP_CROP_X", 0), 0, 20);
    s_cfg.cropy      = clampi(env_int("RECOMP_CROP_Y", 0), 0, 20);
    s_cfg.rumble     = env_int("RECOMP_RUMBLE", 1) != 0;
    s_cfg.rumble_str = clampi(env_int("RECOMP_RUMBLE_STRENGTH", 100), 0, 100);
    s_saved = s_cfg;
}

/* The settings that take effect at once. */
static void cfg_set_fullscreen(int on)
{
    SDL_Window *w = xbox_D3D8Window();
    if (!w) return;
    if ((SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) ==
        (on ? SDL_WINDOW_FULLSCREEN : 0u))
        return;
    SDL_SetWindowFullscreen(w, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void cfg_apply_live(void)
{
    g_present_vsync = s_cfg.vsync;
    g_present_crop[0] = (float)s_cfg.cropx / 100.0f;
    g_present_crop[1] = (float)s_cfg.cropy / 100.0f;
    g_apu_live_gain = (float)s_cfg.volume / 100.0f * (float)s_cfg.sfx / 100.0f;
    g_rumble_scale = s_cfg.rumble ? (float)s_cfg.rumble_str / 100.0f : 0.0f;
    cfg_set_fullscreen(s_cfg.fullscreen);
}

static void exe_dir(char *dir, size_t n)
{
    ssize_t k = readlink("/proc/self/exe", dir, n - 1);
    if (k <= 0) { dir[0] = 0; return; }
    dir[k] = 0;
    while (k > 0 && dir[k - 1] != '/')
        k--;
    dir[k] = 0;
}

/* Write settings.ini in the same layout main.c creates. */
static void cfg_save(void)
{
    char dir[512], path[600];
    char *adv = NULL;
    FILE *f;
    exe_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%ssettings.ini", dir);

    f = fopen(path, "rb");
    if (f) {                                   /* lift [Advanced] out as it is */
        long n;
        char *all;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        all = (char *)malloc((size_t)n + 1);
        if (all && n > 0 && fread(all, 1, (size_t)n, f) == (size_t)n) {
            char *a;
            all[n] = 0;
            a = strstr(all, "[Advanced]");
            if (a) {
                char *end = strstr(a + 10, "\n[");
                size_t len = end ? (size_t)(end + 1 - a) : strlen(a);
                adv = (char *)malloc(len + 1);
                if (adv) { memcpy(adv, a, len); adv[len] = 0; }
            }
        }
        free(all);
        fclose(f);
    }

    f = fopen(path, "wb");
    if (!f) {
        free(adv);
        return;
    }
    fprintf(f,
        "; Panzer Dragoon Orta - settings. Edit by hand, or press F1 in the game.\r\n"
        "\r\n"
        "[Display]\r\n"
        "; Render resolution:\r\n"
        ";   480 (the original, fastest), 720, 1080, 1440, 2160 or Native (your\r\n"
        ";   screen's). The game is drawn at the next whole multiple of 480 lines\r\n"
        ";   (720 -> 960, 1080 and 1440 -> 1440, 2160 -> 2400), then fitted to the screen.\r\n"
        "Resolution = %s\r\n"
        "; 1 = 16:9 widescreen, 0 = original 4:3.\r\n"
        "Widescreen = %d\r\n"
        "; Trim the black border Xbox games leave for old TVs, in percent per side (0-20).\r\n"
        "CropX = %d\r\n"
        "CropY = %d\r\n"
        "; 1 = fill the screen (borderless). Alt+Enter switches while playing.\r\n"
        "Fullscreen = %d\r\n"
        "; Window height when not fullscreen.\r\n"
        "WindowHeight = %d\r\n"
        "; Frame rate cap (the game is made for 60). 0 = no cap.\r\n"
        "FrameRateCap = %d\r\n"
        "; 1 = wait for the monitor's refresh when showing a frame (no tearing, more lag).\r\n"
        "VSync = %d\r\n"
        "\r\n"
        "[Audio]\r\n"
        "; 0 to 100.\r\n"
        "Volume = %d\r\n"
        "; Sound effects relative to music, 0 to 100.\r\n"
        "SfxVolume = %d\r\n"
        "\r\n"
        "[Controller]\r\n"
        "; 1 = the pad rumbles when the game asks it to.\r\n"
        "Rumble = %d\r\n"
        "; 0 to 100.\r\n"
        "RumbleStrength = %d\r\n",
        orta_res_value(s_cfg.res), s_cfg.widescreen, s_cfg.cropx, s_cfg.cropy,
        s_cfg.fullscreen, s_cfg.winh, s_cfg.fps_cap, s_cfg.vsync,
        s_cfg.volume, s_cfg.sfx, s_cfg.rumble, s_cfg.rumble_str);
    if (adv)
        fprintf(f, "\r\n%s", adv);
    fclose(f);
    free(adv);
    s_saved = s_cfg;
}

/* ── State ───────────────────────────────────────────────────────────── */
static int s_open;
static int s_req_toggle;
static int s_block_release;      /* closed: hold the game's input until released */

/* ── Opening and closing ─────────────────────────────────────────────── */
static int  s_focus_first;          /* put the pad/keyboard cursor on the first row */
static int  s_cat;                  /* category shown */
static Uint64 s_pulse_end;          /* "test rumble" running until this tick */

static void ovl_set_open(int on)
{
    if (on == s_open)
        return;
    if (on) {
        cfg_load();
        s_focus_first = 1;
        g_apu_mute = 1;
        orta_rumble_stop_now();
        s_open = 1;
    } else {
        if (memcmp(&s_cfg, &s_saved, sizeof s_cfg) != 0)
            cfg_save();
        if (s_pulse_end) {
            s_pulse_end = 0;
            orta_rumble_stop_now();
        }
        g_apu_mute = 0;
        s_block_release = 1;
        s_open = 0;
    }
}

extern "C" int ovl_is_open(void) { return s_open != 0; }

extern "C" int ovl_block_input(int held)
{
    if (s_open)
        return 1;
    if (s_block_release) {
        if (held)
            return 1;
        s_block_release = 0;
    }
    return 0;
}

static int ovl_pause(void) { return s_open != 0; }

/* ── SDL event translation ───────────────────────────────────────────── */
static float s_mouse_scale_x = 1.0f, s_mouse_scale_y = 1.0f;

static ImGuiKey scancode_to_key(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_TAB:      return ImGuiKey_Tab;
    case SDL_SCANCODE_LEFT:     return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT:    return ImGuiKey_RightArrow;
    case SDL_SCANCODE_UP:       return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN:     return ImGuiKey_DownArrow;
    case SDL_SCANCODE_PAGEUP:   return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_HOME:     return ImGuiKey_Home;
    case SDL_SCANCODE_END:      return ImGuiKey_End;
    case SDL_SCANCODE_RETURN:   return ImGuiKey_Enter;
    case SDL_SCANCODE_ESCAPE:   return ImGuiKey_Escape;
    case SDL_SCANCODE_SPACE:    return ImGuiKey_Space;
    case SDL_SCANCODE_BACKSPACE:return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE:   return ImGuiKey_Delete;
    case SDL_SCANCODE_Q:        return ImGuiKey_Q;
    case SDL_SCANCODE_E:        return ImGuiKey_E;
    case SDL_SCANCODE_R:        return ImGuiKey_R;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:   return ImGuiKey_LeftShift;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:    return ImGuiKey_LeftCtrl;
    default:                    return ImGuiKey_None;
    }
}

/* Called by the GL backend for every polled SDL event. */
static void ovl_sdlevent(const SDL_Event *ev)
{
    static int alt_down;
    switch (ev->type) {
    case SDL_KEYDOWN:
        if (ev->key.keysym.scancode == SDL_SCANCODE_F1) {
            if (!ev->key.repeat)
                s_req_toggle = 1;
            break;
        }
        /* Alt+Enter toggles fullscreen, as it does on Windows. */
        if ((ev->key.keysym.mod & KMOD_ALT) && ev->key.keysym.scancode == SDL_SCANCODE_RETURN) {
            if (!ev->key.repeat) {
                s_cfg.fullscreen = !sdl_is_fullscreen();
                cfg_set_fullscreen(s_cfg.fullscreen);
            }
            break;
        }
        if (!s_open)
            break;
        {
            ImGuiKey k = scancode_to_key(ev->key.keysym.scancode);
            if (k != ImGuiKey_None)
                ImGui::GetIO().AddKeyEvent(k, true);
        }
        break;
    case SDL_KEYUP:
        if (!s_open)
            break;
        {
            ImGuiKey k = scancode_to_key(ev->key.keysym.scancode);
            if (k != ImGuiKey_None)
                ImGui::GetIO().AddKeyEvent(k, false);
        }
        break;
    case SDL_TEXTINPUT:
        if (s_open)
            ImGui::GetIO().AddInputCharactersUTF8(ev->text.text);
        break;
    case SDL_MOUSEMOTION:
        if (s_open)
            ImGui::GetIO().AddMousePosEvent(ev->motion.x * s_mouse_scale_x,
                                            ev->motion.y * s_mouse_scale_y);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (!s_open)
            break;
        ImGui::GetIO().AddMousePosEvent(ev->button.x * s_mouse_scale_x,
                                        ev->button.y * s_mouse_scale_y);
        if (ev->button.button == SDL_BUTTON_LEFT)
            ImGui::GetIO().AddMouseButtonEvent(0, ev->type == SDL_MOUSEBUTTONDOWN);
        else if (ev->button.button == SDL_BUTTON_RIGHT)
            ImGui::GetIO().AddMouseButtonEvent(1, ev->type == SDL_MOUSEBUTTONDOWN);
        break;
    case SDL_MOUSEWHEEL:
        if (s_open)
            ImGui::GetIO().AddMouseWheelEvent(ev->wheel.preciseX, ev->wheel.preciseY);
        break;
    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            if (s_open) {
                ImGui::GetIO().AddFocusEvent(false);
                ImGui::GetIO().AddFocusEvent(true);
            }
        }
        break;
    case SDL_QUIT:
        if (memcmp(&s_cfg, &s_saved, sizeof s_cfg) != 0)
            cfg_save();
        orta_rumble_stop_now();
        exit(0);
        break;
    }
    (void)alt_down;
}

/* ── Pad ─────────────────────────────────────────────────────────────── */
static SDL_GameController *s_pad;
static int s_pad_tried;
static int s_pad_ok;

static void pad_open(void)
{
    int i;
    if (s_pad_tried)
        return;
    s_pad_tried = 1;
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i))
            continue;
        s_pad = SDL_GameControllerOpen(i);
        if (s_pad)
            break;
    }
}

/* Back + Start together opens or closes the menu. */
static void pad_combo(void)
{
    static int was;
    int now;
    if (!s_pad || !SDL_GameControllerGetAttached(s_pad))
        return;
    now = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_BACK)
       && SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_START);
    if (now && !was)
        s_req_toggle = 1;
    was = now;
}

static void pad_to_imgui(ImGuiIO &io)
{
    static const struct { SDL_GameControllerButton b; ImGuiKey key; } map[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_UP,    ImGuiKey_GamepadDpadUp },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN,  ImGuiKey_GamepadDpadDown },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT,  ImGuiKey_GamepadDpadLeft },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight },
        { SDL_CONTROLLER_BUTTON_A,          ImGuiKey_GamepadFaceDown },
        { SDL_CONTROLLER_BUTTON_B,          ImGuiKey_GamepadFaceRight },
        { SDL_CONTROLLER_BUTTON_X,          ImGuiKey_GamepadFaceLeft },
        { SDL_CONTROLLER_BUTTON_Y,          ImGuiKey_GamepadFaceUp },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  ImGuiKey_GamepadL1 },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ImGuiKey_GamepadR1 },
        { SDL_CONTROLLER_BUTTON_START,      ImGuiKey_GamepadStart },
        { SDL_CONTROLLER_BUTTON_BACK,       ImGuiKey_GamepadBack },
    };
    size_t i;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    for (i = 0; i < sizeof map / sizeof map[0]; i++)
        io.AddKeyEvent(map[i].key,
                       s_pad && SDL_GameControllerGetButton(s_pad, map[i].b));
    {
        const float dz = 9000.0f, span = 32767.0f - 9000.0f;
        float lx = s_pad ? (float)SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTX) : 0.0f;
        /* SDL Y is down-positive; ImGui's stick-up wants up-positive. */
        float ly = s_pad ? (float)(-SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY)) : 0.0f;
#define AX(key, v) do { float a = ((v) - dz) / span; a = a < 0 ? 0 : a > 1 ? 1 : a; \
                        io.AddKeyAnalogEvent(key, a > 0.1f, a); } while (0)
        AX(ImGuiKey_GamepadLStickLeft,  -lx);
        AX(ImGuiKey_GamepadLStickRight,  lx);
        AX(ImGuiKey_GamepadLStickUp,     ly);
        AX(ImGuiKey_GamepadLStickDown,  -ly);
#undef AX
    }
}

/* ── Look ────────────────────────────────────────────────────────────── */
static ImFont *s_font_body, *s_font_head, *s_font_small;
static float   s_font_h;            /* display height the fonts were made for */
static ImGuiStyle s_style_base;

static const ImVec4 C_ACCENT = ImVec4(0.96f, 0.76f, 0.36f, 1.00f);
static const ImVec4 C_TEXT   = ImVec4(0.93f, 0.91f, 0.87f, 1.00f);
static const ImVec4 C_DIM    = ImVec4(0.62f, 0.60f, 0.56f, 1.00f);

static void style_base(void)
{
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 0; s.ChildRounding = 4; s.FrameRounding = 4;
    s.PopupRounding = 4; s.GrabRounding = 4; s.ScrollbarRounding = 4;
    s.WindowBorderSize = 0; s.ChildBorderSize = 0; s.FrameBorderSize = 0;
    s.WindowPadding = ImVec2(16, 16); s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(12, 10); s.ScrollbarSize = 12; s.GrabMinSize = 14;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text]            = C_TEXT;
    c[ImGuiCol_TextDisabled]    = C_DIM;
    c[ImGuiCol_WindowBg]        = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg]         = ImVec4(0.06f, 0.05f, 0.05f, 0.55f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.09f, 0.08f, 0.07f, 0.98f);
    c[ImGuiCol_FrameBg]         = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(1, 1, 1, 0.12f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(1, 1, 1, 0.16f);
    c[ImGuiCol_SliderGrab]      = C_ACCENT;
    c[ImGuiCol_SliderGrabActive]= ImVec4(1.0f, 0.86f, 0.55f, 1.0f);
    c[ImGuiCol_Button]          = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.96f, 0.76f, 0.36f, 0.30f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.96f, 0.76f, 0.36f, 0.45f);
    c[ImGuiCol_Header]          = ImVec4(0.96f, 0.76f, 0.36f, 0.18f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.96f, 0.76f, 0.36f, 0.26f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.96f, 0.76f, 0.36f, 0.36f);
    c[ImGuiCol_Separator]       = ImVec4(1, 1, 1, 0.10f);
    c[ImGuiCol_NavHighlight]    = C_ACCENT;
    c[ImGuiCol_CheckMark]       = C_ACCENT;
    c[ImGuiCol_ModalWindowDimBg]= ImVec4(0, 0, 0, 0.5f);
    s_style_base = s;
}

static const char *font_candidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    NULL
};

/* Fonts sized for the display. A system sans if one exists, ImGui's own
 * otherwise. Remade when the window height changes a lot. */
static void fonts_build(float h)
{
    ImGuiIO &io = ImGui::GetIO();
    float k = h / 1080.0f, px;
    const char *body = NULL;
    int i;
    if (k < 0.5f) k = 0.5f;
    px = floorf(26.0f * k);
    io.Fonts->Clear();
    for (i = 0; font_candidates[i]; i++) {
        FILE *f = fopen(font_candidates[i], "rb");
        if (f) { fclose(f); body = font_candidates[i]; break; }
    }
    if (body) {
        s_font_body  = io.Fonts->AddFontFromFileTTF(body, px);
        s_font_small = io.Fonts->AddFontFromFileTTF(body, floorf(px * 0.8f));
        s_font_head  = io.Fonts->AddFontFromFileTTF(body, floorf(px * 1.5f));
    } else {
        ImFontConfig fc;
        fc.SizePixels = px;
        s_font_body = io.Fonts->AddFontDefault(&fc);
        fc.SizePixels = floorf(px * 0.8f);
        s_font_small = io.Fonts->AddFontDefault(&fc);
        fc.SizePixels = floorf(px * 1.5f);
        s_font_head = io.Fonts->AddFontDefault(&fc);
    }
    io.FontDefault = s_font_body;
    ImGui_ImplOpenGL3_DestroyFontsTexture();   /* new atlas on the next frame */
    ImGui::GetStyle() = s_style_base;
    ImGui::GetStyle().ScaleAllSizes(k);
    s_font_h = h;
}

/* ── Widgets ─────────────────────────────────────────────────────────── */
static const char *s_help_title, *s_help_text, *s_help_when;

/* An on/off switch: a pill and its "On"/"Off", one item (so help follows
 * the pad cursor onto it). */
static bool toggle(const char *id, int *v)
{
    ImGuiStyle &st = ImGui::GetStyle();
    float h = ImGui::GetFrameHeight(), w = h * 1.9f;
    float tw = ImGui::CalcTextSize("Off").x + st.ItemInnerSpacing.x * 2;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool changed = ImGui::InvisibleButton(id, ImVec2(w + tw, h));
    if (changed)
        *v = !*v;
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float r = h * 0.5f;
        ImU32 bg = *v ? ImGui::GetColorU32(C_ACCENT) : ImGui::GetColorU32(ImVec4(1, 1, 1, 0.16f));
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, r);
        dl->AddCircleFilled(ImVec2(*v ? p.x + w - r : p.x + r, p.y + r), r - 3.0f,
                            ImGui::GetColorU32(ImVec4(0.08f, 0.07f, 0.06f, 1)));
        dl->AddText(ImVec2(p.x + w + st.ItemInnerSpacing.x * 2, p.y + st.FramePadding.y),
                    ImGui::GetColorU32(ImGuiCol_Text), *v ? "On" : "Off");
    }
    return changed;
}

static void row(const char *label)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

static void help(const char *title, const char *text, const char *when)
{
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) {
        s_help_title = title;
        s_help_text = text;
        s_help_when = when;
    }
}

static const char *k_now  = "Takes effect immediately.";
static const char *k_next = "Takes effect the next time you start the game.";

/* ── Pages ───────────────────────────────────────────────────────────── */
static void page_display(void)
{
    static const char *winh[] = { "540", "720", "900", "1080", "1440" };
    static const int winh_v[] = { 540, 720, 900, 1080, 1440 };
    int i, wi = 1;

    row("Render resolution");
    if (s_focus_first) { ImGui::SetKeyboardFocusHere(); s_focus_first = 0; }
    {
        static char labels[ORTA_RES_COUNT][48];
        static const char *res[ORTA_RES_COUNT];
        for (i = 0; i < ORTA_RES_COUNT; i++) {
            orta_res_label(i, s_cfg.widescreen, labels[i], sizeof labels[i]);
            res[i] = labels[i];
        }
        i = s_cfg.res;
        if (ImGui::Combo("##res", &i, res, ORTA_RES_COUNT))
            s_cfg.res = i;
    }
    help("Render resolution",
         "How sharp the game is drawn. Higher is sharper but slower; the original "
         "480 is the fastest. Native matches your screen. Lower it if the game "
         "runs slowly.", k_next);

    row("Fullscreen");
    toggle("##fs", &s_cfg.fullscreen);
    help("Fullscreen", "Fill the screen with a borderless window. Alt+Enter also "
         "switches while playing.", k_now);

    row("Window height");
    for (i = 0; i < 5; i++)
        if (winh_v[i] == s_cfg.winh) wi = i;
    if (ImGui::Combo("##wh", &wi, winh, 5))
        s_cfg.winh = winh_v[wi];
    help("Window height", "The size of the window when not fullscreen.", k_next);

    row("Widescreen (16:9)");
    toggle("##ws", &s_cfg.widescreen);
    help("Widescreen", "Show the game at 16:9. Off shows the original 4:3 picture.",
         k_next);

    row("VSync");
    toggle("##vs", &s_cfg.vsync);
    help("VSync", "Wait for the monitor's refresh before showing each frame. "
         "Removes tearing but adds a little input lag.", k_now);

    row("Frame rate cap");
    {
        int cap = s_cfg.fps_cap == 0 ? 1 : 0;
        static const char *caps[] = { "60 fps (recommended)", "Off" };
        if (ImGui::Combo("##cap", &cap, caps, 2))
            s_cfg.fps_cap = cap ? 0 : 60;
    }
    help("Frame rate cap", "The game is made for 60 frames a second. Leave this at "
         "60 unless you are testing.", k_next);

    row("Crop border (sides)");
    ImGui::SliderInt("##cx", &s_cfg.cropx, 0, 20, "%d%%");
    help("Crop border", "Xbox games leave a dark border for old TVs. Cropping zooms "
         "in past it. 0 shows the whole picture.", k_now);

    row("Crop border (top/bottom)");
    ImGui::SliderInt("##cy", &s_cfg.cropy, 0, 20, "%d%%");
    help("Crop border", "Xbox games leave a dark border for old TVs. Cropping zooms "
         "in past it. 0 shows the whole picture.", k_now);
}

static void page_audio(void)
{
    row("Master volume");
    if (s_focus_first) { ImGui::SetKeyboardFocusHere(); s_focus_first = 0; }
    ImGui::SliderInt("##vol", &s_cfg.volume, 0, 100, "%d");
    help("Master volume", "Overall volume of the game.", k_now);

    row("Sound effects");
    ImGui::SliderInt("##sfx", &s_cfg.sfx, 0, 100, "%d");
    help("Sound effects", "Sound effects relative to the music. 70 matches the "
         "original mix closely.", k_now);
}

static void page_controller(void)
{
    row("Rumble");
    if (s_focus_first) { ImGui::SetKeyboardFocusHere(); s_focus_first = 0; }
    toggle("##rum", &s_cfg.rumble);
    help("Rumble", "Let the pad vibrate when the game asks it to. The game's own "
         "vibration option must also be on.", k_now);

    row("Rumble strength");
    ImGui::BeginDisabled(!s_cfg.rumble);
    ImGui::SliderInt("##rs", &s_cfg.rumble_str, 0, 100, "%d");
    help("Rumble strength", "How strongly the pad vibrates.", k_now);
    ImGui::EndDisabled();

    row("");
    ImGui::BeginDisabled(!s_cfg.rumble || !s_pad_ok);
    if (ImGui::Button("Test rumble")) {
        orta_rumble_pulse((float)s_cfg.rumble_str / 100.0f);
        s_pulse_end = SDL_GetTicks64() + 600;
    }
    ImGui::EndDisabled();
    help("Test rumble", s_pad_ok ? "A short pulse at the chosen strength."
                                 : "No controller found.", k_now);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Dummy(ImVec2(1, ImGui::GetFrameHeight() * 0.5f));
    ImGui::PushFont(s_font_small);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(C_DIM, "KEYBOARD");
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(C_DIM, "Arrows: D-pad   Enter: Start   Esc: Back\n"
                              "Z: A   X: B   A: X   S: Y\n"
                              "Q / W: left / right trigger   C / V: White / Black");
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(C_DIM, "THIS MENU");
    ImGui::TableSetColumnIndex(1);
    ImGui::TextColored(C_DIM, "F1, or Back + Start on the pad");
    ImGui::PopFont();
}

static void page_about(void)
{
    ImGui::PushTextWrapPos(0.0f);
    ImGui::PushFont(s_font_head);
    ImGui::TextUnformatted("Panzer Dragoon Orta - PC");
    ImGui::PopFont();
    ImGui::TextColored(C_DIM, "Build " __DATE__ " (Linux)");
    ImGui::Spacing();
    ImGui::TextUnformatted(
        "The original Xbox game, recompiled to run natively. "
        "You need your own copy of the game.");
    ImGui::Spacing();
    ImGui::TextColored(C_ACCENT, "SAVING");
    ImGui::TextUnformatted(
        "The game saves only at the end of each episode. Always accept the save "
        "screen after clearing an episode.");
    ImGui::Spacing();
    ImGui::TextColored(C_ACCENT, "CREDITS");
    ImGui::TextUnformatted(
        "Built on xboxrecomp by sp00nznet. Graphics behaviour checked against xemu. "
        "Menu by Dear ImGui. Panzer Dragoon Orta is (c) SEGA.");
    ImGui::PopTextWrapPos();
}

/* ── The menu ────────────────────────────────────────────────────────── */
static void draw_menu(float W, float H)
{
    static const char *cats[] = { "DISPLAY", "AUDIO", "CONTROLLER", "ABOUT" };
    static int popup_was_open;
    ImGuiStyle &st = ImGui::GetStyle();
    float pad = floorf(H * 0.05f), top = floorf(H * 0.13f), foot = floorf(H * 0.09f);
    float left_w = floorf(W * 0.20f), help_w = floorf(W * 0.27f);
    float mid_w = W - 2 * pad - left_w - help_w - 2 * st.ItemSpacing.x;
    float body_h = H - top - foot;
    bool quit_open;

    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H),
                                                  IM_COL32(8, 6, 5, 200));
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("##orta_menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                 | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground
                 | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::SetCursorPos(ImVec2(pad, pad * 0.8f));
    ImGui::PushFont(s_font_small);
    ImGui::TextColored(C_DIM, "PANZER DRAGOON ORTA  /  PC");
    ImGui::PopFont();
    ImGui::SetCursorPosX(pad);
    ImGui::PushFont(s_font_head);
    ImGui::TextUnformatted("Settings");
    ImGui::PopFont();
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float y = top - st.ItemSpacing.y;
        dl->AddLine(ImVec2(pad, y), ImVec2(W - pad, y), IM_COL32(255, 255, 255, 28));
    }

    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_Q, false))
        { s_cat = (s_cat + 3) % 4; s_focus_first = 1; }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_E, false))
        { s_cat = (s_cat + 1) % 4; s_focus_first = 1; }
    ImGui::SetCursorPos(ImVec2(pad, top));
    ImGui::BeginChild("##cats", ImVec2(left_w, body_h), 0, ImGuiWindowFlags_NoBackground);
    for (int i = 0; i < 4; i++) {
        ImGui::PushStyleColor(ImGuiCol_Text, i == s_cat ? C_ACCENT : C_TEXT);
        if (ImGui::Selectable(cats[i], i == s_cat, 0, ImVec2(0, ImGui::GetFrameHeight() * 1.2f))) {
            s_cat = i;
            s_focus_first = 0;
        }
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::PushFont(s_font_small);
    ImGui::TextColored(C_DIM, "LB / RB  or  Q / E");
    ImGui::PopFont();
    ImGui::EndChild();

    s_help_title = s_help_text = s_help_when = NULL;
    ImGui::SetCursorPos(ImVec2(pad + left_w + st.ItemSpacing.x, top));
    ImGui::BeginChild("##page", ImVec2(mid_w, body_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(s_font_small);
    ImGui::TextColored(C_ACCENT, "%s", cats[s_cat]);
    ImGui::PopFont();
    ImGui::Separator();
    if (s_cat == 3) {
        page_about();
    } else if (ImGui::BeginTable("##rows", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.52f);
        if (s_cat == 0) page_display();
        else if (s_cat == 1) page_audio();
        else page_controller();
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(W - pad - help_w, top));
    ImGui::BeginChild("##help", ImVec2(help_w, body_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (s_help_title) {
        ImGui::PushFont(s_font_head);
        ImGui::TextUnformatted(s_help_title);
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(s_help_text);
        ImGui::Spacing();
        ImGui::TextColored(C_ACCENT, "%s", s_help_when);
        ImGui::PopTextWrapPos();
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(C_DIM, "Choose a setting to see what it does.");
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(pad, H - foot + st.ItemSpacing.y));
    if (ImGui::Button("Resume"))
        ovl_set_open(0);
    ImGui::SameLine();
    if (ImGui::Button("Quit game"))
        ImGui::OpenPopup("Quit?");
    ImGui::SameLine(0, st.ItemSpacing.x * 3);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(s_font_small);
    ImGui::TextColored(C_DIM, "Changes are saved when you close this menu.   "
                              "F1 / B / Esc: resume");
    ImGui::PopFont();

    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    quit_open = ImGui::BeginPopupModal("Quit?", NULL, ImGuiWindowFlags_AlwaysAutoResize
                                       | ImGuiWindowFlags_NoTitleBar);
    if (quit_open) {
        ImGui::TextUnformatted("Quit the game?");
        ImGui::TextColored(C_DIM, "Anything since your last save will be lost.");
        ImGui::Spacing();
        if (ImGui::Button("Quit")) {
            if (memcmp(&s_cfg, &s_saved, sizeof s_cfg) != 0)
                cfg_save();
            orta_rumble_stop_now();
            exit(0);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }

    {
        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        if (!any_popup && !popup_was_open && !ImGui::IsAnyItemActive()
            && (ImGui::IsKeyPressed(ImGuiKey_Escape, false)
                || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
            ovl_set_open(0);
        popup_was_open = any_popup;
    }
    ImGui::End();
}

/* ── Present hook (game thread, GL context current) ──────────────────── */
static int s_ready;
static GLuint s_snap_tex;
static int s_snap_w, s_snap_h;
static int s_snap_valid;

/* A texture holding the frame the game had just drawn when the menu opened,
 * so re-rendering the menu each paused frame still shows the game behind it
 * (the back buffer is undefined after a swap). */
static void snapshot_capture(int w, int h)
{
    if (s_snap_tex == 0)
        glGenTextures(1, &s_snap_tex);
    glBindTexture(GL_TEXTURE_2D, s_snap_tex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, w, h, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    s_snap_w = w;
    s_snap_h = h;
    s_snap_valid = 1;
}

static void ovl_present(void *dev, void *ctx, void *rtv, unsigned w, unsigned h)
{
    static Uint64 qf, last;
    Uint64 now;
    (void)dev; (void)ctx; (void)rtv;

    if (qf == 0) {
        qf = SDL_GetPerformanceFrequency();
        last = SDL_GetPerformanceCounter();
    }

    pad_open();
    pad_combo();
    if (s_req_toggle) {
        s_req_toggle = 0;
        /* On opening: snapshot the frame the game has just drawn. On closing
         * the next game frame overwrites it anyway. */
        if (!s_open)
            snapshot_capture((int)w, (int)h);
        ovl_set_open(!s_open);
    }
    if (s_pulse_end && SDL_GetTicks64() >= s_pulse_end) {
        s_pulse_end = 0;
        orta_rumble_stop_now();
    }
    if (!s_open)
        return;

    /* The mouse arrives in window (logical) coordinates but ImGui works in
     * drawable pixels, which differ on a HiDPI display. */
    {
        SDL_Window *win = xbox_D3D8Window();
        int ww = (int)w, wh = (int)h;
        if (win) SDL_GetWindowSize(win, &ww, &wh);
        s_mouse_scale_x = ww > 0 ? (float)w / (float)ww : 1.0f;
        s_mouse_scale_y = wh > 0 ? (float)h / (float)wh : 1.0f;
    }

    if (!s_ready) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = NULL;
        io.LogFilename = NULL;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.BackendPlatformName = "orta_overlay_gl";
        style_base();
        if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
            ImGui::DestroyContext();
            ovl_set_open(0);
            return;
        }
        s_ready = 1;
    }
    if (s_font_h <= 0.0f || fabsf((float)h - s_font_h) > s_font_h * 0.15f)
        fonts_build((float)h);

    ImGuiIO &io = ImGui::GetIO();
    now = SDL_GetPerformanceCounter();
    io.DeltaTime = qf ? (float)((double)(now - last) / (double)qf) : 1.0f / 60.0f;
    if (io.DeltaTime <= 0.0f || io.DeltaTime > 0.5f)
        io.DeltaTime = 1.0f / 60.0f;
    last = now;
    io.DisplaySize = ImVec2((float)w, (float)h);

    s_pad_ok = s_pad && SDL_GameControllerGetAttached(s_pad);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    /* Draw the snapshot first, then the menu on top. */
    if (s_snap_valid) {
        ImGui::GetBackgroundDrawList()->AddImage(
            (ImTextureID)(intptr_t)s_snap_tex, ImVec2(0, 0), ImVec2((float)w, (float)h));
    }
    draw_menu((float)w, (float)h);
    pad_to_imgui(io);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (memcmp(&s_cfg, &s_saved, sizeof s_cfg) != 0 || s_open)
        cfg_apply_live();
}

/* ── Install ─────────────────────────────────────────────────────────── */
extern "C" void ovl_install(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    cfg_load();
    g_sdlevent_hook = ovl_sdlevent;
    g_present_overlay_hook = ovl_present;
    g_present_pause_hook = ovl_pause;
}
