/*
 * rc_hw.h - Amiga hardware detection, timing entropy, UAE self-detection.
 *
 * Detection reads live hardware only:
 *   CPU        ExecBase AttnFlags (68000..68060, FPU bits)
 *   Kickstart  SysBase LibNode lib_Version . SoftVer
 *   ROM        512KB window at 0xF80000, SHA-1 plus unsigned byte sum
 *   Memory     AvailMem(MEMF_ANY)
 *   Entropy    timer.device UNIT_ECLOCK loop timings (~709 kHz PAL)
 *   Emulation  OpenResource("uae.resource") plus bsdsocket id string
 *
 * All timing stats accumulate in unsigned long long. The first miner
 * build used 32-bit signed accumulation and reported variance_ns as
 * -942169152 on real ~1ms EClock deltas. Do not go back.
 *
 * Built with -DHOST_TEST the Amiga calls are stubbed with fixed values
 * so the portable math and payload code can be tested on Linux.
 *
 * Part of librustchain, the RustChain SDK for AmigaOS.
 */

#ifndef RC_HW_H
#define RC_HW_H

#define RC_ROM_BASE 0xF80000UL
#define RC_ROM_SIZE 524288UL

#define RC_ENT_SAMPLES 16

struct rc_hwinfo {
    char arch[16];          /* "68030" */
    char cpu[32];           /* "MC68030" */
    char fpu[16];           /* "68882" or "none" */
    char kick_version[16];  /* "47.96" */
    char rom_hash[41];      /* sha1 hex of the 512KB ROM */
    unsigned long rom_size;
    unsigned long rom_checksum; /* unsigned byte sum, matches tools/rom/checksums.json */
    unsigned long attn_flags;   /* raw ExecBase AttnFlags word */
    unsigned long mem_kb;       /* free memory, KB */
};

struct rc_entropy {
    unsigned long samples_ns[RC_ENT_SAMPLES];
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

struct rc_emu {
    int uae_resource;   /* uae.resource present */
    int uae_id_string;  /* "UAE" in bsdsocket.library id string */
    int passed;         /* no emulation indicators found */
};

/* fill everything except entropy and emulation */
void rc_hw_detect(struct rc_hwinfo *hw);

/* print the detection fields to stdout, one per line */
void rc_hw_print(const struct rc_hwinfo *hw);

/*
 * Collect RC_ENT_SAMPLES EClock loop timings and compute the stats.
 * Opens timer.device lazily; call rc_timer_cleanup() before exit.
 * Falls back to zeroed samples with timer_ok = 0 if the timer fails.
 */
void rc_entropy_collect(struct rc_entropy *e);

/* recompute the derived stats from samples_ns / sample_count */
void rc_entropy_stats(struct rc_entropy *e);

/*
 * Honest local mirror of the server thresholds: cv >= 0.0001, nonzero
 * drift, and the timer actually worked.
 */
int rc_clock_drift_passed(const struct rc_entropy *e);

/*
 * Real UAE self-detection, evidence for the anti_emulation check.
 * uae.resource exists on every UAE flavor including FS-UAE. Reporting
 * this honestly is the point: emulated hardware is expected to be
 * flagged server side and get minimal weight.
 */
void rc_emu_detect(struct rc_emu *em);

/* release timer.device if rc_entropy_collect opened it */
void rc_timer_cleanup(void);

/* dos.library Delay based sleep, whole seconds */
void rc_sleep(long seconds);

#endif /* RC_HW_H */
