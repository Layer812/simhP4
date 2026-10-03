/*
 * simhP4 / RetroP4 integration
 * Copyright (c) 2026 simhP4 contributors
 *
 * Upstream SIMH-derived portions retain their original copyright notices
 * and license terms.  See LICENSES/SIMH_LICENSE.txt.
 */
#include "simhp4_type340_surface.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define TAG "SIMHP4_TYPE340"
#define SURFACE_BYTES ((size_t)SIMHP4_TYPE340_SURFACE_W * \
                       (size_t)SIMHP4_TYPE340_SURFACE_H)

static uint8_t *s_buf[3];
static int s_front = -1;
static int s_writer = -1;
static int s_reader = -1;
static volatile int s_active;
static volatile uint32_t s_generation;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

int simhp4_type340_surface_init(void)
{
    if (s_buf[0] != NULL && s_buf[1] != NULL && s_buf[2] != NULL)
        return 1;

    for (int i = 0; i < 3; ++i) {
        if (s_buf[i] == NULL) {
            s_buf[i] = (uint8_t *)heap_caps_calloc(
                SURFACE_BYTES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (s_buf[i] == NULL) {
            printf("%s: PSRAM allocation failed buffer=%d bytes=%u\n",
                   TAG, i, (unsigned)SURFACE_BYTES);
            return 0;
        }
    }

    portENTER_CRITICAL(&s_lock);
    s_front = -1;
    s_writer = -1;
    s_reader = -1;
    s_active = 0;
    s_generation = 0;
    portEXIT_CRITICAL(&s_lock);

    printf("%s: READY triple-buffer PSRAM=%u bytes (%ux%u x3)\n",
           TAG,
           (unsigned)(SURFACE_BYTES * 3u),
           (unsigned)SIMHP4_TYPE340_SURFACE_W,
           (unsigned)SIMHP4_TYPE340_SURFACE_H);
    return 1;
}

void simhp4_type340_surface_begin_frame(void)
{
    int chosen = -1;

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < 3; ++i) {
        if (i != s_front && i != s_reader) {
            chosen = i;
            break;
        }
    }
    s_writer = chosen;
    portEXIT_CRITICAL(&s_lock);

    if (chosen >= 0)
        memset(s_buf[chosen], 0, SURFACE_BYTES);
}

void simhp4_type340_surface_plot(int x, int y, int intensity)
{
    const int writer = s_writer;
    if (writer < 0 || writer >= 3)
        return;
    if ((unsigned)x > 1023u || (unsigned)y > 1023u)
        return;

    /* Type 340 coordinates are 1024x1024 with origin at lower-left. */
    const unsigned px = ((unsigned)x) >> 1;
    const unsigned py = (1023u - (unsigned)y) >> 1;

    unsigned v = (intensity <= 0) ? 1u : (unsigned)intensity;
    if (v > 7u)
        v = 7u;

    uint8_t *dst = s_buf[writer] +
        (size_t)py * (size_t)SIMHP4_TYPE340_SURFACE_W + px;
    if (*dst < v)
        *dst = (uint8_t)v;
}

void simhp4_type340_surface_publish(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_writer >= 0) {
        s_front = s_writer;
        s_writer = -1;
        s_active = 1;
        ++s_generation;
    }
    portEXIT_CRITICAL(&s_lock);
}

int simhp4_type340_surface_leave_graphics(void)
{
    int was_active;

    portENTER_CRITICAL(&s_lock);
    was_active = s_active;
    if (s_active) {
        s_active = 0;
        ++s_generation;
    }
    portEXIT_CRITICAL(&s_lock);
    return was_active;
}

int simhp4_type340_surface_is_active(void)
{
    return __atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
}

uint32_t simhp4_type340_surface_generation(void)
{
    return __atomic_load_n(&s_generation, __ATOMIC_ACQUIRE);
}

const uint8_t *simhp4_type340_surface_acquire(uint32_t *generation_out)
{
    const uint8_t *p = NULL;

    portENTER_CRITICAL(&s_lock);
    if (s_active && s_front >= 0) {
        s_reader = s_front;
        p = s_buf[s_reader];
        if (generation_out != NULL)
            *generation_out = s_generation;
    }
    portEXIT_CRITICAL(&s_lock);

    return p;
}

void simhp4_type340_surface_release(void)
{
    portENTER_CRITICAL(&s_lock);
    s_reader = -1;
    portEXIT_CRITICAL(&s_lock);
}

/* Minimal RetroP4 backend for Open SIMH display/type340.c.
 * The exact upstream Type340 decoder remains unchanged. */
int display_init(enum display_type type, int scale, void *dptr)
{
    (void)type;
    (void)scale;
    (void)dptr;
    return (s_buf[0] != NULL && s_buf[1] != NULL && s_buf[2] != NULL) ? 1 : 0;
}

int display_point(int x, int y, int intensity, int color)
{
    (void)color;
    simhp4_type340_surface_plot(x, y, intensity);
    return 0; /* no light pen for the first physical bring-up */
}
