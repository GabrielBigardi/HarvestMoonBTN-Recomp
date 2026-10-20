/* CD-ROM controller (host interface + drive command processor) reading the game data pack
 * (HarvestMoonBTN.pak, see cdpak.c) or a disc image (.cue/.bin, Mode2/2352), including XA-ADPCM streaming audio with the hardware's
 * 37800->44100 Hz zigzag interpolation. */
#include "psx.h"
#include <ctype.h>

#define SECTOR_RAW 2352

/* ------------------------------------------------------------------ disc image */
static FILE *disc;
static uint32_t disc_sectors;
static bool use_pak;

static bool path_dir(const char *p, char *dir, size_t n) {
    const char *s1 = strrchr(p, '/'), *s2 = strrchr(p, '\\');
    const char *s = s1 > s2 ? s1 : s2;
    if (!s) { dir[0] = 0; return false; }
    size_t l = (size_t)(s - p + 1);
    if (l >= n) l = n - 1;
    memcpy(dir, p, l);
    dir[l] = 0;
    return true;
}

bool cdrom_open_image(const char *path) {
    char binpath[2048];
    size_t pl = strlen(path);
    if (pl > 4 && !strcasecmp(path + pl - 4, ".pak")) {
        use_pak = cdpak_open(path, &disc_sectors);
        return use_pak;
    }
    snprintf(binpath, sizeof binpath, "%s", path);
    if (pl > 4 && (path[pl - 4] == '.') && tolower((unsigned char)path[pl - 3]) == 'c' &&
        tolower((unsigned char)path[pl - 2]) == 'u' && tolower((unsigned char)path[pl - 1]) == 'e') {
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        char line[1024];
        char dir[1024];
        path_dir(path, dir, sizeof dir);
        binpath[0] = 0;
        while (fgets(line, sizeof line, f)) {
            char *q = strstr(line, "FILE");
            if (!q) continue;
            char *a = strchr(q, '"');
            if (a) {
                char *b = strchr(a + 1, '"');
                if (b) { *b = 0; snprintf(binpath, sizeof binpath, "%s%s", dir, a + 1); break; }
            }
        }
        fclose(f);
        if (!binpath[0]) return false;
    }
    disc = fopen(binpath, "rb");
    if (!disc) { LOGE("cannot open disc image %s", binpath); return false; }
    fseek(disc, 0, SEEK_END);
    long sz = ftell(disc);
    disc_sectors = (uint32_t)(sz / SECTOR_RAW);
    LOGI("disc image %s: %u sectors", binpath, disc_sectors);
    return disc_sectors > 16;
}

int cdrom_read_sector_raw(uint32_t lba, uint8_t *buf) {
    if (use_pak) return cdpak_read_sector(lba, buf);
    if (!disc || lba >= disc_sectors) { memset(buf, 0, SECTOR_RAW); return 0; }
    fseek(disc, (long)lba * SECTOR_RAW, SEEK_SET);
    if (fread(buf, 1, SECTOR_RAW, disc) != SECTOR_RAW) return 0;
    return 1;
}

/* simple ISO9660 path lookup ("\\DIR\\FILE.EXT;1" style or "DIR/FILE") */
bool iso_find_file(const char *path, uint32_t *out_lba, uint32_t *out_size) {
    uint8_t s[SECTOR_RAW];
    if (!cdrom_read_sector_raw(16, s)) return false;
    uint8_t *pvd = s + 24;
    uint32_t dlba, dsize;
    memcpy(&dlba, pvd + 156 + 2, 4);
    memcpy(&dsize, pvd + 156 + 10, 4);
    char comp[64];
    const char *p = path;
    while (*p == '\\' || *p == '/') p++;
    for (;;) {
        int n = 0;
        while (*p && *p != '\\' && *p != '/' && *p != ';' && n < 63) comp[n++] = (char)toupper((unsigned char)*p++);
        comp[n] = 0;
        while (*p == ';' || isdigit((unsigned char)*p)) p++;
        bool last = !*p;
        while (*p == '\\' || *p == '/') p++;
        bool found = false;
        for (uint32_t k = 0; k < (dsize + 2047) / 2048 && !found; k++) {
            cdrom_read_sector_raw(dlba + k, s);
            uint8_t *d = s + 24;
            uint32_t off = 0;
            while (off < 2048) {
                uint8_t len = d[off];
                if (!len) break;
                uint8_t nl = d[off + 32];
                char name[64];
                int m = 0;
                for (int i = 0; i < nl && i < 63; i++) {
                    char ch = (char)d[off + 33 + i];
                    if (ch == ';') break;
                    name[m++] = (char)toupper((unsigned char)ch);
                }
                name[m] = 0;
                if (!strcmp(name, comp)) {
                    memcpy(&dlba, d + off + 2, 4);
                    memcpy(&dsize, d + off + 10, 4);
                    found = true;
                    break;
                }
                off += len;
            }
        }
        if (!found) return false;
        if (last) { *out_lba = dlba; *out_size = dsize; return true; }
    }
}

/* ------------------------------------------------------------------ controller state */
typedef struct { uint8_t type, len; uint8_t data[16]; uint64_t when; } Resp;

static struct {
    uint8_t index;
    uint8_t param[16]; int nparam;
    uint8_t resp[16]; int resp_len, resp_pos;
    uint8_t int_enable, int_flag;
    Resp q[8]; int qn;
    int busy;                 /* BUSYSTS: command written, not yet acknowledged */
    uint8_t cmd;              /* pending command */
    uint8_t cmd_params[16]; int cmd_nparam;

    uint8_t stat;             /* drive status byte */
    uint8_t mode;
    uint8_t filter_file, filter_chan;
    uint32_t setloc_lba; bool setloc_pending;
    uint32_t cur_lba;         /* next sector to read */
    bool reading, seeking, playing;
    int read_after_seek;      /* 1: start reading after seek, 2: just seek (SeekL/P) */

    uint8_t sector[SECTOR_RAW]; bool sector_valid;     /* last delivered data sector */
    uint8_t last_hdr[8];      /* header+subheader of most recent sector read */
    uint8_t pending_sector[SECTOR_RAW]; bool pending_data;   /* sector waiting for INT1 */

    uint8_t data[SECTOR_RAW]; int data_len, data_pos;  /* host data FIFO */

    bool muted, adp_muted;
    uint8_t atv_pend[4], atv[4];

    /* XA */
    int32_t xa_old[2], xa_older[2];
    int16_t ring[2][32]; int ring_pos[2]; int sixstep;
} cd;

/* XA output ring buffer (44100 Hz stereo, before the ATV volume matrix) */
#define XA_BUF 32768
static int16_t xa_buf[XA_BUF][2];
static int xa_rd, xa_wr, xa_count;

static uint8_t bcd2bin(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 15)); }
static uint8_t bin2bcd(uint8_t b) { return (uint8_t)(((b / 10) << 4) | (b % 10)); }

static void lba_to_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f) {
    lba += 150;
    *m = bin2bcd((uint8_t)(lba / (60 * 75)));
    *s = bin2bcd((uint8_t)((lba / 75) % 60));
    *f = bin2bcd((uint8_t)(lba % 75));
}

static uint64_t sector_period(void) {
    /* SystemClock*930h/4/44100Hz per sector at single speed */
    uint64_t p = (uint64_t)PSX_CPU_HZ * 75u / 75u / 75u;  /* 451584 */
    return (cd.mode & 0x80) ? p / 2 : p;
}

static void update_irq(void) {
    if (cd.int_flag & cd.int_enable & 0x1F) irq_raise(IRQ_CDROM);
}

static void schedule_delivery(uint64_t not_before) {
    if (!cd.qn) return;
    uint64_t t = cd.q[0].when;
    if (t < not_before) t = not_before;
    uint64_t cur = sched_when(EV_CDROM);
    if (cd.busy) return;   /* the command event will deliver afterwards */
    if (t < cur || cur == UINT64_MAX) sched_set(EV_CDROM, t);
}

static void push_resp(uint8_t type, const uint8_t *data, int len, uint64_t when) {
    if (cd.qn >= 8) { LOGW("cdrom response queue overflow"); return; }
    /* keep queue ordered by time */
    int i = cd.qn++;
    while (i > 0 && cd.q[i - 1].when > when) { cd.q[i] = cd.q[i - 1]; i--; }
    cd.q[i].type = type;
    cd.q[i].len = (uint8_t)len;
    memcpy(cd.q[i].data, data, (size_t)len);
    cd.q[i].when = when;
}

static void resp_stat(uint8_t type, uint64_t when) {
    uint8_t s = cd.stat;
    push_resp(type, &s, 1, when);
}

static void resp_error(uint8_t err) {
    uint8_t d[2] = { (uint8_t)(cd.stat | 1), err };
    push_resp(5, d, 2, g_cpu.cycles);
}

/* try to deliver the head of the queue (only when no interrupt is pending) */
static void try_deliver(void) {
    if (cd.int_flag & 7) return;
    if (!cd.qn) return;
    if (cd.q[0].when > g_cpu.cycles) { schedule_delivery(0); return; }
    Resp r = cd.q[0];
    memmove(&cd.q[0], &cd.q[1], sizeof(Resp) * (size_t)(cd.qn - 1));
    cd.qn--;
    memcpy(cd.resp, r.data, r.len);
    cd.resp_len = r.len;
    cd.resp_pos = 0;
    cd.int_flag = (uint8_t)((cd.int_flag & ~7) | r.type);
    LOGT("cdrom INT%d resp[0]=%02X len=%d", r.type, r.data[0], r.len);
    update_irq();
}

/* ------------------------------------------------------------------ XA-ADPCM */
static const int16_t zigzag[7][29] = {
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, -0x0002, 0x000A, -0x0022, 0x0041, -0x0054, 0x0034, 0x0009, -0x010A, 0x0400, -0x0A78, 0x234C, 0x6794, -0x1780, 0x0BCD, -0x0623, 0x0350, -0x016D, 0x006B, 0x000A, -0x0010, 0x0011, -0x0008, 0x0003, -0x0001 },
    { 0x0000, 0x0000, 0x0000, -0x0002, 0x0000, 0x0003, -0x0013, 0x003C, -0x004B, 0x00A2, -0x00E3, 0x0132, -0x0043, -0x0267, 0x0C9D, 0x74BB, -0x11B4, 0x09B8, -0x05BF, 0x0372, -0x01A8, 0x00A6, -0x001B, 0x0005, 0x0006, -0x0008, 0x0003, -0x0001, 0x0000 },
    { 0x0000, 0x0000, -0x0001, 0x0003, -0x0002, -0x0005, 0x001F, -0x004A, 0x00B3, -0x0192, 0x02B1, -0x039E, 0x04F8, -0x05A6, 0x7939, -0x05A6, 0x04F8, -0x039E, 0x02B1, -0x0192, 0x00B3, -0x004A, 0x001F, -0x0005, -0x0002, 0x0003, -0x0001, 0x0000, 0x0000 },
    { 0x0000, -0x0001, 0x0003, -0x0008, 0x0006, 0x0005, -0x001B, 0x00A6, -0x01A8, 0x0372, -0x05BF, 0x09B8, -0x11B4, 0x74BB, 0x0C9D, -0x0267, -0x0043, 0x0132, -0x00E3, 0x00A2, -0x004B, 0x003C, -0x0013, 0x0003, 0x0000, -0x0002, 0x0000, 0x0000, 0x0000 },
    { -0x0001, 0x0003, -0x0008, 0x0011, -0x0010, 0x000A, 0x006B, -0x016D, 0x0350, -0x0623, 0x0BCD, -0x1780, 0x6794, 0x234C, -0x0A78, 0x0400, -0x010A, 0x0009, 0x0034, -0x0054, 0x0041, -0x0022, 0x000A, -0x0001, 0x0000, 0x0001, 0x0000, 0x0000, 0x0000 },
    { 0x0002, -0x0008, 0x0010, -0x0023, 0x002B, 0x001A, -0x00EB, 0x027B, -0x0548, 0x0AFA, -0x16FA, 0x53E0, 0x3C07, -0x1249, 0x080E, -0x0347, 0x015B, -0x0044, -0x0017, 0x0046, -0x0023, 0x0011, -0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    { -0x0005, 0x0011, -0x0023, 0x0046, -0x0017, -0x0044, 0x015B, -0x0347, 0x080E, -0x1249, 0x3C07, 0x53E0, -0x16FA, 0x0AFA, -0x0548, 0x027B, -0x00EB, 0x001A, 0x002B, -0x0023, 0x0010, -0x0008, 0x0002, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
};

static int16_t zz_interp(int ch, int t) {
    int32_t sum = 0;
    int p = cd.ring_pos[ch];
    for (int i = 1; i <= 29; i++) sum += (cd.ring[ch][(p - i) & 31] * zigzag[t][i - 1]) / 0x8000;
    if (sum > 0x7FFF) sum = 0x7FFF;
    if (sum < -0x8000) sum = -0x8000;
    return (int16_t)sum;
}

static void xa_out(int16_t l, int16_t r) {
    if (xa_count >= XA_BUF) { xa_rd = (xa_rd + 1) % XA_BUF; xa_count--; }
    xa_buf[xa_wr][0] = l;
    xa_buf[xa_wr][1] = r;
    xa_wr = (xa_wr + 1) % XA_BUF;
    xa_count++;
}

/* push one 37800 Hz sample pair through the zigzag resampler */
static void xa_push37800(int16_t l, int16_t r) {
    cd.ring[0][cd.ring_pos[0]++ & 31] = l;
    cd.ring[1][cd.ring_pos[1]++ & 31] = r;
    if (--cd.sixstep <= 0) {
        cd.sixstep = 6;
        for (int t = 0; t < 7; t++) xa_out(zz_interp(0, t), zz_interp(1, t));
    }
}

static int xa_sectors;
static void xa_decode_sector(const uint8_t *raw) {
    if ((++xa_sectors & 31) == 1) LOGD("XA sector #%d (ci=%02X) buffered=%d", xa_sectors, raw[19], xa_count);
    const uint8_t *sub = raw + 16;
    uint8_t ci = sub[3];
    bool stereo = (ci & 3) == 1;
    bool half = (ci & 0x0C) == 4;     /* 18900 Hz */
    bool eight = (ci & 0x30) == 0x10;
    static const int pos[4] = { 0, 60, 115, 98 }, neg[4] = { 0, 0, -52, -55 };
    int16_t outL[4032], outR[4032];
    int nL = 0, nR = 0;
    const uint8_t *src = raw + 24;
    for (int i = 0; i < 18; i++, src += 128) {
        int nblk = eight ? 4 : 8;
        for (int blk = 0; blk < nblk; blk++) {
            uint8_t hdr = src[4 + blk];
            int shift = hdr & 0x0F;
            if (shift > 12) shift = 9;
            int filter = (hdr >> 4) & 3;
            int ch = stereo ? (blk & 1) : 0;
            int32_t old = cd.xa_old[ch], older = cd.xa_older[ch];
            for (int j = 0; j < 28; j++) {
                int32_t t;
                if (eight) {
                    t = (int8_t)src[16 + blk + j * 4];
                    t = (t << 8) >> shift;
                } else {
                    int nib = (src[16 + (blk >> 1) + j * 4] >> ((blk & 1) * 4)) & 15;
                    t = (int16_t)(nib << 12) >> shift;
                }
                int32_t s = t + ((old * pos[filter] + older * neg[filter] + 32) >> 6);
                if (s > 0x7FFF) s = 0x7FFF;
                if (s < -0x8000) s = -0x8000;
                older = old;
                old = s;
                if (ch == 0) outL[nL++] = (int16_t)s; else outR[nR++] = (int16_t)s;
            }
            cd.xa_old[ch] = old;
            cd.xa_older[ch] = older;
        }
    }
    int n = stereo ? (nL < nR ? nL : nR) : nL;
    for (int k = 0; k < n; k++) {
        int16_t l = outL[k], r = stereo ? outR[k] : outL[k];
        xa_push37800(l, r);
        if (half) xa_push37800(l, r);
    }
}

int cdrom_audio_sample(int16_t *l, int16_t *r) {
    if (!xa_count) { *l = *r = 0; return 0; }
    int32_t L = xa_buf[xa_rd][0], Rr = xa_buf[xa_rd][1];
    xa_rd = (xa_rd + 1) % XA_BUF;
    xa_count--;
    if (cd.muted || cd.adp_muted) { *l = *r = 0; return 1; }
    int32_t ol = (L * cd.atv[0] + Rr * cd.atv[3]) >> 7;
    int32_t orr = (Rr * cd.atv[2] + L * cd.atv[1]) >> 7;
    if (ol > 0x7FFF) ol = 0x7FFF;
    if (ol < -0x8000) ol = -0x8000;
    if (orr > 0x7FFF) orr = 0x7FFF;
    if (orr < -0x8000) orr = -0x8000;
    *l = (int16_t)ol;
    *r = (int16_t)orr;
    return 1;
}

/* ------------------------------------------------------------------ reading */
static void stop_reading(void) {
    cd.reading = false;
    cd.seeking = false;
    cd.playing = false;
    cd.stat &= (uint8_t)~0xE0;
    sched_cancel(EV_CDROM_READ);
}

static uint64_t seek_time(uint32_t from, uint32_t to) {
    uint32_t d = from > to ? from - to : to - from;
    uint64_t t = 20000 + (uint64_t)d * 8;   /* rough model; long seeks ~0.1s */
    if (t > 3000000) t = 3000000;
    if (g_cfg.fast_cd) t /= 4;
    return t;
}

static void start_seek(int then) {
    uint32_t target = cd.setloc_pending ? cd.setloc_lba : cd.cur_lba;
    uint64_t t = seek_time(cd.cur_lba, target);
    cd.cur_lba = target;
    cd.setloc_pending = false;
    cd.seeking = true;
    cd.reading = false;
    cd.read_after_seek = then;
    cd.stat = (uint8_t)((cd.stat & ~0xE0) | 0x42);   /* seeking + motor on */
    sched_set(EV_CDROM_READ, g_cpu.cycles + t);
}

static void deliver_data_sector(void) {
    memcpy(cd.sector, cd.pending_sector, SECTOR_RAW);
    cd.sector_valid = true;
    cd.pending_data = false;
    resp_stat(1, g_cpu.cycles);
    try_deliver();
}

void cdrom_read_event(void) {
    if (cd.seeking) {
        cd.seeking = false;
        if (cd.read_after_seek == 1) {
            cd.reading = true;
            cd.stat = (uint8_t)((cd.stat & ~0xE0) | 0x22);
            sched_set(EV_CDROM_READ, g_cpu.cycles + sector_period());
        } else {
            cd.stat = (uint8_t)((cd.stat & ~0xE0) | 0x02);
            resp_stat(2, g_cpu.cycles);
            try_deliver();
        }
        return;
    }
    if (!cd.reading) return;
    uint8_t raw[SECTOR_RAW];
    cdrom_read_sector_raw(cd.cur_lba, raw);
    memcpy(cd.last_hdr, raw + 12, 8);
    cd.cur_lba++;
    uint64_t period = sector_period();
    const uint8_t *sub = raw + 16;
    bool is_audio_rt = (sub[2] & 0x44) == 0x44;
    bool filt = cd.mode & 0x08;
    bool match = sub[0] == cd.filter_file && sub[1] == cd.filter_chan;
    bool xa_done = false;
    if ((cd.mode & 0x40) && raw[15] == 2 && is_audio_rt && (!filt || match)) {
        xa_decode_sector(raw);
        xa_done = true;
        if (sub[2] & 0x80) {   /* EOF: the controller keeps reading; games watch this via data/GetlocL */
            LOGD("cdrom XA EOF at lba %u", cd.cur_lba - 1);
        }
    }
    if (!xa_done && !(filt && is_audio_rt)) {
        memcpy(cd.pending_sector, raw, SECTOR_RAW);
        cd.pending_data = true;
        if (!(cd.int_flag & 7) && !cd.qn) deliver_data_sector();
    }
    if (g_cfg.fast_cd && !(cd.mode & 0x40)) period /= 4;
    sched_set(EV_CDROM_READ, g_cpu.cycles + period);
}

/* ------------------------------------------------------------------ commands */
static void exec_command(void) {
    uint8_t c = cd.cmd;
    uint8_t *p = cd.cmd_params;
    int np = cd.cmd_nparam;
    uint64_t now = g_cpu.cycles;
    LOGD("cdrom cmd %02X (%d params: %02X %02X %02X) lba=%u", c, np, p[0], p[1], p[2], cd.cur_lba);
    switch (c) {
    case 0x01: /* Getstat */
        resp_stat(3, now);
        cd.stat &= (uint8_t)~0x10;
        break;
    case 0x02: /* Setloc */
        if (np < 3) { resp_error(0x20); break; }
        {
            uint32_t m = bcd2bin(p[0]), s = bcd2bin(p[1]), f = bcd2bin(p[2]);
            uint32_t l = (m * 60 + s) * 75 + f;
            cd.setloc_lba = l >= 150 ? l - 150 : 0;
            cd.setloc_pending = true;
        }
        resp_stat(3, now);
        break;
    case 0x03: /* Play (no CD-DA tracks on this disc: behave like a seek) */
        resp_stat(3, now);
        stop_reading();
        start_seek(2);
        break;
    case 0x06: case 0x1B: /* ReadN / ReadS */
        resp_stat(3, now);
        if (cd.setloc_pending || !cd.reading) {
            sched_cancel(EV_CDROM_READ);
            if (cd.setloc_pending) start_seek(1);
            else {
                cd.reading = true;
                cd.stat = (uint8_t)((cd.stat & ~0xE0) | 0x22);
                sched_set(EV_CDROM_READ, now + sector_period());
            }
        }
        break;
    case 0x07: /* MotorOn */
        resp_stat(3, now);
        cd.stat |= 2;
        resp_stat(2, now + 0x10000);
        break;
    case 0x08: /* Stop */
        resp_stat(3, now);
        stop_reading();
        cd.stat &= (uint8_t)~2;
        resp_stat(2, now + ((cd.mode & 0x80) ? 0x18a6076 : 0x0d38aca) / 8);
        break;
    case 0x09: { /* Pause */
        resp_stat(3, now);
        bool was = cd.reading || cd.playing || cd.seeking;
        stop_reading();
        cd.pending_data = false;
        resp_stat(2, now + (was ? ((cd.mode & 0x80) ? 0x10bd93 : 0x21181c) : 0x1df2));
        break;
    }
    case 0x0A: /* Init */
        resp_stat(3, now);
        stop_reading();
        cd.mode = 0x20;
        cd.stat = 0x02;
        resp_stat(2, now + 0x13cce);
        break;
    case 0x0B: cd.muted = true;  resp_stat(3, now); break;
    case 0x0C: cd.muted = false; resp_stat(3, now); break;
    case 0x0D: /* Setfilter */
        cd.filter_file = p[0];
        cd.filter_chan = p[1];
        resp_stat(3, now);
        break;
    case 0x0E: /* Setmode */
        cd.mode = p[0];
        resp_stat(3, now);
        break;
    case 0x0F: { /* Getparam */
        uint8_t d[5] = { cd.stat, cd.mode, 0, cd.filter_file, cd.filter_chan };
        push_resp(3, d, 5, now);
        break;
    }
    case 0x10: /* GetlocL */
        push_resp(3, cd.last_hdr, 8, now);
        break;
    case 0x11: { /* GetlocP */
        uint32_t l = cd.cur_lba ? cd.cur_lba - 1 : 0;
        uint8_t m, s, f, rm, rs, rf;
        lba_to_msf(l, &m, &s, &f);
        uint32_t rel = l;   /* track 1 starts at LBA 0 (00:02:00) */
        rm = bin2bcd((uint8_t)(rel / (60 * 75)));
        rs = bin2bcd((uint8_t)((rel / 75) % 60));
        rf = bin2bcd((uint8_t)(rel % 75));
        uint8_t d[8] = { 1, 1, rm, rs, rf, m, s, f };
        push_resp(3, d, 8, now);
        break;
    }
    case 0x12: /* SetSession */
        resp_stat(3, now);
        resp_stat(2, now + 0x80000);
        break;
    case 0x13: { /* GetTN */
        uint8_t d[3] = { cd.stat, 1, 1 };
        push_resp(3, d, 3, now);
        break;
    }
    case 0x14: { /* GetTD */
        uint8_t m, s, f;
        uint8_t t = np ? bcd2bin(p[0]) : 0;
        if (t == 0) lba_to_msf(disc_sectors, &m, &s, &f);
        else if (t == 1) { m = 0; s = 2; f = 0; }
        else { resp_error(0x10); break; }
        uint8_t d[3] = { cd.stat, m, s };
        (void)f;
        push_resp(3, d, 3, now);
        break;
    }
    case 0x15: case 0x16: /* SeekL / SeekP */
        resp_stat(3, now);
        stop_reading();
        start_seek(2);
        break;
    case 0x19: { /* Test */
        uint8_t sub = np ? p[0] : 0;
        if (sub == 0x20) {
            uint8_t d[4] = { 0x97, 0x01, 0x10, 0xC2 };
            push_resp(3, d, 4, now);
        } else if (sub == 0x04 || sub == 0x05) {
            uint8_t d[3] = { cd.stat, 0, 0 };
            push_resp(3, d, sub == 0x05 ? 3 : 1, now);
        } else {
            resp_stat(3, now);
        }
        break;
    }
    case 0x1A: { /* GetID */
        resp_stat(3, now);
        uint8_t d[8] = { 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A' };
        push_resp(2, d, 8, now + 0x4a00);
        break;
    }
    case 0x1C: /* Reset */
        resp_stat(3, now);
        stop_reading();
        cd.mode = 0;
        break;
    case 0x1E: /* ReadTOC */
        resp_stat(3, now);
        resp_stat(2, now + 0x13cce * 4);
        break;
    default:
        LOGW("cdrom: unsupported command %02X", c);
        resp_error(0x40);
        break;
    }
}

void cdrom_event(void) {
    if (cd.busy) {
        cd.busy = 0;
        exec_command();
    }
    try_deliver();
    if (!(cd.int_flag & 7) && cd.qn) schedule_delivery(g_cpu.cycles + 1000);
    if (!(cd.int_flag & 7) && !cd.qn && cd.pending_data) deliver_data_sector();
}

/* ------------------------------------------------------------------ host interface */
void cdrom_init(void) {
    memset(&cd, 0, sizeof cd);
    cd.stat = 0x02;
    cd.int_enable = 0x1F;   /* left enabled by the BIOS boot code; libcd relies on it */
    cd.atv[0] = cd.atv[2] = 0x80;
    cd.atv_pend[0] = cd.atv_pend[2] = 0x80;
    cd.sixstep = 6;
    xa_rd = xa_wr = xa_count = 0;
}

uint8_t cdrom_read8(uint32_t off) {
    switch (off) {
    case 0: {
        uint8_t v = cd.index & 3;
        if (xa_count) v |= 0x04;
        if (cd.nparam == 0) v |= 0x08;
        if (cd.nparam < 16) v |= 0x10;
        if (cd.resp_pos < cd.resp_len) v |= 0x20;
        if (cd.data_pos < cd.data_len) v |= 0x40;
        if (cd.busy) v |= 0x80;
        return v;
    }
    case 1: {
        uint8_t v = 0;
        if (cd.resp_pos < cd.resp_len) v = cd.resp[cd.resp_pos];
        cd.resp_pos++;
        if (cd.resp_pos >= 16) cd.resp_pos = 0;
        if (cd.resp_pos >= cd.resp_len && cd.resp_pos != 0) { /* reading past end: zeros */ }
        return v;
    }
    case 2:
        if (cd.data_pos < cd.data_len) return cd.data[cd.data_pos++];
        if (cd.data_len) return cd.data[cd.data_len - 8 > 0 ? cd.data_len - 8 : 0];
        return 0;
    case 3:
        if (cd.index & 1) return (uint8_t)(cd.int_flag | 0xE0);
        return (uint8_t)(cd.int_enable | 0xE0);
    }
    return 0;
}

static void load_data_fifo(void) {
    if (!cd.sector_valid) { cd.data_len = cd.data_pos = 0; return; }
    if (cd.mode & 0x20) {
        memcpy(cd.data, cd.sector + 12, 0x924);
        cd.data_len = 0x924;
    } else {
        memcpy(cd.data, cd.sector + 24, 0x800);
        cd.data_len = 0x800;
    }
    cd.data_pos = 0;
}

void cdrom_write8(uint32_t off, uint8_t v) {
    LOGT("cdrom write %u.%u = %02X", off, cd.index, v);
    if (off == 0) { cd.index = v & 3; return; }
    switch ((off << 2) | cd.index) {
    case (1 << 2) | 0: /* command */
        if (cd.busy) LOGW("cdrom: command %02X while busy with %02X", v, cd.cmd);
        cd.cmd = v;
        memcpy(cd.cmd_params, cd.param, 16);
        cd.cmd_nparam = cd.nparam;
        cd.nparam = 0;
        cd.busy = 1;
        sched_set(EV_CDROM, g_cpu.cycles + (v == 0x0A ? 0x13cce : 0xc4e1) / (g_cfg.fast_cd ? 4 : 1));
        break;
    case (2 << 2) | 0: /* parameter */
        if (cd.nparam < 16) cd.param[cd.nparam++] = v;
        break;
    case (3 << 2) | 0: /* request register */
        if (v & 0x80) { if (cd.data_pos >= cd.data_len) load_data_fifo(); }
        else { cd.data_len = cd.data_pos = 0; }
        break;
    case (1 << 2) | 1: break;                      /* sound map data out */
    case (2 << 2) | 1: cd.int_enable = v & 0x1F; update_irq(); break;
    case (3 << 2) | 1: /* interrupt flag acknowledge */
        cd.int_flag &= (uint8_t)~(v & 0x1F);
        if (v & 0x40) cd.nparam = 0;
        if (!(cd.int_flag & 7)) {
            cd.resp_len = cd.resp_pos = 0;
            if (cd.qn) schedule_delivery(g_cpu.cycles + 2000);
            else if (cd.pending_data) sched_set(EV_CDROM, g_cpu.cycles + 2000);
        }
        break;
    case (1 << 2) | 2: break;                      /* sound map coding info */
    case (2 << 2) | 2: cd.atv_pend[0] = v; break;  /* L->L */
    case (3 << 2) | 2: cd.atv_pend[1] = v; break;  /* L->R */
    case (1 << 2) | 3: cd.atv_pend[2] = v; break;  /* R->R */
    case (2 << 2) | 3: cd.atv_pend[3] = v; break;  /* R->L */
    case (3 << 2) | 3:
        cd.adp_muted = v & 1;
        if (v & 0x20) memcpy(cd.atv, cd.atv_pend, 4);
        break;
    }
}

void cdrom_dma_read(uint32_t *dst, int words) {
    for (int i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) w |= (uint32_t)cdrom_read8(2) << (b * 8);
        dst[i] = w;
    }
}
