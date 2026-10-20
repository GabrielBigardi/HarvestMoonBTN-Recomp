/* Entry point: configuration, game data discovery (the HarvestMoonBTN.pak data pack, or a
 * .cue/.bin disc image as an alternative), boot (loads the game executable from the game data
 * and jumps into the recompiled code). */
#include "psx.h"
#include "gpu.h"
#include "spu.h"
#include "gte.h"
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#define mkdir_one(p) mkdir(p, 0755)
#endif

/* create a directory and any missing parents */
static void mkdir_p(const char *path) {
    char p[1200];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if ((*s == '/' || *s == '\\') && s[-1] != ':') {
            char c = *s;
            *s = 0;
            mkdir_one(p);
            *s = c;
        }
    }
    mkdir_one(p);
}

Config g_cfg;
int g_log_level = LOG_INFO;
volatile int g_quit;
static FILE *log_file;

int platform_init(void);
void platform_shutdown(void);
void platform_error_box(const char *title, const char *msg);
bool platform_set_binding(const char *key, const char *val);

void log_msg(int level, const char *fmt, ...) {
    static const char *tag[] = { "ERROR", "WARN", "INFO", "DEBUG", "TRACE" };
    if (level > g_log_level) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%s] %s\n", tag[level], buf);
    if (log_file) { fprintf(log_file, "[%s] f%llu %s\n", tag[level], (unsigned long long)frame_count(), buf); fflush(log_file); }
}

void fatal(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_msg(LOG_ERR, "FATAL: %s", buf);
    memcard_flush();
    platform_error_box("Harvest Moon: Back to Nature", buf);
    exit(1);
}

/* ------------------------------------------------------------------ paths */
static char exe_dir[1024];

static void find_exe_dir(const char *argv0) {
#ifdef _WIN32
    GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
#else
    ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof exe_dir - 1);
    if (n > 0) exe_dir[n] = 0; else snprintf(exe_dir, sizeof exe_dir, "%s", argv0);
#endif
    char *s1 = strrchr(exe_dir, '/'), *s2 = strrchr(exe_dir, '\\');
    char *s = s1 > s2 ? s1 : s2;
    if (s) s[1] = 0; else snprintf(exe_dir, sizeof exe_dir, "./");
}

static bool file_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && !(st.st_mode & S_IFDIR); }


/* look for a .cue (preferred) or .bin in a directory */
static bool scan_dir_for_disc(const char *dir, char *out, size_t n) {
#ifdef _WIN32
    char pat[1200];
    WIN32_FIND_DATAA fd;
    const char *exts[2] = { ".cue", ".bin" };
    for (int k = 0; k < 2; k++) {
        snprintf(pat, sizeof pat, "%s*%s", dir, exts[k]);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            snprintf(out, n, "%s%s", dir, fd.cFileName);
            FindClose(h);
            return true;
        }
    }
    return false;
#else
    const char *exts[2] = { ".cue", ".bin" };
    for (int k = 0; k < 2; k++) {
        DIR *d = opendir(dir[0] ? dir : ".");
        if (!d) return false;
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 4 && !strcasecmp(e->d_name + l - 4, exts[k])) {
                snprintf(out, n, "%s%s", dir, e->d_name);
                closedir(d);
                return true;
            }
        }
        closedir(d);
    }
    return false;
#endif
}

static bool locate_disc(void) {
    if (g_cfg.disc_path[0]) {
        if (file_exists(g_cfg.disc_path)) return true;
        LOGW("configured game data not found: %s - searching the default locations", g_cfg.disc_path);
        g_cfg.disc_path[0] = 0;
    }
    /* a data pack made from the player's disc (tools/makepak.py) */
    const char *pakdirs[] = { "", "data/", "../" };
    for (size_t i = 0; i < sizeof pakdirs / sizeof *pakdirs; i++) {
        char p[1400];
        snprintf(p, sizeof p, "%s%sHarvestMoonBTN.pak", exe_dir, pakdirs[i]);
        if (file_exists(p)) { snprintf(g_cfg.disc_path, sizeof g_cfg.disc_path, "%s", p); return true; }
    }
    /* otherwise the player's disc image ("../../disc/" = the repository's disc folder for
     * development builds in build/<config>/) */
    const char *subdirs[] = { "", "disc/", "Disc/", "../../disc/" };
    for (size_t i = 0; i < sizeof subdirs / sizeof *subdirs; i++) {
        char dir[1200];
        snprintf(dir, sizeof dir, "%s%s", exe_dir, subdirs[i]);
        char p[1400];
        snprintf(p, sizeof p, "%sHarvest Moon - Back to Nature.cue", dir);
        if (file_exists(p)) { snprintf(g_cfg.disc_path, sizeof g_cfg.disc_path, "%s", p); return true; }
        if (scan_dir_for_disc(dir, g_cfg.disc_path, sizeof g_cfg.disc_path)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ config */
static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static void set_option(const char *key, const char *val) {
    if (!strcmp(key, "disc")) snprintf(g_cfg.disc_path, sizeof g_cfg.disc_path, "%s", val);
    else if (!strcmp(key, "savedir")) snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s", val);
    else if (!strcmp(key, "scale")) g_cfg.scale = atoi(val);
    else if (!strcmp(key, "fullscreen")) g_cfg.fullscreen = atoi(val);
    else if (!strcmp(key, "fastcd")) g_cfg.fast_cd = atoi(val);
    else if (!strcmp(key, "smooth")) g_cfg.smooth = atoi(val);
    else if (!strcmp(key, "aspect43")) g_cfg.aspect_4_3 = atoi(val);
    else if (!strcmp(key, "fastforward")) g_cfg.allow_fast_forward = atoi(val);
    else if (!strcmp(key, "loglevel")) g_log_level = atoi(val);
    else if (!platform_set_binding(key, val)) LOGW("unknown config option '%s'", key);
}

static void load_ini(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        set_option(k, v);
    }
    fclose(f);
}

static void usage(void) {
    printf("Harvest Moon: Back to Nature - native port\n"
           "usage: hmbtn [options] [HarvestMoonBTN.pak|disc.cue|disc.bin]\n"
           "  --fullscreen          start in fullscreen (F11 toggles)\n"
           "  --scale N             initial window scale (default 3)\n"
           "  --savedir DIR         memory card directory\n"
           "  --fastcd              faster disc loading (music streaming unaffected)\n"
           "  --smooth              bilinear filtering when scaling\n"
           "  --log N               log level 0..4\n"
           "  --verify-data         check the game data pack against its checksums and exit\n"
           "testing options:\n"
           "  --headless --frames N --unthrottled --shots N --shotdir DIR --input SCRIPT --trace\n");
}

/* ------------------------------------------------------------------ boot */
static void load_exe(void) {
    uint32_t lba, size;
    if (!iso_find_file("SYSTEM.CNF", &lba, &size)) fatal("SYSTEM.CNF not found on disc");
    uint8_t raw[2352];
    cdrom_read_sector_raw(lba, raw);
    char cnf[2049];
    memcpy(cnf, raw + 24, 2048);
    cnf[size < 2048 ? size : 2048] = 0;
    char *boot = strstr(cnf, "BOOT");
    if (!boot) fatal("SYSTEM.CNF has no BOOT line");
    char *p = strchr(boot, ':');
    if (!p) fatal("bad BOOT line");
    p++;
    while (*p == '\\' || *p == '/') p++;
    char path[128];
    int n = 0;
    while (*p && *p != '\r' && *p != '\n' && *p != ' ' && n < 127) path[n++] = *p++;
    path[n] = 0;
    if (!iso_find_file(path, &lba, &size)) fatal("boot file %s not found", path);
    LOGI("boot executable: %s (%u bytes)", path, size);
    uint8_t *exe = malloc(size + 2048);
    for (uint32_t i = 0; i * 2048 < size; i++) {
        cdrom_read_sector_raw(lba + i, raw);
        memcpy(exe + i * 2048, raw + 24, 2048);
    }
    if (memcmp(exe, "PS-X EXE", 8)) fatal("boot file is not a PS-X EXE");
    uint32_t pc, gp, taddr, tsize, saddr, ssize;
    memcpy(&pc, exe + 0x10, 4);
    memcpy(&gp, exe + 0x14, 4);
    memcpy(&taddr, exe + 0x18, 4);
    memcpy(&tsize, exe + 0x1C, 4);
    memcpy(&saddr, exe + 0x30, 4);
    memcpy(&ssize, exe + 0x34, 4);
    if (tsize > size - 0x800) tsize = size - 0x800;
    memcpy(ram_ptr(taddr), exe + 0x800, tsize);
    free(exe);

    /* verify this disc matches the recompiled executable */
    const ModuleDesc *m = &g_modules[0];
    int bad = 0;
    for (int i = 0; i < m->nfuncs; i++)
        if (ram_r32(m->funcs[i].addr) != m->funcs[i].w0 || ram_r32(m->funcs[i].addr + 4) != m->funcs[i].w1) bad++;
    if (bad) fatal("This disc image does not match Harvest Moon: Back to Nature (USA, SLUS-01115).\n"
                   "%d of %d code checks failed.", bad, m->nfuncs);

    g_cpu.r[28] = gp;
    uint32_t sp = saddr ? saddr + ssize : 0x801FFF00u;
    g_cpu.r[29] = sp;
    g_cpu.r[30] = sp;
    g_cpu.r[31] = 0x80000080u;
    LOGI("entry %08X gp %08X sp %08X", pc, gp, sp);
    call_addr(&g_cpu, pc);
    LOGW("game returned from main()");
}

int main(int argc, char **argv) {
    int verify_data = 0;
    find_exe_dir(argv[0]);
    g_cfg.scale = 3;
    g_cfg.aspect_4_3 = 1;
    g_cfg.allow_fast_forward = 1;
    char ini[1200];
    snprintf(ini, sizeof ini, "%shmbtn.ini", exe_dir);
    load_ini(ini);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *nx = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (!strcmp(a, "--fullscreen")) g_cfg.fullscreen = 1;
        else if (!strcmp(a, "--scale")) { g_cfg.scale = atoi(nx); i++; }
        else if (!strcmp(a, "--savedir")) { snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s", nx); i++; }
        else if (!strcmp(a, "--fastcd")) g_cfg.fast_cd = 1;
        else if (!strcmp(a, "--smooth")) g_cfg.smooth = 1;
        else if (!strcmp(a, "--log")) { g_log_level = atoi(nx); i++; }
        else if (!strcmp(a, "--headless")) g_cfg.headless = 1;
        else if (!strcmp(a, "--verify-data")) { verify_data = 1; g_cfg.headless = 1; }
        else if (!strcmp(a, "--frames")) { g_cfg.max_frames = atoi(nx); i++; }
        else if (!strcmp(a, "--unthrottled")) g_cfg.unthrottled = 1;
        else if (!strcmp(a, "--shots")) { g_cfg.screenshot_every = atoi(nx); i++; }
        else if (!strcmp(a, "--shotdir")) { snprintf(g_cfg.screenshot_dir, sizeof g_cfg.screenshot_dir, "%s", nx); i++; }
        else if (!strcmp(a, "--input")) { snprintf(g_cfg.input_script, sizeof g_cfg.input_script, "%s", nx); i++; }
        else if (!strcmp(a, "--dumpram")) {
            for (int k = 0; k < 8; k++) if (!g_cfg.dump_frame[k]) { g_cfg.dump_frame[k] = atoi(nx); break; }
            i++;
        }
        else if (!strcmp(a, "--calltest")) {
            for (int k = 0; k < 4; k++) if (!g_cfg.call_frame[k]) {
                sscanf(nx, "%d:%x:%x", &g_cfg.call_frame[k], &g_cfg.call_addr[k], &g_cfg.call_a0[k]);
                break;
            }
            i++;
        }
        else if (!strcmp(a, "--poke")) {
            for (int k = 0; k < 4; k++) if (!g_cfg.poke_frame[k]) {
                sscanf(nx, "%d:%x:%x", &g_cfg.poke_frame[k], &g_cfg.poke_addr[k], &g_cfg.poke_val[k]);
                break;
            }
            i++;
        }
        else if (!strcmp(a, "--fuzz")) { g_cfg.fuzz_seed = atoi(nx); i++; }
        else if (!strcmp(a, "--fuzzstart")) { g_cfg.fuzz_start = atoi(nx); i++; }
        else if (!strcmp(a, "--wav")) { snprintf(g_cfg.wav_path, sizeof g_cfg.wav_path, "%s", nx); i++; }
        else if (!strcmp(a, "--trace")) g_trace_calls = 1;
        else if (!strcmp(a, "--tracecalls")) g_trace_calls = 2;
        else if (!strcmp(a, "--logfile")) { log_file = fopen(nx, "w"); i++; }
        else if (a[0] != '-') snprintf(g_cfg.disc_path, sizeof g_cfg.disc_path, "%s", a);
        else { usage(); return 1; }
    }
    if (!g_cfg.save_dir[0]) snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%ssaves", exe_dir);
    if (!log_file && !g_cfg.headless) {
        char lp[1200];
        snprintf(lp, sizeof lp, "%sHarvestMoonBTN.log", exe_dir);
        log_file = fopen(lp, "w");
    }
    if (!g_cfg.screenshot_dir[0]) snprintf(g_cfg.screenshot_dir, sizeof g_cfg.screenshot_dir, "%sscreenshots", exe_dir);

    if (platform_init() != 0) return 1;

    if (!locate_disc())
        fatal("Game data not found.\n\nThis port needs your own copy of Harvest Moon: Back to Nature (USA).\n"
              "Put your disc image (.cue + .bin) into the \"disc\" folder next to the executable,\n"
              "set \"disc =\" in hmbtn.ini, or pass the path on the command line.\n"
              "(A HarvestMoonBTN.pak data pack made from your disc also works.)");
    if (!cdrom_open_image(g_cfg.disc_path)) fatal("Cannot open game data:\n%s", g_cfg.disc_path);
    if (verify_data) {
        cdpak_verify_all();
        printf("game data OK: %s\n", g_cfg.disc_path);
        return 0;
    }

    mkdir_p(g_cfg.save_dir);
    {
        /* fall back to a per-user folder when the game directory is not writable */
        char probe[1200];
        snprintf(probe, sizeof probe, "%s/.write_test", g_cfg.save_dir);
        FILE *t = fopen(probe, "wb");
        if (t) { fclose(t); remove(probe); }
        else {
#ifdef _WIN32
            const char *base = getenv("APPDATA");
            if (base) snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s/HarvestMoonBTN", base);
#else
            const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
            if (xdg) snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s/HarvestMoonBTN", xdg);
            else if (home) {
                char d[1100];
                snprintf(d, sizeof d, "%s/.local", home); mkdir_p(d);
                snprintf(d, sizeof d, "%s/.local/share", home); mkdir_p(d);
                snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s/.local/share/HarvestMoonBTN", home);
            }
#endif
            mkdir_p(g_cfg.save_dir);
            LOGW("game folder is read-only; saving memory cards to %s", g_cfg.save_dir);
        }
    }
    char card1[1200];
    snprintf(card1, sizeof card1, "%s/card1.mcd", g_cfg.save_dir);
    memcard_load(0, card1);

    mem_reset();
    gte_reset();
    gpu_init();
    spu_init();
    dma_init();
    cdrom_init();
    sio_init();
    timers_init();
    sched_init();
    bios_init();
    g_cpu.sr = 0x40000401u;   /* COP2 enabled, IRQs enabled (I_MASK is still 0) */
    g_pad_connected[0] = true;

    load_exe();
    memcard_flush();
    platform_shutdown();
    return 0;
}
