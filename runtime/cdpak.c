/* Game data pack (HarvestMoonBTN.pak): the disc's sectors stored as compressed payloads.
 * Each requested sector is rebuilt bit-exactly (sync, MSF header, subheader, EDC, ECC) and
 * every chunk is checked against the CRC32 of the original disc data.  See tools/makepak.py. */
#include "psx.h"

/* ------------------------------------------------------------------ inflate (RFC 1951) */
typedef struct {
    const uint8_t *in; size_t inlen, inpos;
    uint32_t bitbuf; int bitcnt;
    uint8_t *out; size_t outlen, outpos;
} Inflate;

typedef struct { short count[16]; short symbol[288]; } Huff;

static int inf_bits(Inflate *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) return -1;
        val |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static int inf_decode(Inflate *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int b = inf_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int inf_construct(Huff *h, const short *length, int n) {
    short offs[16];
    for (int len = 0; len <= 15; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int inf_codes(Inflate *s, const Huff *lencode, const Huff *distcode) {
    static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                     35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                    4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                     257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                     8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                    9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = inf_decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->outpos >= s->outlen) return -2;
            s->out[s->outpos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -3;
            int e = inf_bits(s, lext[sym]);
            if (e < 0) return -1;
            int len = lbase[sym] + e;
            int dsym = inf_decode(s, distcode);
            if (dsym < 0 || dsym >= 30) return -3;
            e = inf_bits(s, dext[dsym]);
            if (e < 0) return -1;
            size_t dist = (size_t)(dbase[dsym] + e);
            if (dist > s->outpos || s->outpos + (size_t)len > s->outlen) return -4;
            for (int i = 0; i < len; i++, s->outpos++) s->out[s->outpos] = s->out[s->outpos - dist];
        }
    }
}

static int inf_fixed(Inflate *s) {
    static Huff lencode, distcode;
    static int ready;
    if (!ready) {
        short lengths[288];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < 288; sym++) lengths[sym] = 8;
        inf_construct(&lencode, lengths, 288);
        for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
        inf_construct(&distcode, lengths, 30);
        ready = 1;
    }
    return inf_codes(s, &lencode, &distcode);
}

static int inf_dynamic(Inflate *s) {
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[320];
    Huff lencode, distcode;
    int nlen = inf_bits(s, 5), ndist = inf_bits(s, 5), ncode = inf_bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > 286 || ndist > 30) return -3;
    int index;
    for (index = 0; index < ncode; index++) { int b = inf_bits(s, 3); if (b < 0) return -1; lengths[order[index]] = (short)b; }
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (inf_construct(&lencode, lengths, 19) != 0) return -4;
    index = 0;
    while (index < nlen + ndist) {
        int sym = inf_decode(s, &lencode);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[index++] = (short)sym;
        } else {
            int len = 0, rep;
            if (sym == 16) {
                if (index == 0) return -5;
                len = lengths[index - 1];
                rep = inf_bits(s, 2); if (rep < 0) return -1; rep += 3;
            } else if (sym == 17) {
                rep = inf_bits(s, 3); if (rep < 0) return -1; rep += 3;
            } else {
                rep = inf_bits(s, 7); if (rep < 0) return -1; rep += 11;
            }
            if (index + rep > nlen + ndist) return -6;
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -9;
    int err = inf_construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return -7;
    err = inf_construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return -8;
    return inf_codes(s, &lencode, &distcode);
}

static int inf_stored(Inflate *s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen) return -1;
    unsigned len = s->in[s->inpos] | ((unsigned)s->in[s->inpos + 1] << 8);
    s->inpos += 4;
    if (s->inpos + len > s->inlen || s->outpos + len > s->outlen) return -2;
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len;
    s->outpos += len;
    return 0;
}

/* inflate a zlib stream; returns decompressed size or -1 */
static long zlib_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
    if (inlen < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0) return -1;
    Inflate s = { in, inlen, 2, 0, 0, out, outlen, 0 };
    int last, err;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) err = inf_stored(&s);
        else if (type == 1) err = inf_fixed(&s);
        else if (type == 2) err = inf_dynamic(&s);
        else err = -1;
        if (err) return -1;
    } while (!last);
    return (long)s.outpos;
}

/* ------------------------------------------------------------------ CRC32 / EDC / ECC */
static uint32_t crc_table[256], edc_table[256];
static uint8_t ecc_f[256], ecc_b[256];

static void tables_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i, e = i;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
            e = (e >> 1) ^ ((e & 1) ? 0xD8018001u : 0);
        }
        crc_table[i] = c;
        edc_table[i] = e;
        uint32_t x = ((i << 1) ^ ((i & 0x80) ? 0x11D : 0)) & 0xFF;
        ecc_f[i] = (uint8_t)x;
        ecc_b[i ^ x] = (uint8_t)i;
    }
}

static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t edc_buf(const uint8_t *p, size_t n) {
    uint32_t e = 0;
    while (n--) e = (e >> 8) ^ edc_table[(e ^ *p++) & 0xFF];
    return e;
}

static void ecc_block(uint8_t *sec, int major_count, int minor_count, int major_mult, int minor_inc, int dest) {
    int size = major_count * minor_count;
    for (int major = 0; major < major_count; major++) {
        int index = (major >> 1) * major_mult + (major & 1);
        uint8_t a = 0, b = 0;
        for (int minor = 0; minor < minor_count; minor++) {
            uint8_t t = sec[12 + index];
            index += minor_inc;
            if (index >= size) index -= size;
            a ^= t;
            b ^= t;
            a = ecc_f[a];
        }
        a = ecc_b[ecc_f[a] ^ b];
        sec[dest + major] = a;
        sec[dest + major + major_count] = (uint8_t)(a ^ b);
    }
}

static uint8_t bcd(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void build_sector(uint8_t *s, uint32_t lba, uint8_t flags, const uint8_t *sub4, const uint8_t *payload) {
    static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    memset(s, 0, 2352);
    memcpy(s, sync, 12);
    uint32_t a = lba + 150;
    s[12] = bcd(a / 4500);
    s[13] = bcd((a / 75) % 60);
    s[14] = bcd(a % 75);
    s[15] = 2;
    memcpy(s + 16, sub4, 4);
    memcpy(s + 20, sub4, 4);
    if (flags & 1) {            /* Form 2 */
        memcpy(s + 24, payload, 2324);
        uint32_t e = (flags & 2) ? 0 : edc_buf(s + 16, 0x91C);
        memcpy(s + 2348, &e, 4);
    } else {                    /* Form 1 */
        memcpy(s + 24, payload, 2048);
        uint32_t e = edc_buf(s + 16, 0x808);
        memcpy(s + 2072, &e, 4);
        uint8_t hdr[4];
        memcpy(hdr, s + 12, 4);
        memset(s + 12, 0, 4);   /* Mode 2: the header is not covered by the ECC */
        ecc_block(s, 86, 24, 2, 86, 0x81C);
        ecc_block(s, 52, 43, 86, 88, 0x8C8);
        memcpy(s + 12, hdr, 4);
    }
}

/* ------------------------------------------------------------------ pack access */
static FILE *pak;
static uint32_t pak_total, pak_stored, pak_spc, pak_nchunks;
static uint32_t *pak_offs, *pak_crc;
static uint8_t *pak_verified;

#define PAK_DAMAGED "HarvestMoonBTN.pak is damaged (chunk %u).\nRebuild it from your disc image: python build.py --pak"

#define CACHE_N 8
static struct { int32_t chunk; uint32_t age; uint8_t data[16 * 2352]; } cache[CACHE_N];
static uint32_t cache_clock;

bool cdpak_open(const char *path, uint32_t *total_sectors) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[32];
    if (fread(hdr, 1, 32, f) != 32 || memcmp(hdr, "HMBTNPAK", 8)) { fclose(f); return false; }
    uint32_t v[6];
    memcpy(v, hdr + 8, 24);
    if (v[0] != 1 || v[3] == 0 || v[3] > 16) { fclose(f); LOGE("unsupported data pack version"); return false; }
    pak_total = v[1];
    pak_stored = v[2];
    pak_spc = v[3];
    pak_nchunks = v[4];
    pak_offs = malloc((pak_nchunks + 1) * 4);
    pak_crc = malloc(pak_nchunks * 4);
    pak_verified = calloc(pak_nchunks, 1);
    if (fread(pak_offs, 4, pak_nchunks + 1, f) != pak_nchunks + 1 || fread(pak_crc, 4, pak_nchunks, f) != pak_nchunks) {
        fclose(f);
        return false;
    }
    tables_init();
    for (int i = 0; i < CACHE_N; i++) cache[i].chunk = -1;
    pak = f;
    *total_sectors = pak_total;
    LOGI("game data: %s (%u sectors)", path, pak_stored);
    return true;
}

static const uint8_t *load_chunk(uint32_t c) {
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].chunk == (int32_t)c) { cache[i].age = ++cache_clock; return cache[i].data; }
    int slot = 0;
    for (int i = 1; i < CACHE_N; i++) if (cache[i].age < cache[slot].age) slot = i;
    uint32_t clen = pak_offs[c + 1] - pak_offs[c];
    static uint8_t comp[16 * 2352 + 4096], recs[16 * 2400];
    if (clen > sizeof comp) fatal(PAK_DAMAGED, c);
    fseek(pak, (long)pak_offs[c], SEEK_SET);
    if (fread(comp, 1, clen, pak) != clen) fatal("cannot read game data file");
    long n = zlib_inflate(comp, clen, recs, sizeof recs);
    if (n < 0) fatal(PAK_DAMAGED, c);
    uint32_t first = c * pak_spc, count = pak_spc;
    if (first + count > pak_stored) count = pak_stored - first;
    const uint8_t *r = recs;
    for (uint32_t k = 0; k < count; k++) {
        uint8_t flags = r[0];
        build_sector(cache[slot].data + k * 2352, first + k, flags, r + 1, r + 5);
        r += 5 + ((flags & 1) ? 2324 : 2048);
    }
    if (!pak_verified[c]) {
        if (crc32_buf(cache[slot].data, (size_t)count * 2352) != pak_crc[c])
            fatal(PAK_DAMAGED, c);
        pak_verified[c] = 1;
    }
    cache[slot].chunk = (int32_t)c;
    cache[slot].age = ++cache_clock;
    return cache[slot].data;
}

int cdpak_read_sector(uint32_t lba, uint8_t *out) {
    if (!pak) return 0;
    if (lba >= pak_stored) {
        /* beyond the stored data (DUMMY.DAT filler / lead-out): never read by the game */
        static const uint8_t zero_sub[4] = { 0, 0, 8, 0 }, zero[2048];
        build_sector(out, lba, 0, zero_sub, zero);
        return lba < pak_total;
    }
    const uint8_t *d = load_chunk(lba / pak_spc);
    memcpy(out, d + (lba % pak_spc) * 2352, 2352);
    return 1;
}

/* testing: verify every chunk */
int cdpak_verify_all(void) {
    uint8_t s[2352];
    for (uint32_t lba = 0; lba < pak_stored; lba += pak_spc) cdpak_read_sector(lba, s);
    return 1;
}
