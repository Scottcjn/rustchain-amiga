/*
 * rustchain_amiga_miner.c - RustChain attestation miner for classic AmigaOS (m68k)
 *
 * Single file. Detects CPU via ExecBase AttnFlags, reads Kickstart version,
 * hashes the 512KB ROM at 0xF80000 with an in-file SHA-1, then POSTs an
 * attestation JSON to the RustChain node over bsdsocket.library.
 *
 * Flags:
 *   --test-only        print hardware detection, no network
 *   --dry-run          print the exact JSON payload, no network
 *   --once             attest one time and exit
 *   --node host:port   node address (default 50.28.86.131:8088)
 *   --wallet id        wallet / miner id (default amiga-fsuae-scott)
 *
 * Build (cross): m68k-amigaos-gcc -noixemul -m68000 -O2 -o rustchain_amiga rustchain_amiga_miner.c
 * Host test:     gcc -DHOST_TEST -O2 -o host_test rustchain_amiga_miner.c && ./host_test
 *
 * Protocol matches rustchain_mac_universal_miner_v2.2.2.py (legacy plain-HTTP path):
 * POST /attest/challenge {} -> nonce, then POST /attest/submit with
 * miner/miner_id/nonce/report/device/signals/fingerprint. No Ed25519.
 * The fingerprint block is honest: clock_drift carries raw EClock samples,
 * anti_emulation self-reports UAE when uae.resource is present.
 * Emulated hardware is expected to be flagged server side. That is fine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef HOST_TEST
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct Device *TimerBase = NULL;
#include <proto/timer.h>

struct Library *SocketBase = NULL;

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/bsdsocket.h>

extern struct ExecBase *SysBase;
#endif

#define MINER_VERSION "1.0"
#define DEFAULT_NODE_HOST "50.28.86.131"
#define DEFAULT_NODE_PORT 8088
#define DEFAULT_WALLET "amiga-fsuae-scott"
#define ROM_BASE 0xF80000UL
#define ROM_SIZE 524288UL
#define POLL_SECONDS (30 * 60)

#define PAYLOAD_MAX 4096
#define HTTP_MAX 5120
#define RESP_MAX 8192

/* ------------------------------------------------------------------ */
/* SHA-1                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long h[5];
    unsigned long len_lo;
    unsigned long len_hi;
    unsigned char buf[64];
    int buf_used;
} sha1_ctx;

static unsigned long sha1_rol(unsigned long v, int n)
{
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFFUL;
}

static void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301UL;
    c->h[1] = 0xEFCDAB89UL;
    c->h[2] = 0x98BADCFEUL;
    c->h[3] = 0x10325476UL;
    c->h[4] = 0xC3D2E1F0UL;
    c->len_lo = 0;
    c->len_hi = 0;
    c->buf_used = 0;
}

static void sha1_block(sha1_ctx *c, const unsigned char *p)
{
    unsigned long w[80];
    unsigned long a, b, d, e, f, k, t;
    unsigned long cc;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned long)p[i * 4] << 24) |
               ((unsigned long)p[i * 4 + 1] << 16) |
               ((unsigned long)p[i * 4 + 2] << 8) |
               ((unsigned long)p[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++)
        w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];

    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & cc) | ((~b) & d);          k = 0x5A827999UL; }
        else if (i < 40) { f = b ^ cc ^ d;                     k = 0x6ED9EBA1UL; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);  k = 0x8F1BBCDCUL; }
        else             { f = b ^ cc ^ d;                     k = 0xCA62C1D6UL; }
        t = (sha1_rol(a, 5) + f + e + k + w[i]) & 0xFFFFFFFFUL;
        e = d; d = cc; cc = sha1_rol(b, 30); b = a; a = t;
    }

    c->h[0] = (c->h[0] + a) & 0xFFFFFFFFUL;
    c->h[1] = (c->h[1] + b) & 0xFFFFFFFFUL;
    c->h[2] = (c->h[2] + cc) & 0xFFFFFFFFUL;
    c->h[3] = (c->h[3] + d) & 0xFFFFFFFFUL;
    c->h[4] = (c->h[4] + e) & 0xFFFFFFFFUL;
}

static void sha1_update(sha1_ctx *c, const unsigned char *p, unsigned long n)
{
    unsigned long old = c->len_lo;
    c->len_lo = (c->len_lo + (n << 3)) & 0xFFFFFFFFUL;
    if (c->len_lo < old)
        c->len_hi++;
    c->len_hi += (n >> 29);

    while (n > 0) {
        unsigned long take = 64 - c->buf_used;
        if (take > n) take = n;
        memcpy(c->buf + c->buf_used, p, take);
        c->buf_used += (int)take;
        p += take;
        n -= take;
        if (c->buf_used == 64) {
            sha1_block(c, c->buf);
            c->buf_used = 0;
        }
    }
}

static void sha1_final(sha1_ctx *c, unsigned char out[20])
{
    unsigned char pad = 0x80;
    unsigned char zero = 0;
    unsigned char lenb[8];
    unsigned long lo = c->len_lo, hi = c->len_hi;
    int i;

    sha1_update(c, &pad, 1);
    while (c->buf_used != 56)
        sha1_update(c, &zero, 1);

    lenb[0] = (unsigned char)(hi >> 24); lenb[1] = (unsigned char)(hi >> 16);
    lenb[2] = (unsigned char)(hi >> 8);  lenb[3] = (unsigned char)(hi);
    lenb[4] = (unsigned char)(lo >> 24); lenb[5] = (unsigned char)(lo >> 16);
    lenb[6] = (unsigned char)(lo >> 8);  lenb[7] = (unsigned char)(lo);
    /* length goes in raw, do not recount it */
    memcpy(c->buf + 56, lenb, 8);
    sha1_block(c, c->buf);

    for (i = 0; i < 5; i++) {
        out[i * 4]     = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(c->h[i]);
    }
}

static void sha1_hex(const unsigned char *data, unsigned long n, char out[41])
{
    sha1_ctx c;
    unsigned char dig[20];
    int i;
    static const char hexd[] = "0123456789abcdef";

    sha1_init(&c);
    sha1_update(&c, data, n);
    sha1_final(&c, dig);
    for (i = 0; i < 20; i++) {
        out[i * 2] = hexd[dig[i] >> 4];
        out[i * 2 + 1] = hexd[dig[i] & 0x0F];
    }
    out[40] = '\0';
}

/* ------------------------------------------------------------------ */
/* Hardware info                                                      */
/* ------------------------------------------------------------------ */

struct hwinfo {
    char arch[16];          /* "68030" */
    char cpu[32];           /* "MC68030" */
    char fpu[16];           /* "68882" or "none" */
    char kick_version[16];  /* "47.96" */
    char rom_hash[41];      /* sha1 hex of 512KB ROM */
    unsigned long rom_size;
    unsigned long rom_checksum; /* unsigned byte sum, matches tools/rom/checksums.json */
    unsigned long attn_flags;   /* raw ExecBase AttnFlags word */
    unsigned long mem_kb;   /* free memory, KB */
};

#define ENT_SAMPLES 16

struct entropy_info {
    unsigned long samples_ns[ENT_SAMPLES];
    int sample_count;
    int timer_ok;
    unsigned long long mean_ns;
    unsigned long long variance_ns;
    unsigned long long stdev_ns;
    unsigned long long drift_stdev_ns;
    unsigned long min_ns;
    unsigned long max_ns;
    unsigned long long cv_ppm;  /* coefficient of variation * 1e6.
                                   64-bit: a wrong stdev once made the
                                   quotient wrap the 32-bit ulong on m68k */
};

struct emu_info {
    int uae_resource;   /* uae.resource present */
    int uae_id_string;  /* "UAE" in bsdsocket.library id string */
    int passed;         /* no emulation indicators found */
};

/* unsigned 64-bit to decimal, printf %lld is not a safe bet in libnix */
static char *u64s(unsigned long long v, char *buf)
{
    char tmp[24];
    int i = 0, o = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    while (v > 0) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (i > 0) buf[o++] = tmp[--i];
    buf[o] = '\0';
    return buf;
}

/*
 * Shift-free integer square root. The first version used 64-bit shifts
 * (1ULL<<62, r>>1) and bebbo gcc at -m68000 miscompiled them: on target
 * stdev came back exactly 2^34 while the x86 host test passed. The live
 * run proved 64-bit add, sub, compare and divide all work on target
 * (variance and its printed decimal were correct), so binary search on
 * those ops only. mid <= x/mid avoids the mid*mid overflow. ~32 divides.
 */
static unsigned long long isqrt64(unsigned long long x)
{
    unsigned long long lo = 1, hi = 4294967295ULL, mid;

    if (x == 0) return 0;
    if (x < hi) hi = x;
    while (lo < hi) {
        mid = lo + (hi - lo + 1) / 2;
        if (mid <= x / mid) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* all accumulation in 64-bit. The old 32-bit squaring overflowed and
   reported variance_ns = -942169152 on real EClock deltas near 1ms. */
static void entropy_compute_stats(struct entropy_info *e)
{
    int n = e->sample_count, i;
    unsigned long long sum = 0, var = 0;
    long long dsum = 0, dvar = 0, dmean;

    e->min_ns = e->samples_ns[0];
    e->max_ns = e->samples_ns[0];
    for (i = 0; i < n; i++) {
        sum += e->samples_ns[i];
        if (e->samples_ns[i] < e->min_ns) e->min_ns = e->samples_ns[i];
        if (e->samples_ns[i] > e->max_ns) e->max_ns = e->samples_ns[i];
    }
    e->mean_ns = sum / (unsigned long long)n;

    for (i = 0; i < n; i++) {
        long long d = (long long)e->samples_ns[i] - (long long)e->mean_ns;
        var += (unsigned long long)(d * d);
    }
    e->variance_ns = var / (unsigned long long)n;
    e->stdev_ns = isqrt64(e->variance_ns);
    e->cv_ppm = e->mean_ns ? (e->stdev_ns * 1000000ULL) / e->mean_ns : 0;

    /* stdev of successive differences, same idea as reference drift_stdev */
    if (n > 1) {
        for (i = 1; i < n; i++)
            dsum += (long long)e->samples_ns[i] - (long long)e->samples_ns[i - 1];
        dmean = dsum / (n - 1);
        for (i = 1; i < n; i++) {
            long long d = ((long long)e->samples_ns[i] -
                           (long long)e->samples_ns[i - 1]) - dmean;
            dvar += d * d;
        }
        e->drift_stdev_ns = isqrt64((unsigned long long)(dvar / (n - 1)));
    } else {
        e->drift_stdev_ns = 0;
    }
}

/* honest local mirror of the server thresholds:
   cv >= 0.0001 and nonzero drift, and the timer actually worked */
static int clock_drift_passed(const struct entropy_info *e)
{
    return e->timer_ok && e->cv_ppm >= 100 && e->drift_stdev_ns > 0;
}

#ifndef HOST_TEST

static void detect_cpu(struct hwinfo *hw)
{
    unsigned short af = SysBase->AttnFlags;
    const char *arch;

    hw->attn_flags = (unsigned long)af;

    if (af & AFF_68060)      arch = "68060";
    else if (af & AFF_68040) arch = "68040";
    else if (af & AFF_68030) arch = "68030";
    else if (af & AFF_68020) arch = "68020";
    else if (af & AFF_68010) arch = "68010";
    else                     arch = "68000";

    strcpy(hw->arch, arch);
    sprintf(hw->cpu, "MC%s", arch);

    if (af & AFF_68060)      strcpy(hw->fpu, "060-fpu");
    else if (af & AFF_FPU40) strcpy(hw->fpu, "040-fpu");
    else if (af & AFF_68882) strcpy(hw->fpu, "68882");
    else if (af & AFF_68881) strcpy(hw->fpu, "68881");
    else                     strcpy(hw->fpu, "none");
}

static void detect_kick(struct hwinfo *hw)
{
    sprintf(hw->kick_version, "%d.%d",
            (int)SysBase->LibNode.lib_Version,
            (int)SysBase->SoftVer);
}

static void detect_rom(struct hwinfo *hw)
{
    const unsigned char *rom = (const unsigned char *)ROM_BASE;
    unsigned long sum = 0, i;

    hw->rom_size = ROM_SIZE;
    sha1_hex(rom, ROM_SIZE, hw->rom_hash);
    /* same algorithm as tools/amiga/amiga_fingerprint.asm intends:
       unsigned byte sum of the 512KB ROM window */
    for (i = 0; i < ROM_SIZE; i++)
        sum += rom[i];
    hw->rom_checksum = sum;
}

static void detect_mem(struct hwinfo *hw)
{
    hw->mem_kb = AvailMem(MEMF_ANY) / 1024;
}

/* EClock timing, ~709kHz on PAL. Falls back to zeros if timer.device fails. */
static struct MsgPort *timer_port = NULL;
static struct timerequest *timer_req = NULL;

static int timer_open(void)
{
    if (TimerBase) return 1;
    timer_port = CreateMsgPort();
    if (!timer_port) return 0;
    timer_req = (struct timerequest *)CreateIORequest(timer_port,
                    sizeof(struct timerequest));
    if (!timer_req) {
        DeleteMsgPort(timer_port);
        timer_port = NULL;
        return 0;
    }
    if (OpenDevice((STRPTR)"timer.device", UNIT_ECLOCK,
                   (struct IORequest *)timer_req, 0) != 0) {
        DeleteIORequest((struct IORequest *)timer_req);
        DeleteMsgPort(timer_port);
        timer_req = NULL;
        timer_port = NULL;
        return 0;
    }
    TimerBase = timer_req->tr_node.io_Device;
    return 1;
}

static void timer_close(void)
{
    if (TimerBase) {
        CloseDevice((struct IORequest *)timer_req);
        DeleteIORequest((struct IORequest *)timer_req);
        DeleteMsgPort(timer_port);
        TimerBase = NULL;
        timer_req = NULL;
        timer_port = NULL;
    }
}

static void collect_entropy(struct entropy_info *e)
{
    int i, j;
    volatile unsigned long acc = 0;

    memset(e, 0, sizeof(*e));
    e->sample_count = ENT_SAMPLES;
    e->timer_ok = timer_open();

    for (i = 0; i < ENT_SAMPLES; i++) {
        if (e->timer_ok) {
            struct EClockVal ev0, ev1;
            unsigned long freq;
            freq = ReadEClock(&ev0);
            for (j = 0; j < 20000; j++)
                acc ^= (unsigned long)(j * 19);
            ReadEClock(&ev1);
            if (freq == 0) freq = 709379;
            /* ticks to ns, one eclock tick is about 1410 ns */
            e->samples_ns[i] = (ev1.ev_lo - ev0.ev_lo) * (1000000000UL / freq);
        } else {
            e->samples_ns[i] = 0;
        }
    }

    entropy_compute_stats(e);
}

/* real UAE self-detection, evidence for the anti_emulation check.
   uae.resource exists on every UAE flavor including FS-UAE. */
static void detect_emulation(struct emu_info *em)
{
    struct Library *sb;

    memset(em, 0, sizeof(*em));

    if (OpenResource((STRPTR)"uae.resource") != NULL)
        em->uae_resource = 1;

    sb = SocketBase ? SocketBase : OpenLibrary((STRPTR)"bsdsocket.library", 0);
    if (sb) {
        const char *id = (const char *)((struct Library *)sb)->lib_IdString;
        if (id && (strstr(id, "UAE") || strstr(id, "uae")))
            em->uae_id_string = 1;
        if (sb != SocketBase)
            CloseLibrary(sb);
    }

    em->passed = !em->uae_resource && !em->uae_id_string;
}

static void miner_sleep(long seconds)
{
    Delay(seconds * 50);
}

#else /* HOST_TEST stubs */

static unsigned char host_rom[ROM_SIZE];

static void detect_cpu(struct hwinfo *hw)
{
    strcpy(hw->arch, "68030");
    strcpy(hw->cpu, "MC68030");
    strcpy(hw->fpu, "68882");
    /* AFF_68010|AFF_68020|AFF_68030|AFF_68881|AFF_68882 */
    hw->attn_flags = 0x37;
}

static void detect_kick(struct hwinfo *hw)
{
    strcpy(hw->kick_version, "47.96");
}

static void detect_rom(struct hwinfo *hw)
{
    unsigned long i, sum = 0;
    for (i = 0; i < ROM_SIZE; i++) {
        host_rom[i] = (unsigned char)((i * 7 + 13) & 0xFF);
        sum += host_rom[i];
    }
    hw->rom_size = ROM_SIZE;
    hw->rom_checksum = sum;
    sha1_hex(host_rom, ROM_SIZE, hw->rom_hash);
}

static void detect_mem(struct hwinfo *hw)
{
    hw->mem_kb = 2048;
}

/* fixed samples near 1ms so the old 32-bit overflow would be caught:
   true variance is 6712609375 ns^2, way past what int32 holds */
static void collect_entropy(struct entropy_info *e)
{
    static const unsigned long fixed[ENT_SAMPLES] = {
        1000000, 1103000, 951000, 1200000, 1050000, 998000, 1150000, 1023000,
        987000, 1210000, 1075000, 1005000, 963000, 1180000, 1042000, 1017000
    };
    int i;

    memset(e, 0, sizeof(*e));
    e->sample_count = ENT_SAMPLES;
    e->timer_ok = 1;
    for (i = 0; i < ENT_SAMPLES; i++)
        e->samples_ns[i] = fixed[i];
    entropy_compute_stats(e);
}

/* host stub reports UAE detected, same shape the emulator run produces */
static void detect_emulation(struct emu_info *em)
{
    memset(em, 0, sizeof(*em));
    em->uae_resource = 1;
    em->uae_id_string = 0;
    em->passed = 0;
}

static void miner_sleep(long seconds)
{
    (void)seconds;
}

#endif

static void detect_hardware(struct hwinfo *hw)
{
    memset(hw, 0, sizeof(*hw));
    detect_cpu(hw);
    detect_kick(hw);
    detect_rom(hw);
    detect_mem(hw);
}

/* ------------------------------------------------------------------ */
/* JSON payload                                                       */
/* ------------------------------------------------------------------ */

/* escape quote, backslash and control chars; truncates to fit */
static void json_escape(char *dst, int dstlen, const char *src)
{
    int o = 0;
    while (*src && o < dstlen - 8) {
        unsigned char ch = (unsigned char)*src++;
        if (ch == '"' || ch == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)ch;
        } else if (ch < 0x20) {
            sprintf(dst + o, "\\u%04x", ch);
            o += 6;
        } else {
            dst[o++] = (char)ch;
        }
    }
    dst[o] = '\0';
}

/*
 * Attestation payload. Field names match rustchain_mac_universal_miner_v2.2.2.py
 * plus rom_hash/rom_size/kick_version in device for server ROM clustering.
 * Server accepts both model|arch|family and device_* names (2025-12-20 fix).
 * macs is left empty on purpose: unknown emulated OUIs trip the RIP-0147a gate.
 * commitment is opaque to the server (stored as-is), sha1 here.
 */
/*
 * Fingerprint block, shape from fingerprint_checks.py / the linux miner:
 * {"all_passed":bool,"checks":{name:{"passed":bool,"data":{...}}}}
 * Evidence over verdicts: raw EClock samples go in clock_drift.data,
 * anti_emulation lists the concrete indicators found (uae.resource etc).
 */
static int build_fingerprint(char *out, int outlen,
                             const struct entropy_info *e,
                             const struct emu_info *em)
{
    char mean_s[24], stdev_s[24], drift_s[24], cvw_s[24];
    char samples_s[ENT_SAMPLES * 12];
    char indicators_s[80];
    int clock_ok = clock_drift_passed(e);
    int all_ok = clock_ok && em->passed;
    int i, o = 0, len;

    for (i = 0; i < e->sample_count; i++)
        o += sprintf(samples_s + o, "%s%lu", i ? "," : "", e->samples_ns[i]);

    o = 0;
    if (em->uae_resource)
        o += sprintf(indicators_s + o, "\"uae.resource\"");
    if (em->uae_id_string)
        o += sprintf(indicators_s + o, "%s\"bsdsocket_id:uae\"", o ? "," : "");
    indicators_s[o] = '\0';

    len = sprintf(out,
        "{"
        "\"all_passed\":%s,"
        "\"checks\":{"
            "\"clock_drift\":{"
                "\"passed\":%s,"
                "\"data\":{"
                    "\"mean_ns\":%s,"
                    "\"stdev_ns\":%s,"
                    "\"cv\":%s.%06lu,"
                    "\"drift_stdev\":%s,"
                    "\"timer_source\":\"%s\","
                    "\"samples_ns\":[%s]"
                "}"
            "},"
            "\"anti_emulation\":{"
                "\"passed\":%s,"
                "\"data\":{"
                    "\"platform\":\"amigaos\","
                    "\"vm_indicators\":[%s]"
                "}"
            "}"
        "}"
        "}",
        all_ok ? "true" : "false",
        clock_ok ? "true" : "false",
        u64s(e->mean_ns, mean_s),
        u64s(e->stdev_ns, stdev_s),
        u64s(e->cv_ppm / 1000000ULL, cvw_s),
        (unsigned long)(e->cv_ppm % 1000000ULL),
        u64s(e->drift_stdev_ns, drift_s),
        e->timer_ok ? "eclock" : "none",
        samples_s,
        em->passed ? "true" : "false",
        indicators_s);

    if (len >= outlen) return -1;
    return len;
}

static int build_payload(char *out, int outlen,
                         const struct hwinfo *hw,
                         const char *wallet, const char *miner_id,
                         const char *nonce,
                         const struct entropy_info *e,
                         const struct emu_info *em)
{
    char w[128], m[128], n[160];
    char commit_src[512];
    char commitment[41];
    char mean_s[24], var_s[24], score_s[24];
    static char fingerprint[1024];
    int len;

    json_escape(w, sizeof(w), wallet);
    json_escape(m, sizeof(m), miner_id);
    json_escape(n, sizeof(n), nonce);

    if (build_fingerprint(fingerprint, sizeof(fingerprint), e, em) < 0)
        return -1;

    sprintf(commit_src, "%s%s%s%s", n, w, hw->arch, hw->rom_hash);
    sha1_hex((const unsigned char *)commit_src, (unsigned long)strlen(commit_src),
             commitment);

    len = sprintf(out,
        "{"
        "\"miner\":\"%s\","
        "\"miner_id\":\"%s\","
        "\"nonce\":\"%s\","
        "\"report\":{"
            "\"nonce\":\"%s\","
            "\"commitment\":\"%s\","
            "\"derived\":{"
                "\"mean_ns\":%s,"
                "\"variance_ns\":%s,"
                "\"min_ns\":%lu,"
                "\"max_ns\":%lu,"
                "\"sample_count\":%d"
            "},"
            "\"entropy_score\":%s"
        "},"
        "\"device\":{"
            "\"family\":\"m68k\","
            "\"arch\":\"%s\","
            "\"model\":\"Amiga (AROS/FS-UAE)\","
            "\"cpu\":\"%s\","
            "\"fpu\":\"%s\","
            "\"cores\":1,"
            "\"memory_gb\":0,"
            "\"memory_kb\":%lu,"
            "\"machine\":\"m68k\","
            "\"attn_flags\":%lu,"
            "\"rom_hash\":\"%s\","
            "\"rom_size\":%lu,"
            "\"rom_checksum\":%lu,"
            "\"kick_version\":\"%s\","
            "\"miner_version\":\"" MINER_VERSION "\""
        "},"
        "\"signals\":{"
            "\"macs\":[],"
            "\"hostname\":\"amiga-fsuae\""
        "},"
        "\"fingerprint\":%s"
        "}",
        w, m, n, n, commitment,
        u64s(e->mean_ns, mean_s), u64s(e->variance_ns, var_s),
        e->min_ns, e->max_ns, e->sample_count,
        u64s(e->variance_ns, score_s),
        hw->arch, hw->cpu, hw->fpu, hw->mem_kb, hw->attn_flags,
        hw->rom_hash, hw->rom_size, hw->rom_checksum, hw->kick_version,
        fingerprint);

    if (len >= outlen) return -1;
    return len;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                               */
/* ------------------------------------------------------------------ */

static int format_http_post(char *out, int outlen,
                            const char *host, int port,
                            const char *path, const char *body)
{
    int len = (int)strlen(body);
    int n;

    n = sprintf(out,
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: rustchain-amiga/" MINER_VERSION "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, port, len, body);

    if (n >= outlen) return -1;
    return n;
}

static int http_status(const char *resp)
{
    if (strncmp(resp, "HTTP/1.", 7) != 0) return -1;
    return atoi(resp + 9);
}

/* pull a string field value out of a JSON blob, dumb scan, good enough here */
static int json_find_string(const char *json, const char *key,
                            char *out, int outlen)
{
    char pat[64];
    const char *p;
    int o = 0;

    sprintf(pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"' && o < outlen - 1)
        out[o++] = *p++;
    out[o] = '\0';
    return o > 0;
}

#ifndef HOST_TEST

static int parse_ipv4(const char *s, unsigned long *out)
{
    unsigned long parts[4];
    int i = 0;
    char *end;

    for (i = 0; i < 4; i++) {
        parts[i] = strtoul(s, &end, 10);
        if (end == s || parts[i] > 255) return 0;
        if (i < 3 && *end != '.') return 0;
        s = end + 1;
    }
    if (*end != '\0') return 0;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

static int http_request(const char *host, int port,
                        const char *path, const char *body,
                        char *resp, int resplen)
{
    static char req[HTTP_MAX];
    struct sockaddr_in sa;
    unsigned long ip;
    long s;
    int reqlen, got, total = 0;

    reqlen = format_http_post(req, sizeof(req), host, port, path, body);
    if (reqlen < 0) {
        printf("[FAIL] request too large\n");
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = (unsigned short)port;   /* m68k is big endian, htons is identity */

    if (parse_ipv4(host, &ip)) {
        sa.sin_addr.s_addr = ip;
    } else {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he) {
            printf("[FAIL] cannot resolve %s\n", host);
            return -1;
        }
        memcpy(&sa.sin_addr, he->h_addr, 4);
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("[FAIL] socket() failed\n");
        return -1;
    }

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("[FAIL] connect to %s:%d failed\n", host, port);
        CloseSocket(s);
        return -1;
    }

    if (send(s, req, reqlen, 0) != reqlen) {
        printf("[FAIL] send failed\n");
        CloseSocket(s);
        return -1;
    }

    while (total < resplen - 1) {
        got = recv(s, resp + total, resplen - 1 - total, 0);
        if (got <= 0) break;
        total += got;
    }
    resp[total] = '\0';
    CloseSocket(s);

    return total;
}

#endif /* !HOST_TEST */

/* ------------------------------------------------------------------ */
/* Attestation flow                                                   */
/* ------------------------------------------------------------------ */

static void print_detection(const struct hwinfo *hw)
{
    printf("RustChain Amiga Miner v" MINER_VERSION "\n");
    printf("  CPU:       %s (fpu: %s)\n", hw->cpu, hw->fpu);
    printf("  Arch:      %s / family m68k\n", hw->arch);
    printf("  AttnFlags: 0x%04lx\n", hw->attn_flags);
    printf("  Kickstart: %s\n", hw->kick_version);
    printf("  ROM:       sha1=%s size=%lu checksum=%lu\n",
           hw->rom_hash, hw->rom_size, hw->rom_checksum);
    printf("  Free mem:  %lu KB\n", hw->mem_kb);
}

#ifndef HOST_TEST

static int attest_once(const char *host, int port,
                       const char *wallet, const char *miner_id,
                       const struct hwinfo *hw)
{
    static char resp[RESP_MAX];
    static char payload[PAYLOAD_MAX];
    char nonce[160];
    struct entropy_info ent;
    struct emu_info emu;
    int n, status;

    printf("[ATTEST] requesting challenge from %s:%d\n", host, port);
    n = http_request(host, port, "/attest/challenge", "{}", resp, sizeof(resp));
    if (n <= 0) return 0;

    status = http_status(resp);
    if (status != 200) {
        printf("[FAIL] challenge HTTP %d\n", status);
        return 0;
    }
    if (!json_find_string(resp, "nonce", nonce, sizeof(nonce))) {
        printf("[FAIL] no nonce in challenge response\n");
        return 0;
    }
    printf("[OK] got nonce\n");

    collect_entropy(&ent);
    detect_emulation(&emu);
    if (!emu.passed)
        printf("[INFO] UAE detected, reporting honestly (flagging expected)\n");
    if (build_payload(payload, sizeof(payload), hw, wallet, miner_id,
                      nonce, &ent, &emu) < 0) {
        printf("[FAIL] payload too large\n");
        return 0;
    }

    printf("[ATTEST] submitting (%d bytes)\n", (int)strlen(payload));
    n = http_request(host, port, "/attest/submit", payload, resp, sizeof(resp));
    if (n <= 0) return 0;

    status = http_status(resp);
    if (status == 200 && strstr(resp, "\"ok\"")) {
        char ticket[64];
        printf("[PASS] attestation accepted\n");
        if (json_find_string(resp, "ticket_id", ticket, sizeof(ticket)))
            printf("       ticket: %s\n", ticket);
        if (strstr(resp, "\"fingerprint_passed\":false") ||
            strstr(resp, "\"fingerprint_passed\": false"))
            printf("       flagged: emulated hardware, minimal reward (by design)\n");
        return 1;
    }

    printf("[FAIL] submit HTTP %d\n", status);
    /* show start of body so failures are debuggable from miner.log */
    {
        const char *body = strstr(resp, "\r\n\r\n");
        if (body) {
            char snip[201];
            strncpy(snip, body + 4, 200);
            snip[200] = '\0';
            printf("       %s\n", snip);
        }
    }
    return 0;
}

#endif /* !HOST_TEST */

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

#ifndef HOST_TEST

int main(int argc, char **argv)
{
    struct hwinfo hw;
    char host[128];
    int port = DEFAULT_NODE_PORT;
    char wallet[96];
    int test_only = 0, dry_run = 0, once = 0;
    int i;

    strcpy(host, DEFAULT_NODE_HOST);
    strcpy(wallet, DEFAULT_WALLET);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test-only") == 0) test_only = 1;
        else if (strcmp(argv[i], "--dry-run") == 0) dry_run = 1;
        else if (strcmp(argv[i], "--once") == 0) once = 1;
        else if (strcmp(argv[i], "--node") == 0 && i + 1 < argc) {
            char *colon;
            strncpy(host, argv[++i], sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
            colon = strchr(host, ':');
            if (colon) {
                *colon = '\0';
                port = atoi(colon + 1);
            }
        }
        else if (strcmp(argv[i], "--wallet") == 0 && i + 1 < argc) {
            strncpy(wallet, argv[++i], sizeof(wallet) - 1);
            wallet[sizeof(wallet) - 1] = '\0';
        }
        else {
            printf("usage: rustchain_amiga [--test-only] [--dry-run] [--once]\n");
            printf("                       [--node host:port] [--wallet id]\n");
            return 5;
        }
    }

    detect_hardware(&hw);
    print_detection(&hw);

    if (test_only)
        return 0;

    if (dry_run) {
        static char payload[PAYLOAD_MAX];
        struct entropy_info ent;
        struct emu_info emu;
        collect_entropy(&ent);
        detect_emulation(&emu);
        if (build_payload(payload, sizeof(payload), &hw, wallet, wallet,
                          "dry-run-nonce", &ent, &emu) < 0) {
            printf("[FAIL] payload too large\n");
            return 10;
        }
        printf("--- payload for POST http://%s:%d/attest/submit ---\n", host, port);
        printf("%s\n", payload);
        timer_close();
        return 0;
    }

    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 3);
    if (!SocketBase) {
        printf("[FAIL] bsdsocket.library not available\n");
        printf("       start a TCP/IP stack (Roadshow/AmiTCP) or enable\n");
        printf("       bsdsocket_library = 1 in FS-UAE\n");
        return 10;
    }

    for (;;) {
        attest_once(host, port, wallet, wallet, &hw);
        if (once)
            break;
        printf("[SLEEP] next attestation in %d minutes\n", POLL_SECONDS / 60);
        miner_sleep(POLL_SECONDS);
    }

    CloseLibrary(SocketBase);
    timer_close();
    return 0;
}

#else /* HOST_TEST */

static int failures = 0;

static void check(const char *name, int cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) failures++;
}

static void check_str(const char *name, const char *got, const char *want)
{
    int ok = strcmp(got, want) == 0;
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        printf("        got:  %s\n        want: %s\n", got, want);
        failures++;
    }
}

int main(void)
{
    char hex[41];
    struct hwinfo hw;
    struct entropy_info ent;
    struct emu_info emu;
    static char payload[PAYLOAD_MAX];
    static char req[HTTP_MAX];
    char nonce_out[160];
    int plen, rlen;

    printf("rustchain_amiga_miner host smoke test\n");

    printf("SHA-1 vectors:\n");
    sha1_hex((const unsigned char *)"", 0, hex);
    check_str("sha1(empty)", hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    sha1_hex((const unsigned char *)"abc", 3, hex);
    check_str("sha1(abc)", hex, "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha1_hex((const unsigned char *)"The quick brown fox jumps over the lazy dog", 43, hex);
    check_str("sha1(fox)", hex, "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");

    printf("ROM hash (512KB synthetic pattern):\n");
    detect_hardware(&hw);
    print_detection(&hw);
    miner_sleep(0);
    check_str("sha1(rom pattern)", hw.rom_hash,
              "aabd0895b5c2ec38884f5d54cff3fc6aab4fe7ba");
    check("rom_size is 524288", hw.rom_size == 524288UL);
    check("rom_checksum byte-sum", hw.rom_checksum == 66846720UL);

    printf("64-bit entropy math (regression for the variance overflow bug):\n");
    collect_entropy(&ent);
    {
        char b[24];
        check_str("u64s(6712609375)", u64s(6712609375ULL, b), "6712609375");
        check_str("u64s(0)", u64s(0ULL, b), "0");
        check_str("u64s(17179869184)", u64s(17179869184ULL, b), "17179869184");
        check_str("u64s(28133137871)", u64s(28133137871ULL, b), "28133137871");
        check_str("u64s(u64 max)", u64s(18446744073709551615ULL, b),
                  "18446744073709551615");
    }

    printf("isqrt64 (shift-free, regression for the m68k 2^34 stdev bug):\n");
    {
        static const struct { unsigned long long x, r; } sq[] = {
            {0ULL, 0ULL}, {1ULL, 1ULL}, {2ULL, 1ULL}, {3ULL, 1ULL},
            {4ULL, 2ULL}, {15ULL, 3ULL}, {16ULL, 4ULL}, {17ULL, 4ULL},
            {999999ULL, 999ULL}, {1000000ULL, 1000ULL},
            {17179869184ULL, 131072ULL},            /* 2^34, the bad value */
            {28133137871ULL, 167729ULL},            /* live-run variance */
            {4294967295ULL, 65535ULL},
            {4611686018427387904ULL, 2147483648ULL},/* 2^62 */
            {18446744065119617025ULL, 4294967295ULL},/* (2^32-1)^2 */
            {18446744073709551615ULL, 4294967295ULL} /* u64 max */
        };
        unsigned int k;
        int sq_ok = 1;
        for (k = 0; k < sizeof(sq) / sizeof(sq[0]); k++) {
            if (isqrt64(sq[k].x) != sq[k].r) {
                char bx[24], bg[24], bw[24];
                printf("        isqrt64(%s) = %s, want %s\n",
                       u64s(sq[k].x, bx), u64s(isqrt64(sq[k].x), bg),
                       u64s(sq[k].r, bw));
                sq_ok = 0;
            }
        }
        check("isqrt64 table (16 cases)", sq_ok);
        check("live-run stdev is 167729 not 2^34",
              isqrt64(28133137871ULL) == 167729ULL &&
              isqrt64(28133137871ULL) != 17179869184ULL);
    }
    printf("composed pipeline at live-run magnitudes (cv must be ~0.26 not thousands):\n");
    {
        struct entropy_info e2;
        int k;
        memset(&e2, 0, sizeof(e2));
        e2.sample_count = ENT_SAMPLES;
        e2.timer_ok = 1;
        for (k = 0; k < ENT_SAMPLES; k++)
            e2.samples_ns[k] = (k % 2) ? 1200000UL : 700000UL;
        entropy_compute_stats(&e2);
        check("mean 950000", e2.mean_ns == 950000ULL);
        check("variance 62500000000", e2.variance_ns == 62500000000ULL);
        check("stdev 250000", e2.stdev_ns == 250000ULL);
        check("cv_ppm 263157", e2.cv_ppm == 263157ULL);
        check("drift_stdev 498887", e2.drift_stdev_ns == 498887ULL);
    }
    printf("build sanity: sizeof(long)=%d sizeof(void*)=%d sizeof(long long)=%d\n",
           (int)sizeof(long), (int)sizeof(void *), (int)sizeof(long long));
    check("mean_ns 1059625", ent.mean_ns == 1059625ULL);
    check("variance_ns 6712609375 (would be negative in 32-bit)",
          ent.variance_ns == 6712609375ULL);
    check("stdev_ns 81930", ent.stdev_ns == 81930ULL);
    check("cv_ppm 77319", ent.cv_ppm == 77319UL);
    check("drift_stdev_ns 142080", ent.drift_stdev_ns == 142080ULL);
    check("min/max consistent", ent.min_ns == 951000UL && ent.max_ns == 1210000UL);
    check("clock_drift passes on real-looking samples", clock_drift_passed(&ent));

    printf("JSON payload:\n");
    detect_emulation(&emu);
    plen = build_payload(payload, sizeof(payload), &hw,
                         "amiga-fsuae-scott", "amiga-fsuae-scott",
                         "cafebabe1234", &ent, &emu);
    check("payload builds", plen > 0);
    check("payload length sane", plen > 400 && plen < PAYLOAD_MAX);
    check("has miner", strstr(payload, "\"miner\":\"amiga-fsuae-scott\"") != NULL);
    check("has miner_id", strstr(payload, "\"miner_id\":\"amiga-fsuae-scott\"") != NULL);
    check("has nonce", strstr(payload, "\"nonce\":\"cafebabe1234\"") != NULL);
    check("has family m68k", strstr(payload, "\"family\":\"m68k\"") != NULL);
    check("has arch 68030", strstr(payload, "\"arch\":\"68030\"") != NULL);
    check("has model", strstr(payload, "\"model\":\"Amiga (AROS/FS-UAE)\"") != NULL);
    check("has rom_hash", strstr(payload, "\"rom_hash\":\"aabd0895") != NULL);
    check("has rom_size", strstr(payload, "\"rom_size\":524288") != NULL);
    check("has rom_checksum", strstr(payload, "\"rom_checksum\":66846720") != NULL);
    check("has attn_flags", strstr(payload, "\"attn_flags\":55") != NULL);
    check("has kick_version", strstr(payload, "\"kick_version\":\"47.96\"") != NULL);
    check("has commitment", strstr(payload, "\"commitment\":\"") != NULL);
    check("has derived entropy", strstr(payload, "\"mean_ns\":1059625") != NULL);
    check("has 64-bit variance", strstr(payload, "\"variance_ns\":6712609375") != NULL);
    check("no negative numbers anywhere", strstr(payload, ":-") == NULL);
    check("has empty macs", strstr(payload, "\"macs\":[]") != NULL);

    printf("fingerprint block:\n");
    check("has fingerprint", strstr(payload, "\"fingerprint\":{") != NULL);
    check("has checks", strstr(payload, "\"checks\":{") != NULL);
    check("clock_drift present", strstr(payload, "\"clock_drift\":{") != NULL);
    check("clock_drift passed", strstr(payload, "\"clock_drift\":{\"passed\":true") != NULL);
    check("cv fixed-point 0.077319", strstr(payload, "\"cv\":0.077319") != NULL);
    check("drift_stdev present", strstr(payload, "\"drift_stdev\":142080") != NULL);
    check("raw samples as evidence",
          strstr(payload, "\"samples_ns\":[1000000,1103000,951000,") != NULL);
    check("anti_emulation present", strstr(payload, "\"anti_emulation\":{") != NULL);
    check("anti_emulation failed (UAE stub)",
          strstr(payload, "\"anti_emulation\":{\"passed\":false") != NULL);
    check("uae.resource indicator",
          strstr(payload, "\"vm_indicators\":[\"uae.resource\"]") != NULL);
    check("all_passed false", strstr(payload, "\"all_passed\":false") != NULL);

    printf("fingerprint block on real hardware (no UAE):\n");
    {
        struct emu_info real_emu;
        static char p2[PAYLOAD_MAX];
        memset(&real_emu, 0, sizeof(real_emu));
        real_emu.passed = 1;
        check("payload builds", build_payload(p2, sizeof(p2), &hw,
              "real-amiga", "real-amiga", "n0nce", &ent, &real_emu) > 0);
        check("anti_emulation passes",
              strstr(p2, "\"anti_emulation\":{\"passed\":true") != NULL);
        check("empty vm_indicators", strstr(p2, "\"vm_indicators\":[]") != NULL);
        check("all_passed true", strstr(p2, "\"all_passed\":true") != NULL);
    }

    printf("JSON escaping:\n");
    {
        char esc[128];
        json_escape(esc, sizeof(esc), "bad\"wallet\\x\n");
        check_str("escape quotes/backslash/ctrl", esc, "bad\\\"wallet\\\\x\\u000a");
    }

    printf("JSON payload is parseable (paranoia brace count):\n");
    {
        int depth = 0, i, instr = 0, bad = 0;
        for (i = 0; i < plen; i++) {
            char ch = payload[i];
            if (instr) {
                if (ch == '\\') i++;
                else if (ch == '"') instr = 0;
            } else {
                if (ch == '"') instr = 1;
                else if (ch == '{') depth++;
                else if (ch == '}') depth--;
                if (depth < 0) bad = 1;
            }
        }
        check("braces balance", depth == 0 && !bad && !instr);
    }

    printf("HTTP formatting:\n");
    rlen = format_http_post(req, sizeof(req), "50.28.86.131", 8088,
                            "/attest/submit", payload);
    check("request builds", rlen > 0);
    check("starts with POST", strncmp(req, "POST /attest/submit HTTP/1.1\r\n", 30) == 0);
    check("has host header", strstr(req, "Host: 50.28.86.131:8088\r\n") != NULL);
    {
        char want[64];
        sprintf(want, "Content-Length: %d\r\n", plen);
        check("content-length matches body", strstr(req, want) != NULL);
    }
    check("header/body split", strstr(req, "\r\n\r\n{") != NULL);

    printf("HTTP response parsing:\n");
    {
        const char *resp =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
            "{\"nonce\":\"deadbeef\",\"expires_at\":123}";
        check("status 200", http_status(resp) == 200);
        check("nonce extract", json_find_string(resp, "nonce", nonce_out,
              sizeof(nonce_out)) && strcmp(nonce_out, "deadbeef") == 0);
    }
    {
        const char *resp412 = "HTTP/1.1 412 Precondition Failed\r\n\r\n{}";
        check("status 412", http_status(resp412) == 412);
    }

    printf("\n%s (%d failures)\n", failures ? "SMOKE TEST FAILED" : "ALL CHECKS PASSED",
           failures);
    if (!failures) {
        printf("\nsample payload:\n%s\n", payload);
    }
    return failures ? 20 : 0;
}

#endif /* HOST_TEST */
