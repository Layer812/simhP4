/*
 * simhP4 / RetroP4 integration
 * Copyright (c) 2026 simhP4 contributors
 *
 * Upstream SIMH-derived portions retain their original copyright notices
 * and license terms.  See LICENSES/SIMH_LICENSE.txt.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIMHP4_TYPE340_SURFACE_W 512
#define SIMHP4_TYPE340_SURFACE_H 512

int simhp4_type340_surface_init(void);

void simhp4_type340_surface_begin_frame(void);
void simhp4_type340_surface_plot(int x, int y, int intensity);
void simhp4_type340_surface_publish(void);
int simhp4_type340_surface_leave_graphics(void);

int simhp4_type340_surface_is_active(void);
uint32_t simhp4_type340_surface_generation(void);

const uint8_t *simhp4_type340_surface_acquire(uint32_t *generation_out);
void simhp4_type340_surface_release(void);

#ifdef __cplusplus
}
#endif
