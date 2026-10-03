#pragma once

/* R0A9R12F1:
 * Keep this interface completely free of Open SIMH headers.
 * sim_defs.h defines BIT(nm), which collides with ESP-IDF's BIT(n) macro
 * when FreeRTOS/RISC-V headers are included in the persistence worker. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start background persistence for the already-loaded RB09 RAM image.
 * Guest-visible RB09 I/O remains memory-backed. */
int simhp4_rb09_persist_start(const char *path,
                              int32_t *filebuf,
                              size_t word_count);

/* Called by the real PDP18B RB09 write path on CPU1.
 * Performs no filesystem I/O. */
void simhp4_rb09_persist_write_word(int32_t *filebuf,
                                    uint32_t word_index,
                                    int32_t value);

uint32_t simhp4_rb09_persist_flush_count(void);
uint32_t simhp4_rb09_persist_error_count(void);

#ifdef __cplusplus
}
#endif
