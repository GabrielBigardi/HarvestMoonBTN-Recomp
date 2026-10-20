/* SDL2 frontend: window/presentation, keyboard + game controller input (XInput/Xbox pads on
 * Windows, evdev on Linux), audio output and frame pacing.  Also implements the headless
 * test mode (scripted input + screenshots) used for automated verification. */
#include "psx.h"
#include "gpu.h"
#include <SDL.h>
#ifndef _WIN32
#include "sdl_dyn.h"
#endif
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

uint16_t g_pad_buttons[2];
bool g_pad_connected[2];

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioDeviceID audio;
static int audio_ok;
static uint32_t *frame_px;
static int fullscreen;
static uint64_t perf_next;
static int shot_request;

#define MAX_PADS 4
static SDL_GameController *pads[MAX_PADS];

/* ------------------------------------------------------------------ input bindings */
/* PS buttons in a fixed order; each can be bound to several keys and controller inputs. */
static const struct { const char *name; uint16_t bit; } ps_buttons[14] = {
    { "up", PAD_UP }, { "down", PAD_DOWN }, { "left", PAD_LEFT }, { "right", PAD_RIGHT },
    { "cross", PAD_CROSS }, { "circle", PAD_CIRCLE }, { "square", PAD_SQUARE }, { "triangle", PAD_TRIANGLE },
    { "l1", PAD_L1 }, { "r1", PAD_R1 }, { "l2", PAD_L2 }, { "r2", PAD_R2 },
    { "start", PAD_START }, { "select", PAD_SELECT },
};
#define MAX_BIND 6
static char bind_keys[14][128] = {
    "Up", "Down", "Left", "Right",
    "X,Space,K", "Z,L", "S,J", "A,I",
    "Q", "W", "1", "2",
    "Return", "Right Shift,Backspace",
};
/* SDL game controller names: a b x y back start leftshoulder rightshoulder dpup ...;
 * "lefttrigger"/"righttrigger" are analog triggers */
static char bind_pad[14][128] = {
    "dpup", "dpdown", "dpleft", "dpright",
    "a", "b", "x", "y",
    "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger",
    "start", "back",
};
static SDL_Scancode key_codes[14][MAX_BIND];
static int pad_codes[14][MAX_BIND];     /* SDL_GameControllerButton, or 1000+axis for triggers */
static int stick_as_dpad = 1;

bool platform_set_binding(const char *key, const char *val) {
    int is_key = !strncmp(key, "key_", 4), is_pad = !strncmp(key, "pad_", 4);
    if (!strcmp(key, "stick_as_dpad")) { stick_as_dpad = atoi(val); return true; }
    if (!is_key && !is_pad) return false;
    for (int i = 0; i < 14; i++) {
        if (!strcmp(key + 4, ps_buttons[i].name)) {
            snprintf(is_key ? bind_keys[i] : bind_pad[i], 128, "%s", val);
            return true;
        }
    }
    return false;
}

static void resolve_bindings(void) {
    for (int i = 0; i < 14; i++) {
        for (int k = 0; k < MAX_BIND; k++) { key_codes[i][k] = SDL_SCANCODE_UNKNOWN; pad_codes[i][k] = -1; }
        char buf[128], *save = NULL;
        snprintf(buf, sizeof buf, "%s", bind_keys[i]);
        int n = 0;
        for (char *t = strtok_r(buf, ",", &save); t && n < MAX_BIND; t = strtok_r(NULL, ",", &save)) {
            while (*t == ' ') t++;
            SDL_Scancode sc = SDL_GetScancodeFromName(t);
            if (sc == SDL_SCANCODE_UNKNOWN) LOGW("unknown key name '%s' for %s", t, ps_buttons[i].name);
            else key_codes[i][n++] = sc;
        }
        snprintf(buf, sizeof buf, "%s", bind_pad[i]);
        n = 0;
        for (char *t = strtok_r(buf, ",", &save); t && n < MAX_BIND; t = strtok_r(NULL, ",", &save)) {
            while (*t == ' ') t++;
            if (!strcmp(t, "lefttrigger")) { pad_codes[i][n++] = 1000 + SDL_CONTROLLER_AXIS_TRIGGERLEFT; continue; }
            if (!strcmp(t, "righttrigger")) { pad_codes[i][n++] = 1000 + SDL_CONTROLLER_AXIS_TRIGGERRIGHT; continue; }
            SDL_GameControllerButton gb = SDL_GameControllerGetButtonFromString(t);
            if (gb == SDL_CONTROLLER_BUTTON_INVALID) LOGW("unknown controller button '%s' for %s", t, ps_buttons[i].name);
            else pad_codes[i][n++] = gb;
        }
    }
}

/* ------------------------------------------------------------------ input script (testing) */
typedef struct { int frame, dur, end, period; uint16_t btn; } ScriptEv;
static ScriptEv script[1024];
static int nscript;

static uint16_t parse_buttons(const char *s) {
    static const struct { const char *n; uint16_t b; } names[] = {
        { "UP", PAD_UP }, { "DOWN", PAD_DOWN }, { "LEFT", PAD_LEFT }, { "RIGHT", PAD_RIGHT },
        { "CROSS", PAD_CROSS }, { "X", PAD_CROSS }, { "CIRCLE", PAD_CIRCLE }, { "O", PAD_CIRCLE },
        { "SQUARE", PAD_SQUARE }, { "TRIANGLE", PAD_TRIANGLE }, { "L1", PAD_L1 }, { "R1", PAD_R1 },
        { "L2", PAD_L2 }, { "R2", PAD_R2 }, { "START", PAD_START }, { "SELECT", PAD_SELECT },
    };
    uint16_t b = 0;
    char tok[32];
    while (*s) {
        int n = 0;
        while (*s && *s != '+' && n < 31) tok[n++] = (char)toupper((unsigned char)*s++);
        tok[n] = 0;
        if (*s == '+') s++;
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(tok, names[i].n)) b |= names[i].b;
    }
    return b;
}

static void parse_script(const char *s) {
    /* "frame:BUTTONS[:dur],..." or repeating "start-end/period:BUTTONS[:dur]" */
    static char buf[65536];
    snprintf(buf, sizeof buf, "%s", s);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && nscript < 1024; tok = strtok_r(NULL, ",", &save)) {
        ScriptEv e = { 0, 6, 0, 0, 0 };
        char *c1 = strchr(tok, ':');
        if (!c1) continue;
        *c1 = 0;
        char *dash = strchr(tok, '-'), *slash = strchr(tok, '/');
        e.frame = atoi(tok);
        if (dash && slash) { e.end = atoi(dash + 1); e.period = atoi(slash + 1); }
        char *c2 = strchr(c1 + 1, ':');
        if (c2) { *c2 = 0; e.dur = atoi(c2 + 1); }
        e.btn = parse_buttons(c1 + 1);
        script[nscript++] = e;
    }
}

static uint16_t script_buttons(int frame);
/* testing: pseudo-random button presses that still let the game make progress */
static uint16_t fuzz_buttons(int frame) {
    static uint32_t rng;
    static uint16_t cur;
    static int hold;
    if (!g_cfg.fuzz_seed || frame < g_cfg.fuzz_start) return 0;
    if (!rng) rng = (uint32_t)g_cfg.fuzz_seed * 2654435761u + 1;
    if (--hold > 0) return cur;
    rng = rng * 1664525u + 1013904223u;
    uint32_t r = rng >> 8;
    static const uint16_t dirs[4] = { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT };
    static const uint16_t acts[8] = { PAD_CROSS, PAD_CROSS, PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1 };
    cur = 0;
    switch (r % 10) {
    case 0: case 1: case 2: case 3: cur = dirs[(r >> 4) & 3]; hold = 10 + (int)((r >> 8) % 50); break;
    case 4: case 5: case 6: cur = acts[(r >> 4) & 7]; hold = 3; break;
    case 7: cur = dirs[(r >> 4) & 3] | PAD_CROSS; hold = 8; break;
    case 8: cur = ((r >> 4) % 23 == 0) ? PAD_START : (((r >> 4) % 31 == 0) ? PAD_SELECT : 0); hold = 4; break;
    default: cur = 0; hold = 10; break;
    }
    return cur;
}

static uint16_t script_buttons(int frame) {
    uint16_t b = fuzz_buttons(frame);
    for (int i = 0; i < nscript; i++) {
        const ScriptEv *e = &script[i];
        if (e->period > 0) {
            if (frame >= e->frame && frame <= e->end && (frame - e->frame) % e->period < e->dur) b |= e->btn;
        } else if (frame >= e->frame && frame < e->frame + e->dur) {
            b |= e->btn;
        }
    }
    return b;
}

/* ------------------------------------------------------------------ screenshots */
static void write_bmp(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t rowb = (uint32_t)w * 3, pad = (4 - rowb % 4) % 4, img = (rowb + pad) * (uint32_t)h;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t fsz = 54 + img;
    memcpy(hdr + 2, &fsz, 4);
    uint32_t v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    int32_t ww = w, hh = h;
    memcpy(hdr + 18, &ww, 4);
    memcpy(hdr + 22, &hh, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    uint8_t row[1024 * 3 + 4] = { 0 };
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t p = px[y * w + x];
            row[x * 3 + 0] = (uint8_t)p;
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)(p >> 16);
        }
        fwrite(row, 1, rowb + pad, f);
    }
    fclose(f);
}

static void save_screenshot(const uint32_t *px, int w, int h, uint64_t frame) {
    char path[1400];
#ifdef _WIN32
    _mkdir(g_cfg.screenshot_dir);
#else
    mkdir(g_cfg.screenshot_dir, 0755);
#endif
    snprintf(path, sizeof path, "%s/frame_%06llu.bmp", g_cfg.screenshot_dir, (unsigned long long)frame);
    write_bmp(path, px, w, h);
    LOGI("screenshot %s (%dx%d)", path, w, h);
}

/* ------------------------------------------------------------------ init */
static void open_pad(int index) {
    if (!SDL_IsGameController(index)) return;
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i]) {
            pads[i] = SDL_GameControllerOpen(index);
            if (pads[i]) LOGI("controller connected: %s", SDL_GameControllerName(pads[i]));
            return;
        }
    }
}

/* Open the default output device.  Skipped when the system has no output device at all
 * (WASAPI would otherwise stall for seconds); a device plugged in later is picked up via
 * SDL_AUDIODEVICEADDED. */
static void open_audio(void) {
    if (audio_ok) return;
    if (SDL_GetNumAudioDevices(0) <= 0) {
        LOGW("no audio output device found - running without sound until one is connected");
        return;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) {
        audio_ok = 1;
        SDL_PauseAudioDevice(audio, 0);
        LOGI("audio: %s", SDL_GetCurrentAudioDriver());
    } else {
        LOGW("cannot open audio device: %s", SDL_GetError());
    }
}

int platform_init(void) {
    frame_px = calloc(1024 * 512, sizeof(uint32_t));
    if (g_cfg.input_script[0]) parse_script(g_cfg.input_script);
    if (g_cfg.headless) return 0;
#ifndef _WIN32
    const char *err = sdl_dyn_load();
    if (err) { fprintf(stderr, "%s\n", err); return 1; }
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    resolve_bindings();
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");          /* pacing is done by the emulated video clock */
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    int sc = g_cfg.scale > 0 ? g_cfg.scale : 3;
    win = SDL_CreateWindow("Harvest Moon: Back to Nature", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           320 * sc, 240 * sc, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_cfg.smooth ? "1" : "0");
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 1024, 512);
    if (g_cfg.fullscreen) { SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP); fullscreen = 1; }

    open_audio();

    for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
    perf_next = SDL_GetPerformanceCounter();
    LOGI("platform init took %.0f ms", (double)(perf_next - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
    return 0;
}

void platform_shutdown(void) {
    if (g_cfg.headless) return;
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_Quit();
}

void platform_error_box(const char *title, const char *msg) {
    if (g_cfg.headless || !win) return;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, win);
}

static FILE *wav_file;
static uint32_t wav_frames;

static void wav_finish(void) {
    if (!wav_file) return;
    uint32_t data = wav_frames * 4, riff = 36 + data;
    fseek(wav_file, 4, SEEK_SET); fwrite(&riff, 4, 1, wav_file);
    fseek(wav_file, 40, SEEK_SET); fwrite(&data, 4, 1, wav_file);
    fclose(wav_file);
    wav_file = NULL;
}

void platform_audio_push(const int16_t *stereo, int frames) {
    if (g_cfg.wav_path[0]) {
        if (!wav_file) {
            wav_file = fopen(g_cfg.wav_path, "wb");
            if (wav_file) {
                static const uint8_t hdr[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                    0x44,0xAC,0,0,0x10,0xB1,2,0,4,0,16,0,'d','a','t','a',0,0,0,0 };
                fwrite(hdr, 1, 44, wav_file);
                atexit(wav_finish);
            }
        }
        if (wav_file) { fwrite(stereo, 4, (size_t)frames, wav_file); wav_frames += (uint32_t)frames; }
    }
    if (audio_ok && !g_cfg.headless && !g_cfg.unthrottled) SDL_QueueAudio(audio, stereo, (Uint32)frames * 4);
}

/* ------------------------------------------------------------------ per-frame */
static uint16_t read_controllers(void) {
    uint16_t b = 0;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < 14; i++)
        for (int k = 0; k < MAX_BIND && key_codes[i][k] != SDL_SCANCODE_UNKNOWN; k++)
            if (ks[key_codes[i][k]]) b |= ps_buttons[i].bit;
    for (int p = 0; p < MAX_PADS; p++) {
        SDL_GameController *c = pads[p];
        if (!c) continue;
        for (int i = 0; i < 14; i++) {
            for (int k = 0; k < MAX_BIND && pad_codes[i][k] >= 0; k++) {
                int code = pad_codes[i][k];
                if (code >= 1000) { if (SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)(code - 1000)) > 16000) b |= ps_buttons[i].bit; }
                else if (SDL_GameControllerGetButton(c, (SDL_GameControllerButton)code)) b |= ps_buttons[i].bit;
            }
        }
        if (stick_as_dpad) {
            int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
            int ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
            const int dz = 16000;
            if (lx < -dz) b |= PAD_LEFT;
            if (lx > dz) b |= PAD_RIGHT;
            if (ly < -dz) b |= PAD_UP;
            if (ly > dz) b |= PAD_DOWN;
        }
    }
    return b;
}

static double t_upload, t_present;
static int t_n;

static void present(int w, int h) {
    uint64_t f = SDL_GetPerformanceFrequency(), t0 = SDL_GetPerformanceCounter();
    int ww, wh;
    SDL_GetRendererOutputSize(ren, &ww, &wh);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (w > 0 && h > 0) {
        SDL_Rect src = { 0, 0, w, h };
        SDL_UpdateTexture(tex, &src, frame_px, w * 4);
        double aspect = g_cfg.aspect_4_3 ? 4.0 / 3.0 : (double)w / h;
        int dw = ww, dh = (int)(ww / aspect);
        if (dh > wh) { dh = wh; dw = (int)(wh * aspect); }
        SDL_Rect dst = { (ww - dw) / 2, (wh - dh) / 2, dw, dh };
        SDL_RenderCopy(ren, tex, &src, &dst);
    }
    uint64_t t1 = SDL_GetPerformanceCounter();
    SDL_RenderPresent(ren);
    uint64_t t2 = SDL_GetPerformanceCounter();
    t_upload += (double)(t1 - t0) * 1000.0 / (double)f;
    t_present += (double)(t2 - t1) * 1000.0 / (double)f;
    if (++t_n == 120) {
        SDL_RendererInfo ri;
        SDL_GetRendererInfo(ren, &ri);
        static uint64_t last;
        uint64_t nowt = SDL_GetPerformanceCounter();
        double fps = last ? 120.0 * (double)f / (double)(nowt - last) : 0;
        last = nowt;
        LOGD("present: renderer=%s upload %.2f ms, present %.2f ms (avg), %.2f fps", ri.name, t_upload / t_n, t_present / t_n, fps);
        t_upload = t_present = 0;
        t_n = 0;
    }
}

static void dump_ram(uint64_t frame) {
    for (int k = 0; k < 8; k++) {
        if (g_cfg.dump_frame[k] && (uint64_t)g_cfg.dump_frame[k] == frame) {
            char path[1400];
#ifdef _WIN32
            _mkdir(g_cfg.screenshot_dir);
#else
            mkdir(g_cfg.screenshot_dir, 0755);
#endif
            snprintf(path, sizeof path, "%s/ram_%06llu.bin", g_cfg.screenshot_dir, (unsigned long long)frame);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(g_ram, 1, sizeof g_ram, f); fwrite(g_scratch, 1, sizeof g_scratch, f); fclose(f); }
            LOGI("RAM dump %s", path);
            /* full VRAM as a 1024x512 image */
            static uint32_t vpx[1024 * 512];
            const uint16_t *vr = gpu_vram();
            for (int i = 0; i < 1024 * 512; i++) {
                uint16_t c = vr[i];
                uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
                vpx[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
            }
            snprintf(path, sizeof path, "%s/vram_%06llu.bmp", g_cfg.screenshot_dir, (unsigned long long)frame);
            write_bmp(path, vpx, 1024, 512);
        }
    }
}

/* Wait until the next emulated frame is due (59.29 Hz NTSC).  When an audio device is
 * present the queue depth is used as the clock instead, so audio never drifts. */
static int fast_forward;

static void throttle(void) {
    if (g_cfg.unthrottled) return;
    if (fast_forward) { perf_next = SDL_GetPerformanceCounter(); if (audio_ok) SDL_ClearQueuedAudio(audio); return; }
    uint64_t freq = SDL_GetPerformanceFrequency();
    double period = (double)freq / 59.2898;          /* NTSC PS1 frame rate */
    if (audio_ok) {
        /* keep ~3-5 frames of audio queued: nudge the frame period by up to 1% so the
           emulated clock follows the sound card's clock without audible gaps */
        double queued = (double)SDL_GetQueuedAudioSize(audio) / 4.0 / 735.0;   /* in frames */
        if (queued > 5.0) period *= 1.0 + 0.002 * (queued - 5.0 > 5.0 ? 5.0 : queued - 5.0);
        else if (queued < 3.0) period *= 1.0 - 0.003 * (3.0 - queued);
    }
    perf_next += (uint64_t)period;
    uint64_t now = SDL_GetPerformanceCounter();
    if (now > perf_next + (uint64_t)period * 4 || perf_next > now + (uint64_t)period * 4) perf_next = now;   /* resync after a stall */
    for (;;) {
        now = SDL_GetPerformanceCounter();
        if (now >= perf_next) break;
        uint64_t us = (perf_next - now) * 1000000u / freq;
        if (us > 2000) SDL_Delay((Uint32)(us / 1000 - 1));   /* coarse sleep, then spin the rest */
    }
}

void platform_frame(void) {
    uint64_t frame = frame_count();
    dump_ram(frame);
    int w = 0, h = 0;
    bool need_pixels = !g_cfg.headless || (g_cfg.screenshot_every && frame % (uint64_t)g_cfg.screenshot_every == 0) || shot_request;
    if (need_pixels) gpu_render_display(frame_px, &w, &h);
    if (g_cfg.screenshot_every && frame % (uint64_t)g_cfg.screenshot_every == 0) save_screenshot(frame_px, w, h, frame);

    if (g_cfg.headless) {
        g_pad_buttons[0] = script_buttons((int)frame);
        if (g_cfg.max_frames && frame >= (uint64_t)g_cfg.max_frames) g_quit = 1;
        return;
    }

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT: g_quit = 1; break;
        case SDL_CONTROLLERDEVICEADDED: open_pad(ev.cdevice.which); break;
        case SDL_AUDIODEVICEADDED: if (!ev.adevice.iscapture) open_audio(); break;
        case SDL_CONTROLLERDEVICEREMOVED:
            for (int i = 0; i < MAX_PADS; i++)
                if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == ev.cdevice.which) {
                    SDL_GameControllerClose(pads[i]);
                    pads[i] = NULL;
                }
            break;
        case SDL_KEYDOWN:
            if (ev.key.repeat) break;
            if (ev.key.keysym.scancode == SDL_SCANCODE_F11 ||
                (ev.key.keysym.scancode == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            } else if (ev.key.keysym.scancode == SDL_SCANCODE_F12) {
                shot_request = 1;
            } else if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE && fullscreen) {
                fullscreen = 0;
                SDL_SetWindowFullscreen(win, 0);
            }
            break;
        }
    }
    {
        const Uint8 *ks = SDL_GetKeyboardState(NULL);
        fast_forward = ks[SDL_SCANCODE_TAB] && g_cfg.allow_fast_forward;
    }
    uint16_t b = read_controllers();
    if (!(SDL_GetModState() & KMOD_ALT)) g_pad_buttons[0] = b | script_buttons((int)frame);
    if (shot_request) { save_screenshot(frame_px, w, h, frame); shot_request = 0; }
    present(w, h);
    throttle();
    if (g_cfg.max_frames && frame >= (uint64_t)g_cfg.max_frames) g_quit = 1;
}
