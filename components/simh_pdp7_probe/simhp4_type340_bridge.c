#include "pdp18b_defs.h"
#include "type340.h"

#include <stdio.h>

/* SIMH-side bridge only.  No ESP-IDF/FreeRTOS headers are included here. */
extern void simhp4_type340_surface_begin_frame(void);
extern void simhp4_type340_surface_publish(void);

extern int32 *M;

#define SIMHP4_T340_MAX_WORDS 8192u

static unsigned s_announced;
static unsigned s_frame_count;

ty340word ty340_fetch(ty340word addr)
{
    return (ty340word)(M[addr & 017777u] & 0777777u);
}

void ty340_store(ty340word addr, ty340word value)
{
    M[addr & 017777u] = (int32)(value & 0777777u);
}

void ty340_lp_int(ty340word x, ty340word y)
{
    (void)x;
    (void)y;
}

void ty340_rfd(void)
{
    /* The bridge feeds display words synchronously. */
}

int simhp4_type340_capture(uint32 addr)
{
    ty340word status = 0;
    uint32 a = addr & 017777u;
    unsigned words;

    simhp4_type340_surface_begin_frame();

    /* A PDP-7 G2 BEG starts the captured program from a clean Type340 state.
     * The user's own display list then supplies mode/scale/intensity/position. */
    (void)ty340_reset(NULL);

    for (words = 0; words < SIMHP4_T340_MAX_WORDS; ++words) {
        const ty340word inst = (ty340word)(M[a] & 0777777u);
        status = ty340_instruction(inst);
        a = (a + 1u) & 017777u; /* PDP-7 8K address space */

        if (status & ST340_STOPPED)
            break;
    }

    simhp4_type340_surface_publish();
    ++s_frame_count;

    if (!s_announced) {
        s_announced = 1;
        printf("SIMHP4_TYPE340: ACTIVE start=%06o first=%06o words=%u status=%06o\n",
               (unsigned)(addr & 017777u),
               (unsigned)(M[addr & 017777u] & 0777777u),
               words + 1u,
               (unsigned)status);
        printf("SIMHP4_TYPE340: controls while graphics active: keys 1..8 -> PB0..PB7\n");
    }

    if (!(status & ST340_STOPPED) && ((s_frame_count & 63u) == 1u)) {
        printf("SIMHP4_TYPE340: WARN display list word limit start=%06o status=%06o\n",
               (unsigned)(addr & 017777u),
               (unsigned)status);
    }

    return (int)status;
}
