#include "pdp18b_defs.h"
#include "sim_console.h"
#include "simhp4_probe.h"
#include "simhp4_rb09_persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * SIMHP4 R0A3: map DoctorWkt/pdp7-unix build/unixv0.simh to Tab5.
 *
 * Guest-visible setup:
 *   set cpu 8k
 *   set cpu eae
 *   set tti unix
 *   set rb ena / attach image.fs
 *   set g2in ena / local attach (TELNET transport replaced by Tab5)
 *   load boot.rim 010000
 *   go
 *
 * LPT/DRM/DT are not in this minimal machine graph, equivalent to disabled.
 * The exact Open SIMH PDP18B/pdp18b_g2tty.c supplies device 005/043/044.
 */

extern UNIT cpu_unit;
extern REG cpu_reg[];
extern t_stat cpu_reset(DEVICE *dptr);
extern t_stat sim_instr(void);
extern int32 *M;
extern int32 PC;

extern UNIT clk_unit;
extern DIB clk_dib;
extern t_stat clk_reset(DEVICE *dptr);

extern UNIT tti_unit;
extern DIB tti_dib;
extern t_stat tti_reset(DEVICE *dptr);

extern UNIT tto_unit;
extern DIB tto_dib;
extern t_stat tto_reset(DEVICE *dptr);
extern t_stat tty_set_mode(UNIT *uptr, int32 val, CONST char *cptr, void *desc);

extern UNIT rb_unit;
extern REG rb_reg[];
extern DIB rb_dib;
extern t_stat rb_reset(DEVICE *dptr);
extern int32 rb_sta;

extern DEVICE g2out_dev;
extern DEVICE g2in_dev;
extern UNIT g2in_unit;
extern t_stat g2_reset(DEVICE *dptr);
extern t_stat g2_attach(UNIT *uptr, CONST char *cptr);

/* pdp18b_cpu.c private flag layout; set cpu eae == clear UNIT_NOEAE. */
#define SIMHP4_UNIT_NOEAE (1u << (UNIT_V_UF + 0))

#define SIMHP4_TTUF_UNIX (1u << (TTUF_V_UF + 1))

static DEVICE simhp4_cpu_dev = {
    "CPU", &cpu_unit, cpu_reg, NULL,
    1, 8, ADDRSIZE, 1, 8, 18,
    NULL, NULL, &cpu_reset,
    NULL, NULL, NULL,
    NULL, 0
};

static DEVICE simhp4_clk_dev = {
    "CLK", &clk_unit, NULL, NULL,
    1, 10, 31, 1, 8, 8,
    NULL, NULL, &clk_reset,
    NULL, NULL, NULL,
    &clk_dib, 0
};

static DEVICE simhp4_tti_dev = {
    "TTI", &tti_unit, NULL, NULL,
    1, 10, 31, 1, 8, 8,
    NULL, NULL, &tti_reset,
    NULL, NULL, NULL,
    &tti_dib, 0
};

static DEVICE simhp4_tto_dev = {
    "TTO", &tto_unit, NULL, NULL,
    1, 10, 31, 1, 8, 8,
    NULL, NULL, &tto_reset,
    NULL, NULL, NULL,
    &tto_dib, 0
};

static DEVICE simhp4_rb_dev = {
    "RB", &rb_unit, rb_reg, NULL,
    1, 8, 21, 1, 8, 18,
    NULL, NULL, &rb_reset,
    NULL, NULL, NULL,
    &rb_dib, 0
};

char sim_name[64] = "PDP-7";
REG *sim_PC = &cpu_reg[0];
int32 sim_emax = 3;

DEVICE *sim_devices[] = {
    &simhp4_cpu_dev,
    &simhp4_clk_dev,
    &simhp4_tti_dev,
    &simhp4_tto_dev,
    &simhp4_rb_dev,
    &g2out_dev,
    &g2in_dev,
    NULL
};

const char *sim_stop_messages[SCPE_BASE] = {
    "Unknown error",
    "Undefined instruction",
    "HALT instruction",
    "Breakpoint",
    "Nested XCT's",
    "Invalid API interrupt",
    "Non-standard device number",
    "Memory management error",
    "FP15 instruction disabled",
    "DECtape off reel",
    "Infinite loop"
};

static int simhp4_rb09_load_sd_image(const char *path, int *words_out)
{
    FILE *fp;
    int32 *buf;
    size_t cap;
    size_t nread;
    size_t i;

    if (path == NULL)
        return -1;

    fp = fopen(path, "rb");
    if (fp == NULL)
        return -2;

    cap = (size_t)rb_unit.capac;
    if (cap == 0) {
        fclose(fp);
        return -3;
    }

    if (rb_unit.filebuf != NULL) {
        free(rb_unit.filebuf);
        rb_unit.filebuf = NULL;
    }
    if (rb_unit.filebuf2 != NULL) {
        free(rb_unit.filebuf2);
        rb_unit.filebuf2 = NULL;
    }

    buf = (int32 *)calloc(cap, sizeof(int32));
    if (buf == NULL) {
        fclose(fp);
        return -4;
    }

    nread = fread(buf, sizeof(int32), cap, fp);
    if (ferror(fp)) {
        fclose(fp);
        free(buf);
        return -5;
    }
    fclose(fp);

    if (nread != cap) {
        free(buf);
        return -6;
    }

    for (i = 0; i < nread; ++i)
        buf[i] &= DMASK;

    rb_unit.filebuf = buf;
    rb_unit.filebuf2 = NULL;
    rb_unit.flags |= (UNIT_BUF | UNIT_ATT);
    rb_unit.hwmark = (t_addr)nread;
    rb_unit.pos = 0;

    if (words_out)
        *words_out = (int)nread;
    return 0;
}

/* Exact PDP-7/9/15 Hardware Read-In word format used by SIMH.
 * Three marked bytes supply 6 data bits each.  Bit 6 of each byte is
 * accumulated separately; bit 0 of the three-bit result marks the final
 * word on the tape. */
static int simhp4_hri_getword(FILE *fp, int32 *hi)
{
    int32 word = 0;
    int32 bits = 0;
    int st = 0;
    int ch;

    do {
        ch = getc(fp);
        if (ch == EOF)
            return -1;
        if (ch & 0200) {
            word = (word << 6) | (ch & 077);
            bits = (bits << 1) | ((ch >> 6) & 1);
            ++st;
        }
    } while (st < 3);

    if (hi)
        *hi = bits;
    return word;
}

static int simhp4_load_hri_boot(const char *path,
                                int32 origin,
                                int *words_out)
{
    FILE *fp;
    int words = 0;

    fp = fopen(path, "rb");
    if (fp == NULL)
        return -1;

    for (;;) {
        int32 bits = 0;
        int32 val = simhp4_hri_getword(fp, &bits);

        if (val < 0) {
            fclose(fp);
            return -2;
        }

        if (bits & 1) {
            if ((val & 0760000) == OP_JMP) {
                PC = ((origin - 1) & 060000) | (val & 017777);
            } else if (val != OP_HLT) {
                fclose(fp);
                return -3;
            }
            break;
        }

        if (MEM_ADDR_OK(origin))
            M[origin++] = val & DMASK;
        ++words;
    }

    fclose(fp);
    if (words_out)
        *words_out = words;
    return 0;
}

int simhp4_pdp7_unixv0_g2_interactive(const char *boot_path,
                                      const char *disk_path,
                                      int *disk_load_rc_out,
                                      int *disk_words_out,
                                      int *boot_load_rc_out,
                                      int *boot_words_out,
                                      int *g2_attach_rc_out,
                                      int *reason_out,
                                      int *pc_out,
                                      int *rb_status_out,
                                      int *shell_seen_out)
{
    t_stat reason;
    int disk_rc = 0;
    int disk_words = 0;
    int boot_rc = 0;
    int boot_words = 0;
    int g2_rc = 0;
    int shell_seen = 0;

    /* unixv0.simh: set cpu 8k / set cpu eae. */
    cpu_unit.capac = 8192;
    cpu_unit.flags &= ~SIMHP4_UNIT_NOEAE;

    reason = cpu_reset(&simhp4_cpu_dev);
    if (reason != SCPE_OK) {
        if (disk_load_rc_out) *disk_load_rc_out = -10;
        if (boot_load_rc_out) *boot_load_rc_out = -10;
        if (reason_out) *reason_out = reason;
        return 0;
    }

    memset(M, 0, (size_t)cpu_unit.capac * sizeof(*M));

    /* RESET state before boot media are connected. */
    clk_reset(&simhp4_clk_dev);
    tti_reset(&simhp4_tti_dev);
    tto_reset(&simhp4_tto_dev);
    rb_reset(&simhp4_rb_dev);

    /* unixv0.simh:
     *   set g2in ena
     *   att -U g2in 12345
     *
     * R0A4 establishes a clean device state BEFORE attach, then the original
     * g2_attach() schedules upstream g2in_svc() at guest time 0.  Only the
     * host transport is local; pdp18b_g2tty.c remains unmodified. */
    g2in_dev.flags &= ~DEV_DIS;
    g2out_dev.flags &= ~DEV_DIS;
    simhp4_g2_local_reset();
    g2_reset(&g2in_dev);

    g2_rc = (int)g2_attach(&g2in_unit, "TAB5-LOCAL");
    if (g2_rc != SCPE_OK)
        goto done_without_run;

    disk_rc = simhp4_rb09_load_sd_image(disk_path, &disk_words);
    if (disk_rc != 0)
        goto done_without_run;

    /* R0A9R12: keep RB09 guest I/O entirely RAM-speed on CPU1.
     * Persistence is a separate CPU0 task which flushes only dirty 512-byte
     * blocks to the exact SD image. */
    disk_rc = simhp4_rb09_persist_start(disk_path,
                                        (int32 *)rb_unit.filebuf,
                                        (size_t)disk_words);
    if (disk_rc != 0)
        goto done_without_run;

    boot_rc = simhp4_load_hri_boot(boot_path, 010000, &boot_words);
    if (boot_rc != 0)
        goto done_without_run;

    simhp4_tty_capture_reset();
    simhp4_tti_input_reset();

    /* Do NOT reset the local G2 transport here: it is already attached and
     * scheduled.  The shell watch is a non-stopping checkpoint only.
     * pbsh.s defines prompt as "@ ". */
    simhp4_g2_watch_begin("@ ");

    /* R0A9R9: establish the exact upstream "set tti unix" state at the final
     * boundary before guest execution.  R0A9R8 proved remote CR reached TTI
     * as plain 0x0D, so the UNIX-v0 mark-parity CR->LF conversion did not fire. */
    reason = tty_set_mode(&tti_unit,
                          (int32)(SIMHP4_TTUF_UNIX | TT_PAR_MARK | TT_MODE_7B),
                          NULL, NULL);
    if (reason != SCPE_OK) {
        if (disk_load_rc_out) *disk_load_rc_out = -18;
        if (boot_load_rc_out) *boot_load_rc_out = -18;
        if (reason_out) *reason_out = reason;
        return 0;
    }

    {
        const uint32 tty_mask = (uint32)(SIMHP4_TTUF_UNIX | TT_PAR);
        const uint32 tty_expect = (uint32)(SIMHP4_TTUF_UNIX | TT_PAR_MARK);
        if (((tti_unit.flags & tty_mask) != tty_expect) ||
            ((tto_unit.flags & tty_mask) != tty_expect)) {
            printf("SIMHP4_R0A9R9 FATAL TTY UNIX mode verify failed tti=%08lx tto=%08lx expect=%08lx\n",
                   (unsigned long)tti_unit.flags,
                   (unsigned long)tto_unit.flags,
                   (unsigned long)tty_expect);
            if (disk_load_rc_out) *disk_load_rc_out = -19;
            if (boot_load_rc_out) *boot_load_rc_out = -19;
            if (reason_out) *reason_out = SCPE_IERR;
            return 0;
        }

        printf("SIMHP4_R0A9R9 TTY UNIX mode VERIFIED tti=%08lx tto=%08lx\n",
               (unsigned long)tti_unit.flags,
               (unsigned long)tto_unit.flags);
    }

    sim_is_running = TRUE;
    reason = sim_instr();
    sim_is_running = FALSE;

    /* R0A5 operational invariant:
     * a healthy UNIX session does not return from sim_instr().
     * If execution reaches here, guest execution stopped unexpectedly. */
    shell_seen = simhp4_g2_watch_hit();

    if (disk_load_rc_out) *disk_load_rc_out = 0;
    if (disk_words_out) *disk_words_out = disk_words;
    if (boot_load_rc_out) *boot_load_rc_out = 0;
    if (boot_words_out) *boot_words_out = boot_words;
    if (g2_attach_rc_out) *g2_attach_rc_out = g2_rc;
    if (reason_out) *reason_out = reason;
    if (pc_out) *pc_out = PC;
    if (rb_status_out) *rb_status_out = rb_sta;
    if (shell_seen_out) *shell_seen_out = shell_seen;

    return 0; /* sim_instr() returned: operational guest is no longer live */

done_without_run:
    if (disk_load_rc_out) *disk_load_rc_out = disk_rc;
    if (disk_words_out) *disk_words_out = disk_words;
    if (boot_load_rc_out) *boot_load_rc_out = boot_rc;
    if (boot_words_out) *boot_words_out = boot_words;
    if (g2_attach_rc_out) *g2_attach_rc_out = g2_rc;
    if (reason_out) *reason_out = 0;
    if (pc_out) *pc_out = PC;
    if (rb_status_out) *rb_status_out = rb_sta;
    if (shell_seen_out) *shell_seen_out = 0;
    return 0;
}
