/* simhP4 / RetroP4 integration modifications
 * Copyright (c) 2026 simhP4 contributors
 * Original SIMH/GRAPHIC-II portions retain the upstream notices below.
 */
/*
 * wrap, or clip?
 * skip vector points when scaling?
 * are scaled character sizes right??
 */

/*
 * $Id: type340.c,v 1.6 2005/01/14 18:58:00 phil Exp $
 * Simulator Independent DEC Type 340 Graphic Display Processor Simulation
 * Phil Budne <phil@ultimate.com>
 * September 20, 2003
 * from vt11.c
 *
 * First displayed PDP-6/10 SPCWAR Feb 2018!
 *
 * The Type 340 was used on the PDP-{4,6,7,9,10}
 * and used 18-bit words, with bits numbered 0 thru 17
 * (most significant to least)
 *
 * This file simulates ONLY the 340 proper
 * and not CPU specific interfacing details
 *
 * see:
 * http://www.bitsavers.org/pdf/dec/graphics/H-340_Type_340_Precision_Incremental_CRT_System_Nov64.pdf
 *
 * Initial information from DECUS 7-13:
 * http://www.bitsavers.org/pdf/dec/graphics/7-13_340_Display_Programming_Manual.pdf
 * pre-bitsavers location(!!):
 * http://www.spies.com/~aek/pdf/dec/pdp7/7-13_340displayProgMan.pdf
 *
 * NOTE!!! The 340 is an async processor, with multiple control signals
 * running in parallel.  No attempt has been made to simulate this.
 * And while it might be fun to try to implement it as a bit vector
 * of signals, and run code triggered by those signals in the next
 * service interval, BUT unless/until this is proven necessary, I'm
 * resisting that impulse (pun not intended).
 */

/*
 * Copyright (c) 2003-2018, Philip L. Budne
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name of the author shall
 * not be used in advertising or otherwise to promote the sale, use or
 * other dealings in this Software without prior written authorization
 * from the authors.
 */

/* SIMHP4_R0A9R16_GRAPHIC2_CORE
 * Recovered Bell Labs GRAPHIC-II decoder from the user's known-working SIMH.
 * Only the desktop display backend is replaced by the existing Tab5 PSRAM
 * intensity surface; instruction semantics below remain the recovered code. */
#include "simhp4_graphics2_core.h"
#include "simhp4_type340_surface.h"

#define DISPLAY_INT_MAX 7

#define BITMASK(N) (1<<(17-(N)))

/* mask for a field */
#define FIELDMASK(START,END) ((1<<((END)-(START)+1))-1)

/* extract a field */
#define GETFIELD(W,START,END) (((W)>>(17-(END)))&FIELDMASK(START,END))

/* extract a 1-bit field */
#define TESTBIT(W,B) (((W) & BITMASK(B)) != 0)

#undef DEBUG_G2
#ifdef DEBUG_G2
#include <stdio.h>
#define DEBUGF(X) printf X
#else
#define DEBUGF(X)
#endif

#define G2_BLINK 500000 /* Microseconds. */
#define G2_UNITS 1

/* put all the state in a struct "just in case" */
static struct graphics2 {
    g2word DAC;              /* Display Address Counter */
    g2word status;           /* see ST340_XXX in type340.h */
    signed short xpos, ypos;    /* 10 bits, signed (for OOB checks) */
    unsigned short xhold, yhold; /* 11 bits */
    int counter;                /* For blinking. */
    char initialized;           /* 0 before display_init */
    unsigned char lp_ena;       /* 1 bit */
    unsigned char scale;        /* multiplier: 1,2,4,8 */
    unsigned char intensity;    /* 2 bits */
    unsigned char blink;        /* 1 bit */
    unsigned char blink_on;     /* 1 bit */
} u340[G2_UNITS];

#if G2_UNITS == 1
#define UNIT(N) u340
#else
#define UNIT(N) (u340+(N))
#endif

void
g2_set_address(g2word addr)
{
    struct graphics2 *u = UNIT(0);
    u->DAC = addr & 017777;
    DEBUGF(("set DAC %06o\r\r\n", u->DAC));

    g2_rfd();                 /* ready for data */
}

void
g2_cycle(int us)
{
    struct graphics2 *u = UNIT(0);

    if (u->status & G2_DATA) {
        DEBUGF(("GRAPHICS-2: address %06o\r\n", u->DAC));
        g2word insn = g2_fetch(u->DAC);
        g2_instruction (insn);
        u->DAC = (u->DAC + 1) & 017777;
    }

    u->counter -= us;
    if (u->counter++ <= 0) {
      u->blink_on ^= 1;
      u->counter = G2_BLINK;
    }
}

g2word
g2_get_address(void)
{
    struct graphics2 *u = UNIT(0);
    DEBUGF(("GRAPHICS-2: get address %06o\r\n", u->DAC));
    return u->DAC;
}

void
g2_set_lp(g2word x)
{
    struct graphics2 *u = UNIT(0);
    DEBUGF(("GRAPHICS-2: set lp enable %o\r\n", x));
    u->lp_ena = x;
}

g2word
g2_sense(g2word flags)
{
    struct graphics2 *u = UNIT(0);
    DEBUGF(("GRAPHICS-2: sense %06o from %06o\r\n", flags, u->status));
    return u->status & flags;
}

void
g2_clear_flags(g2word flags)
{
    struct graphics2 *u = UNIT(0);
    DEBUGF(("GRAPHICS-2: clear flags %06o\r\n", flags));
    u->status &= ~flags;
}

void
g2_set_flags(g2word flags)
{
    struct graphics2 *u = UNIT(0);
    DEBUGF(("GRAPHICS-2: set flags %06o\r\n", flags));
    u->status |= flags;
}

g2word
gr2_reset(void *dptr)
{
    struct graphics2 *u = UNIT(0);
    DEBUGF(("GRAPHICS-2: reset\r\n"));
    /* Tab5 surface lifetime is owned by CPU0/main startup. */
    (void)dptr;
    u->initialized = 1;
    u->xpos = u->ypos = 0;
    u->xhold = u->yhold = 0;
    u->status = 0;
    u->scale = 1;
    u->counter = 0;
    u->blink = 0;
    u->blink_on = 0;
    g2_rfd();                /* ready for data */
    return u->status;
}

static int
point(int x, int y)
{
    struct graphics2 *u = UNIT(0);
    int i;

#ifdef TYPE340_POINT
    DEBUGF(("type340 point %d %d %d\r\r\n", x, y));
#endif

    i = DISPLAY_INT_MAX-3+u->intensity;
    if (i <= 0)
        i = 1;

    if (x < 0 || x > 1023) {
        /* XXX clip? wrap?? */
        u->status |= G2_EDGE;
        return 0;
    }
    if (y < 0 || y > 1023) {
        /* XXX clip? wrap?? */
        u->status |= G2_EDGE;
        return 0;
    }

    /* CPU1 records only guest-visible vector output into the PSRAM writer.
     * Physical presentation remains CPU0-owned.  Light pen is not present on
     * Tab5, so no synthetic LP hit is generated. */
    simhp4_type340_surface_plot(x, y, i);
    return 1;
}

static void
lpoint(int x, int y)
{
#ifdef TYPE340_LPOINT
    DEBUGF(("type340 lpoint %d %d\r\r\n", x, y));
#endif
    point(x, y);
}

/*
 * two-step algorithm, developed by Xiaolin Wu
 * from http://graphics.lcs.mit.edu/~mcmillan/comp136/Lecture6/Lines.html
 */

/*
 * The two-step algorithm takes the interesting approach of treating
 * line drawing as a automaton, or finite state machine. If one looks
 * at the possible configurations for the next two pixels of a line,
 * it is easy to see that only a finite set of possibilities exist.
 * The two-step algorithm shown here also exploits the symmetry of
 * line-drawing by simultaneously drawn from both ends towards the
 * midpoint.
 */

static void
lineTwoStep(int x0, int y0, int x1, int y1)
{
    int dy = y1 - y0;
    int dx = x1 - x0;
    int stepx, stepy;

    if (dy < 0) { dy = -dy;  stepy = -1; } else { stepy = 1; }
    if (dx < 0) { dx = -dx;  stepx = -1; } else { stepx = 1; }

    lpoint(x0,y0);
    if (dx == 0 && dy == 0)             /* following algorithm won't work */
        return;                         /* just the one dot */
    lpoint(x1, y1);
    if (dx > dy) {
        int length = (dx - 1) >> 2;
        int extras = (dx - 1) & 3;
        int incr2 = (dy << 2) - (dx << 1);
        if (incr2 < 0) {
            int c = dy << 1;
            int incr1 = c << 1;
            int d =  incr1 - dx;
            int i;

            for (i = 0; i < length; i++) {
                x0 += stepx;
                x1 -= stepx;
                if (d < 0) {                            /* Pattern: */
                    lpoint(x0, y0);
                    lpoint(x0 += stepx, y0);            /*  x o o   */
                    lpoint(x1, y1);
                    lpoint(x1 -= stepx, y1);
                    d += incr1;
                }
                else {
                    if (d < c) {                          /* Pattern: */
                        lpoint(x0, y0);                   /*      o   */
                        lpoint(x0 += stepx, y0 += stepy); /*  x o     */
                        lpoint(x1, y1);
                        lpoint(x1 -= stepx, y1 -= stepy);
                    } else {
                        lpoint(x0, y0 += stepy);        /* Pattern: */
                        lpoint(x0 += stepx, y0);        /*    o o   */
                        lpoint(x1, y1 -= stepy);        /*  x       */
                        lpoint(x1 -= stepx, y1);
                    }
                    d += incr2;
                }
            }
            if (extras > 0) {
                if (d < 0) {
                    lpoint(x0 += stepx, y0);
                    if (extras > 1) lpoint(x0 += stepx, y0);
                    if (extras > 2) lpoint(x1 -= stepx, y1);
                } else
                    if (d < c) {
                        lpoint(x0 += stepx, y0);
                        if (extras > 1) lpoint(x0 += stepx, y0 += stepy);
                        if (extras > 2) lpoint(x1 -= stepx, y1);
                    } else {
                        lpoint(x0 += stepx, y0 += stepy);
                        if (extras > 1) lpoint(x0 += stepx, y0);
                        if (extras > 2) lpoint(x1 -= stepx, y1 -= stepy);
                    }
            }
        } else {
            int c = (dy - dx) << 1;
            int incr1 = c << 1;
            int d =  incr1 + dx;
            int i;
            for (i = 0; i < length; i++) {
                x0 += stepx;
                x1 -= stepx;
                if (d > 0) {
                    lpoint(x0, y0 += stepy);            /* Pattern: */
                    lpoint(x0 += stepx, y0 += stepy);   /*      o   */
                    lpoint(x1, y1 -= stepy);            /*    o     */
                    lpoint(x1 -= stepx, y1 -= stepy);   /*  x       */
                    d += incr1;
                } else {
                    if (d < c) {
                        lpoint(x0, y0);                   /* Pattern: */
                        lpoint(x0 += stepx, y0 += stepy); /*      o   */
                        lpoint(x1, y1);                   /*  x o     */
                        lpoint(x1 -= stepx, y1 -= stepy);
                    } else {
                        lpoint(x0, y0 += stepy);        /* Pattern: */
                        lpoint(x0 += stepx, y0);        /*    o o   */
                        lpoint(x1, y1 -= stepy);        /*  x       */
                        lpoint(x1 -= stepx, y1);
                    }
                    d += incr2;
                }
            }
            if (extras > 0) {
                if (d > 0) {
                    lpoint(x0 += stepx, y0 += stepy);
                    if (extras > 1) lpoint(x0 += stepx, y0 += stepy);
                    if (extras > 2) lpoint(x1 -= stepx, y1 -= stepy);
                } else if (d < c) {
                    lpoint(x0 += stepx, y0);
                    if (extras > 1) lpoint(x0 += stepx, y0 += stepy);
                    if (extras > 2) lpoint(x1 -= stepx, y1);
                } else {
                    lpoint(x0 += stepx, y0 += stepy);
                    if (extras > 1) lpoint(x0 += stepx, y0);
                    if (extras > 2) {
                        if (d > c)
                            lpoint(x1 -= stepx, y1 -= stepy);
                        else
                            lpoint(x1 -= stepx, y1);
                    }
                }
            }
        }
    } else {
        int length = (dy - 1) >> 2;
        int extras = (dy - 1) & 3;
        int incr2 = (dx << 2) - (dy << 1);
        if (incr2 < 0) {
            int c = dx << 1;
            int incr1 = c << 1;
            int d =  incr1 - dy;
            int i;
            for (i = 0; i < length; i++) {
                y0 += stepy;
                y1 -= stepy;
                if (d < 0) {
                    lpoint(x0, y0);
                    lpoint(x0, y0 += stepy);
                    lpoint(x1, y1);
                    lpoint(x1, y1 -= stepy);
                    d += incr1;
                } else {
                    if (d < c) {
                        lpoint(x0, y0);
                        lpoint(x0 += stepx, y0 += stepy);
                        lpoint(x1, y1);
                        lpoint(x1 -= stepx, y1 -= stepy);
                    } else {
                        lpoint(x0 += stepx, y0);
                        lpoint(x0, y0 += stepy);
                        lpoint(x1 -= stepx, y1);
                        lpoint(x1, y1 -= stepy);
                    }
                    d += incr2;
                }
            }
            if (extras > 0) {
                if (d < 0) {
                    lpoint(x0, y0 += stepy);
                    if (extras > 1) lpoint(x0, y0 += stepy);
                    if (extras > 2) lpoint(x1, y1 -= stepy);
                } else
                    if (d < c) {
                        lpoint(x0, y0 += stepy);
                        if (extras > 1) lpoint(x0 += stepx, y0 += stepy);
                        if (extras > 2) lpoint(x1, y1 -= stepy);
                    } else {
                        lpoint(x0 += stepx, y0 += stepy);
                        if (extras > 1) lpoint(x0, y0 += stepy);
                        if (extras > 2) lpoint(x1 -= stepx, y1 -= stepy);
                    }
            }
        } else {
            int c = (dx - dy) << 1;
            int incr1 = c << 1;
            int d =  incr1 + dy;
            int i;
            for (i = 0; i < length; i++) {
                y0 += stepy;
                y1 -= stepy;
                if (d > 0) {
                    lpoint(x0 += stepx, y0);
                    lpoint(x0 += stepx, y0 += stepy);
                    lpoint(x1 -= stepx, y1);
                    lpoint(x1 -= stepx, y1 -= stepy);
                    d += incr1;
                } else {
                    if (d < c) {
                        lpoint(x0, y0);
                        lpoint(x0 += stepx, y0 += stepy);
                        lpoint(x1, y1);
                        lpoint(x1 -= stepx, y1 -= stepy);
                    } else {
                        lpoint(x0 += stepx, y0);
                        lpoint(x0, y0 += stepy);
                        lpoint(x1 -= stepx, y1);
                        lpoint(x1, y1 -= stepy);
                    }
                    d += incr2;
                }
            }
            if (extras > 0) {
                if (d > 0) {
                    lpoint(x0 += stepx, y0 += stepy);
                    if (extras > 1) lpoint(x0 += stepx, y0 += stepy);
                    if (extras > 2) lpoint(x1 -= stepx, y1 -= stepy);
                } else if (d < c) {
                    lpoint(x0, y0 += stepy);
                    if (extras > 1) lpoint(x0 += stepx, y0 += stepy);
                    if (extras > 2) lpoint(x1, y1 -= stepy);
                } else {
                    lpoint(x0 += stepx, y0 += stepy);
                    if (extras > 1) lpoint(x0, y0 += stepy);
                    if (extras > 2) {
                        if (d > c)
                            lpoint(x1 -= stepx, y1 -= stepy);
                        else
                            lpoint(x1, y1 -= stepy);
                    }
                }
            }
        }
    }
} /* lineTwoStep */

/* here in VECTOR & VCONT modes */
static int
vector(int i, int sy, int dy, int sx, int dx)
{
    struct graphics2 *u = UNIT(0);
    int x0, y0, x1, y1;
    int flags = 0;

    DEBUGF(("v i%d y%c%d x%c%d\r\r\n", i,
            (sy ? '-' : '+'), dy,
            (sx ? '-' : '+'), dx));
    x0 = u->xpos;
    y0 = u->ypos;

    if (sx) {
        x1 = x0 - dx * u->scale;
        if (x1 < 0) {
            x1 = 0;
            flags = G2_EDGE;
        }
    }
    else {
        x1 = x0 + dx * u->scale;
        if (x1 > 1023) {
            x1 = 1023;
            flags = G2_EDGE;
        }
    }

    if (sy) {
        y1 = y0 - dy * u->scale;
        if (y1 < 0) {
            y1 = 0;
            flags |= G2_EDGE;
        }
    }
    else {
        y1 = y0 + dy * u->scale;
        if (y1 > 1023) {
            y1 = 1023;
            flags |= G2_EDGE;
        }
    }

    DEBUGF(("vector i%d (%d,%d) to (%d,%d)\r\r\n", i, x0, y0, x1, y1));
    if (i && (!u->blink || u->blink_on))
        lineTwoStep(x0, y0, x1, y1);

    u->xpos = x1;
    u->ypos = y1;
    u->status |= flags;                 /* ?? */
    return flags;
}

static int
ipoint(int byte)
{
    static int xtab[] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static int ytab[] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    struct graphics2 *u = UNIT(0);
    int dx, dy;
    int n, i;

    n = (byte >> 4) & 7;
    dx = xtab[byte & 7] * u->scale;
    dy = ytab[byte & 7] * u->scale;
        
    DEBUGF(("G-2 incremental %03o: %d %d %d %svisible\r\n", byte, n, dx, dy, (byte & 010) ? "" : "in"));

    for (i = 0; i <= n; i++) {
      if ((byte & 010) && (!u->blink || u->blink_on))
        point(u->xpos, u->ypos);

      u->xpos += dx;
      u->ypos += dy;
      if (u->xpos < 0) {
        u->xpos = 0;
        u->status |= G2_EDGE;
      } else if (u->xpos > 1023) {
        u->xpos = 1023;
        u->status |= G2_EDGE;
      }

      if (u->ypos < 0) {
        u->ypos = 0;
        u->status |= G2_EDGE;
      } else if (u->ypos > 1023) {
        u->ypos = 1023;
        u->status |= G2_EDGE;
      }
    }

    return 0;
}

#include "simhp4_g2chars.inc"

static void
g2char (int c)
{
  /* R0A9R16A: GCC -O2 cannot prove the first loop iteration assigns x.
   * The loop always assigns x at i==0 before use, so zero-initialization is
   * warning-only hardening and does not change recovered GRAPHIC-II semantics. */
  g2word x = 0;
  int i, j, n;
  struct graphics2 *u = UNIT(0);

  if (c == 010) {
    DEBUGF(("G-2 back space\r\n"));
    u->xpos -= 13;
    if (u->xpos < 0) {
      u->xpos = 0;
      u->status |= G2_EDGE;
    }
    return;
  } else if (c == 012) {
    DEBUGF(("G-2 newline\r\n"));
    u->xpos = 0;
    u->ypos -= 32; /* complete and utter guess */
    if (u->ypos < 0) {
      u->ypos = 0;
      u->status |= G2_EDGE;
    }
    return;
  } else if (c < 040 || c > 0177)
  {
    DEBUGF(("G-2 character %04o ignored\r\n", c));
    return;
  }

  n = (chargen[c-040][0] >> 10) & 077;
  DEBUGF(("G-2 character '%c', %d bytes\r\n", c, n));
  for (i = 0, j = 1; i < n; i++) {
    if ((i % 3) == 0)
      x = chargen[c-040][j++];
    ipoint ((x >> 12) & 077);
    x <<= 6;
  }
}

/*
 * Execute one GRAPHICS-2 instruction.
 * returns status word
 */
g2word
g2_instruction(g2word inst)
{
    struct graphics2 *u = UNIT(0);
    signed short val, xval, yval;

    switch ((inst & 0777777) >> 14) {
    case 000: /* character */
      DEBUGF(("GRAPHICS-2: character %03o %03o\r\n",
              (inst >> 9) & 0177, inst & 0177));
      /* SIMHP4_R0A9R17_PACKED_CHAR_9BIT_FIX
       * UNIX V0 as7 packs <x as ASCII << 9 in an 18-bit word.
       * The low character remains in bits 0..6. */
      g2char ((inst >> 9) & 0177);
      g2char (inst & 0177);
      break;
    case 001: /* parameter */
      DEBUGF(("GRAPHICS-2: parameter %06o\r\n", inst));
      if (inst & 0020000)
        u->blink = (inst >> 12) & 1;
      if (inst & 0004000)
        u->lp_ena = (inst >> 10) & 1;
      /* TODO: more bits here */
      if (inst & 040)
        u->scale = 1 << ((inst & 030) >> 3);
      if (inst & 4)
        u->intensity = inst & 3;
      break;
    case 002: /* long vector */
      DEBUGF(("GRAPHICS-2: long vector %06o\r\n", inst));
      val = inst & 03777;
      if (inst & 04000)
        u->yhold = val;
      else
        u->xhold = val;
      switch ((inst >> 12) & 3) {
      case 0:
        break;
      case 1:
        vector(0, u->yhold & 02000, u->yhold & 01777,
               u->xhold & 02000, u->xhold & 01777);
        u->xhold = u->yhold = 0;
        break;
      case 2:
        vector(1, u->yhold & 02000, u->yhold & 01777,
               u->xhold & 02000, u->xhold & 01777);
        u->xhold = u->yhold = 0;
        break;
      case 3:
        if (u->xhold & 02000)
          u->xpos -= u->xhold & 01777;
        else
          u->xpos += u->xhold & 01777;
        if (u->yhold & 02000)
          u->ypos -= u->yhold & 01777;
        else
          u->ypos += u->yhold & 01777;
        point(u->xpos, u->ypos);
        u->xhold = u->yhold = 0;
        break;
      }
      break;
    case 003: /* x-y */
      DEBUGF(("GRAPHICS-2: point %06o\r\n", inst));
      if (inst & 04000)
        u->ypos = inst & 01777;
      else
        u->xpos = inst & 01777;
      break;
    case 004: /* short vector */
      DEBUGF(("GRAPHICS-2: short vector %06o\r\n", inst));
      xval = (inst & 07700) >> 6;
      yval = inst & 077;
      switch ((inst >> 12) & 3) {
      case 0:
        break;
      case 1:
        vector(0, yval & 040, yval & 037, xval & 040, xval & 037);
        break;
      case 2:
        vector(1, yval & 040, yval & 037, xval & 040, xval & 037);
        break;
      case 3:
        if (xval & 040)
          u->xpos -= xval;
        else
          u->xpos += xval;
        if (yval & 040)
          u->ypos -= yval;
        else
          u->ypos += yval;
        point(u->xpos, u->ypos);
        break;
      }
      break;
    case 005: /* incremental */
      DEBUGF(("GRAPHICS-2: incremental %06o\r\n", inst));
      ipoint((inst >> 7) & 0177);
      ipoint(inst & 0177);
      break;
    case 006: /* slave */
      DEBUGF(("GRAPHICS-2: slave %06o\r\n", inst));
      break;
    case 010: case 011: case 012: case 013: /* trap */
    case 014: case 015: case 016: case 017:
      DEBUGF(("GRAPHICS-2: trap %06o\r\n", inst));
      u->status &= ~G2_DATA;
      u->status |= G2_TRAP;
      break;
    }

    if (u->status & G2_STEP)
      u->status &= ~G2_DATA;

    if (u->status & G2_DATA)
        g2_rfd();                    /* ready for data */

    return u->status;
}

g2word
g2_get_flags(void)
{
    struct graphics2 *u = UNIT(0);
    DEBUGF(("GRAPHICS-2: get flags %06o\r\n", u->status));
    return u->status;
}
