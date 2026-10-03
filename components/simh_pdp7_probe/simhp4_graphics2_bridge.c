/*
 * simhP4 / RetroP4 integration
 * Copyright (c) 2026 simhP4 contributors
 *
 * Upstream SIMH-derived portions retain their original copyright notices
 * and license terms.  See LICENSES/SIMH_LICENSE.txt.
 */
#include "pdp18b_defs.h"
#include "simhp4_graphics2_core.h"
#include "simhp4_type340_surface.h"

#include <stdio.h>

/* SIMHP4_R0A9R16_GRAPHIC2_BRIDGE
 * CPU1 owns recovered GRAPHIC-II guest semantics.  CPU0 remains the physical
 * presenter; this file never calls M5GFX, SD, Wi-Fi, or FreeRTOS display code. */

extern int32 *M;

#define SIMHP4_G2_MAX_WORDS 8192u

static unsigned s_inited;
static unsigned s_announced;
static unsigned s_frame_count;

g2word g2_fetch(g2word addr)
{
    return (g2word)(M[addr & 017777u] & 0777777u);
}

void g2_store(g2word addr, g2word value)
{
    M[addr & 017777u] = (int32)(value & 0777777u);
}

void g2_lp_int(g2word x, g2word y)
{
    (void)x;
    (void)y;
}

void g2_rfd(void)
{
    /* The embedded bridge executes a complete display list synchronously
     * when UNIX issues BEG.  There is no host data-request handshake. */
}

static void simhp4_graphics2_init_once(void)
{
    if (!s_inited) {
        (void)gr2_reset(NULL);
        s_inited = 1;
    }
}

int simhp4_graphics2_capture(uint32 addr)
{
    unsigned words = 0;
    g2word status;

    simhp4_graphics2_init_once();
    simhp4_type340_surface_begin_frame();

    /* Exact recovered BEG semantics: CDF + WDA + ECR + DATA. */
    g2_clear_flags(G2_DISPLAY_FLAGS);
    g2_set_address((g2word)(addr & 017777u));
    g2_clear_flags(G2_STEP);
    g2_set_flags(G2_DATA);

    while ((g2_get_flags() & G2_DATA) && words < SIMHP4_G2_MAX_WORDS) {
        /* The recovered desktop device services one display word per 100 us.
         * Preserve that internal blink/processor time accounting without
         * sleeping CPU1 in real host time. */
        g2_cycle(100);
        ++words;
    }

    status = g2_get_flags();
    simhp4_type340_surface_publish();
    ++s_frame_count;

    if (!s_announced) {
        s_announced = 1;
        printf("SIMHP4_GRAPHIC2_R16: ACTIVE start=%06o first=%06o words=%u status=%06o source=recovered-graphics2\n",
               (unsigned)(addr & 017777u),
               (unsigned)(M[addr & 017777u] & 0777777u),
               words, (unsigned)(status & 0777777u));
    }

    if ((status & G2_DATA) && ((s_frame_count & 63u) == 1u)) {
        printf("SIMHP4_GRAPHIC2_R16: WARN word-limit start=%06o dac=%06o status=%06o\n",
               (unsigned)(addr & 017777u),
               (unsigned)(g2_get_address() & 017777u),
               (unsigned)(status & 0777777u));
    }

    return (int)(status & 0777777u);
}

int32 simhp4_graphics2_iot05(int32 pulse, int32 dat)
{
    simhp4_graphics2_init_once();

    if (pulse & 001)
        g2_clear_flags(G2_DISPLAY_FLAGS);       /* CDF */
    if (pulse & 002)
        g2_set_address((g2word)dat);            /* WDA */
    if (pulse & 004)
        g2_clear_flags(G2_STEP);                /* ECR */
    if (pulse & 020)
        g2_set_flags(G2_STEP);                  /* ESS */
    if (pulse & 040)
        g2_set_flags(G2_DATA);                  /* CON/BEG */

    return dat;
}

int32 simhp4_graphics2_iot06(int32 pulse, int32 dat)
{
    /* Recovered implementation leaves WDBC/LDB/WDBS unused by UNIX V0. */
    (void)pulse;
    return dat;
}

int32 simhp4_graphics2_iot07(int32 pulse, int32 dat)
{
    simhp4_graphics2_init_once();

    if (pulse & 001) {
        if (pulse & 002)
            g2_set_lp(0);                       /* DLP */
        else
            g2_set_lp(1);                       /* ELP */
    }
    if (pulse & 002) {
        if (pulse & 040)
            g2_clear_flags(G2_EDGE | G2_REDGE | G2_LEDGE |
                           G2_TEDGE | G2_BEDGE); /* RAEF */
        else
            g2_clear_flags(G2_LP);              /* RLPE/RLPD */
    }
    return dat;
}

int32 simhp4_graphics2_iot10(int32 pulse, int32 dat)
{
    simhp4_graphics2_init_once();

    if (pulse & 002) {
        if (pulse & 020) {
            /* LPM is not used by the recovered UNIX V0 path. */
        } else if (pulse & 040) {
            dat |= (int32)g2_get_flags();        /* LDS */
        } else {
            dat |= (int32)g2_get_address();      /* LDA */
        }
    }
    return dat;
}
