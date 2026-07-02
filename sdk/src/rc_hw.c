/*
 * rc_hw.c - hardware detection, timing entropy, UAE self-detection.
 * Extracted from the working miner (rustchain_amiga_miner.c), behavior
 * identical. Built with -DHOST_TEST the Amiga calls are stubbed with
 * the same fixed values the miner's host test used, so the regression
 * numbers carry over unchanged.
 */

#include <stdio.h>
#include <string.h>

#include "rustchain/rc_hw.h"
#include "rustchain/rc_sha1.h"
#include "rustchain/rc_u64.h"

#ifndef HOST_TEST
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct Device *TimerBase = NULL;
#include <proto/timer.h>

/* owned by rc_http.c; used here to scan the bsdsocket id string */
extern struct Library *SocketBase;

extern struct ExecBase *SysBase;
#endif

/* ------------------------------------------------------------------ */
/* portable stats                                                     */
/* ------------------------------------------------------------------ */

/* all accumulation in 64-bit. The old 32-bit squaring overflowed and
   reported variance_ns = -942169152 on real EClock deltas near 1ms. */
void rc_entropy_stats(struct rc_entropy *e)
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
    e->stdev_ns = rc_isqrt64(e->variance_ns);
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
        e->drift_stdev_ns = rc_isqrt64((unsigned long long)(dvar / (n - 1)));
    } else {
        e->drift_stdev_ns = 0;
    }
}

/* honest local mirror of the server thresholds:
   cv >= 0.0001 and nonzero drift, and the timer actually worked */
int rc_clock_drift_passed(const struct rc_entropy *e)
{
    return e->timer_ok && e->cv_ppm >= 100 && e->drift_stdev_ns > 0;
}

/* ------------------------------------------------------------------ */
/* Amiga detection                                                    */
/* ------------------------------------------------------------------ */

#ifndef HOST_TEST

static void detect_cpu(struct rc_hwinfo *hw)
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

static void detect_kick(struct rc_hwinfo *hw)
{
    sprintf(hw->kick_version, "%d.%d",
            (int)SysBase->LibNode.lib_Version,
            (int)SysBase->SoftVer);
}

static void detect_rom(struct rc_hwinfo *hw)
{
    const unsigned char *rom = (const unsigned char *)RC_ROM_BASE;
    unsigned long sum = 0, i;

    hw->rom_size = RC_ROM_SIZE;
    rc_sha1_hex(rom, RC_ROM_SIZE, hw->rom_hash);
    /* same algorithm as tools/amiga/amiga_fingerprint.asm intends:
       unsigned byte sum of the 512KB ROM window */
    for (i = 0; i < RC_ROM_SIZE; i++)
        sum += rom[i];
    hw->rom_checksum = sum;
}

static void detect_mem(struct rc_hwinfo *hw)
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

void rc_timer_cleanup(void)
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

void rc_entropy_collect(struct rc_entropy *e)
{
    int i, j;
    volatile unsigned long acc = 0;

    memset(e, 0, sizeof(*e));
    e->sample_count = RC_ENT_SAMPLES;
    e->timer_ok = timer_open();

    for (i = 0; i < RC_ENT_SAMPLES; i++) {
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

    rc_entropy_stats(e);
}

/* real UAE self-detection, evidence for the anti_emulation check.
   uae.resource exists on every UAE flavor including FS-UAE. */
void rc_emu_detect(struct rc_emu *em)
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

void rc_sleep(long seconds)
{
    Delay(seconds * 50);
}

#else /* HOST_TEST stubs, same fixed values as the miner's host test */

static unsigned char host_rom[RC_ROM_SIZE];

static void detect_cpu(struct rc_hwinfo *hw)
{
    strcpy(hw->arch, "68030");
    strcpy(hw->cpu, "MC68030");
    strcpy(hw->fpu, "68882");
    /* AFF_68010|AFF_68020|AFF_68030|AFF_68881|AFF_68882 */
    hw->attn_flags = 0x37;
}

static void detect_kick(struct rc_hwinfo *hw)
{
    strcpy(hw->kick_version, "47.96");
}

static void detect_rom(struct rc_hwinfo *hw)
{
    unsigned long i, sum = 0;
    for (i = 0; i < RC_ROM_SIZE; i++) {
        host_rom[i] = (unsigned char)((i * 7 + 13) & 0xFF);
        sum += host_rom[i];
    }
    hw->rom_size = RC_ROM_SIZE;
    hw->rom_checksum = sum;
    rc_sha1_hex(host_rom, RC_ROM_SIZE, hw->rom_hash);
}

static void detect_mem(struct rc_hwinfo *hw)
{
    hw->mem_kb = 2048;
}

/* fixed samples near 1ms so the old 32-bit overflow would be caught:
   true variance is 6712609375 ns^2, way past what int32 holds */
void rc_entropy_collect(struct rc_entropy *e)
{
    static const unsigned long fixed[RC_ENT_SAMPLES] = {
        1000000, 1103000, 951000, 1200000, 1050000, 998000, 1150000, 1023000,
        987000, 1210000, 1075000, 1005000, 963000, 1180000, 1042000, 1017000
    };
    int i;

    memset(e, 0, sizeof(*e));
    e->sample_count = RC_ENT_SAMPLES;
    e->timer_ok = 1;
    for (i = 0; i < RC_ENT_SAMPLES; i++)
        e->samples_ns[i] = fixed[i];
    rc_entropy_stats(e);
}

/* host stub reports UAE detected, same shape the emulator run produces */
void rc_emu_detect(struct rc_emu *em)
{
    memset(em, 0, sizeof(*em));
    em->uae_resource = 1;
    em->uae_id_string = 0;
    em->passed = 0;
}

void rc_sleep(long seconds)
{
    (void)seconds;
}

void rc_timer_cleanup(void)
{
}

#endif /* HOST_TEST */

void rc_hw_detect(struct rc_hwinfo *hw)
{
    memset(hw, 0, sizeof(*hw));
    detect_cpu(hw);
    detect_kick(hw);
    detect_rom(hw);
    detect_mem(hw);
}

void rc_hw_print(const struct rc_hwinfo *hw)
{
    printf("  CPU:       %s (fpu: %s)\n", hw->cpu, hw->fpu);
    printf("  Arch:      %s / family m68k\n", hw->arch);
    printf("  AttnFlags: 0x%04lx\n", hw->attn_flags);
    printf("  Kickstart: %s\n", hw->kick_version);
    printf("  ROM:       sha1=%s size=%lu checksum=%lu\n",
           hw->rom_hash, hw->rom_size, hw->rom_checksum);
    printf("  Free mem:  %lu KB\n", hw->mem_kb);
}
