#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CPU0 host side. */
int simhp4_remote_tty_start(void);
int simhp4_remote_tty_network_ready(void);
int simhp4_remote_tty_client_connected(void);
uint32_t simhp4_remote_tty_rx_drop_count(void);
uint32_t simhp4_remote_tty_tx_drop_count(void);

/* R0A9R11 CPU0 UI mirror of the actual primary TTY stream.
 * snapshot() returns oldest-to-newest bytes currently retained. */
uint32_t simhp4_remote_tty_console_generation(void);
size_t simhp4_remote_tty_console_snapshot(uint8_t *dst, size_t cap);

/* CPU1-facing byte queue API.  Deliberately contains no SIMH types or SCPE constants. */
int simhp4_remote_tty_guest_getc(uint8_t *out);
int simhp4_remote_tty_guest_putc(uint8_t ch);

/* R0A9R8 read-only tracing hooks.  Still deliberately free of SIMH types. */
void simhp4_remote_tty_trace_tti_delivery(uint8_t raw_host_ch, uint16_t converted_guest_ch);
void simhp4_remote_tty_trace_tto_output(uint8_t ch);

#ifdef __cplusplus
}
#endif


