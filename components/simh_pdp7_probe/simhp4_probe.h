#pragma once

#include <stdint.h>

#define SIMHP4_SIMH_COMMIT "87eb7d5e96f9ce0ee6ac183e20160e5c486b0712"

#ifdef __cplusplus
extern "C" {
#endif

const char *simhp4_pdp7_compile_probe_version(void);
const char *simhp4_pdp7_machine_name(void);
int simhp4_pdp7_linked_device_count(void);

void simhp4_runtime_prepare(void);
const char *simhp4_runtime_profile(void);

void simhp4_tty_capture_reset(void);
const char *simhp4_tty_capture_get(void);
void simhp4_console_watch_begin(const char *needle);
int simhp4_console_watch_hit(void);

/* Primary TTI queue retained for the SIMH console path. */
int simhp4_tti_input_init(void);
void simhp4_tti_input_reset(void);
int simhp4_tti_host_enqueue(uint8_t ch);
uint32_t simhp4_tti_input_drop_count(void);

/* R0A3 local GRAPHICS-2 transport.
 *
 * CPU0 physical side:
 *   USB HID -> simhp4_g2_host_enqueue()
 *   simhp4_g2_host_dequeue() -> Tab5 display
 *
 * CPU1 guest side:
 *   unmodified pdp18b_g2tty.c
 *   tmxr_getc_ln()/tmxr_putc_ln() local shim
 */
int simhp4_g2_local_init(void);
void simhp4_g2_local_reset(void);
int simhp4_g2_host_enqueue(uint8_t ch);
int simhp4_g2_host_dequeue(uint8_t *ch);
int simhp4_g2_host_dequeue_wait(uint8_t *ch);
int simhp4_g2_host_dequeue_timed(uint8_t *ch, uint32_t timeout_ms);
uint32_t simhp4_g2_input_drop_count(void);
uint32_t simhp4_g2_output_drop_count(void);
void simhp4_g2_watch_begin(const char *needle);
int simhp4_g2_watch_hit(void);
const char *simhp4_g2_capture_get(void);

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
                                      int *shell_seen_out);

#ifdef __cplusplus
}
#endif
