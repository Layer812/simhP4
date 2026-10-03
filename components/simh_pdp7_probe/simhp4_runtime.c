#include "pdp18b_defs.h"
#include "sim_tmxr.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* scp.h intentionally maps stdio calls to SCP wrappers.  This embedded file
 * supplies those wrappers and uses libc vfprintf/vprintf underneath. */
#ifdef fprintf
#undef fprintf
#endif
#ifdef fputs
#undef fputs
#endif
#ifdef fputc
#undef fputc
#endif

#define SIMHP4_GUEST_IPS 960000.0

/* Minimal SCP globals required by the PDP-7 execution core. */
UNIT *sim_clock_queue = QUEUE_LIST_END;
int32 sim_interval = NOQUEUE_WAIT;
int32 sim_switches = 0;
int32 sim_switch_number = 0;
volatile t_bool sim_is_running = FALSE;
t_bool sim_processing_event = FALSE;
volatile t_bool stop_cpu = FALSE;
uint32 sim_brk_summ = 0;
uint32 sim_brk_types = 0;
uint32 sim_brk_dflt = 0;
uint32 sim_brk_match_type = 0;
t_addr sim_brk_match_addr = 0;
BRKTYPTAB *sim_brk_type_desc = NULL;
DEVICE *sim_dflt_dev = NULL;
DEVICE *sim_dfdev = NULL;
UNIT *sim_dfunit = NULL;
FILE *sim_deb = NULL;
FILE *sim_log = NULL;
FILEREF *sim_deb_ref = NULL;
FILEREF *sim_log_ref = NULL;
int32 sim_deb_switches = 0;
size_t sim_deb_buffer_size = 0;
char *sim_deb_buffer = NULL;
size_t sim_debug_buffer_offset = 0;
size_t sim_debug_buffer_inuse = 0;
DEVICE **sim_internal_devices = NULL;
uint32 sim_internal_device_count = 0;
t_value *sim_eval = NULL;

/* R0A1C1: Open SIMH timer idling defaults to disabled.  RetroP4 keeps it
 * disabled here: ESP32-P4 is not host-throttled to PDP-7 wallclock speed. */
t_bool sim_idle_enab = FALSE;

/* Console defaults: ^E is the traditional SIMH interrupt character. */
int32 sim_int_char = 005;
int32 sim_brk_char = 0;
int32 sim_del_char = 0177;
int32 sim_tt_pchar =
    (1 << 7)  | /* BEL */
    (1 << 8)  | /* BS  */
    (1 << 9)  | /* HT  */
    (1 << 10) | /* LF  */
    (1 << 11) | /* VT  */
    (1 << 12) | /* FF  */
    (1 << 13) | /* CR  */
    (1 << 27);  /* ESC */
t_bool sim_signaled_int_char = FALSE;
uint32 sim_last_poll_kbd_time = 0;
int32 sim_rem_cmd_active_line = -1;

static int32 simhp4_noqueue_time = NOQUEUE_WAIT;
static double simhp4_sim_time = 0.0;

#define SIMHP4_TTY_CAPTURE_MAX 4096
static char simhp4_tty_capture[SIMHP4_TTY_CAPTURE_MAX];
static size_t simhp4_tty_capture_len = 0;
static const char *simhp4_console_watch_needle = NULL;
static int simhp4_console_watch_seen = 0;

/* R0A2: host input is a cross-core byte FIFO.
 * Producer: CPU0 USB HID tasks.
 * Consumer: CPU1 Open SIMH tti_svc() via sim_poll_kbd(). */
#define SIMHP4_TTI_INPUT_DEPTH 128
static QueueHandle_t simhp4_tti_input_queue = NULL;
static volatile uint32_t simhp4_tti_input_drops = 0;

void simhp4_tty_capture_reset(void)
{
    simhp4_tty_capture_len = 0;
    simhp4_tty_capture[0] = '\0';
}

const char *simhp4_tty_capture_get(void)
{
    return simhp4_tty_capture;
}

void simhp4_console_watch_begin(const char *needle)
{
    simhp4_console_watch_needle = needle;
    simhp4_console_watch_seen = 0;
}

int simhp4_console_watch_hit(void)
{
    return simhp4_console_watch_seen;
}

int simhp4_tti_input_init(void)
{
    if (simhp4_tti_input_queue == NULL)
        simhp4_tti_input_queue = xQueueCreate(SIMHP4_TTI_INPUT_DEPTH, sizeof(uint8_t));
    return (simhp4_tti_input_queue != NULL) ? 1 : 0;
}

void simhp4_tti_input_reset(void)
{
    if (simhp4_tti_input_queue != NULL)
        xQueueReset(simhp4_tti_input_queue);
    simhp4_tti_input_drops = 0;
}

int simhp4_tti_host_enqueue(uint8_t ch)
{
    if (simhp4_tti_input_queue == NULL)
        return 0;
    if (xQueueSend(simhp4_tti_input_queue, &ch, 0) != pdTRUE) {
        ++simhp4_tti_input_drops;
        return 0;
    }
    return 1;
}

uint32_t simhp4_tti_input_drop_count(void)
{
    return simhp4_tti_input_drops;
}

extern DEVICE *sim_devices[];

static void simhp4_sync_queue_time(void)
{
    int32 prior;
    if (sim_clock_queue == QUEUE_LIST_END) {
        simhp4_noqueue_time = sim_interval;
        return;
    }
    prior = sim_clock_queue->time;
    simhp4_sim_time += (double)(prior - sim_interval);
    sim_clock_queue->time = sim_interval;
}

/* R0A1E: RB09 rotational position uses SIMH global guest time.
 * Keep it in the same instruction-time domain as the existing delta queue. */
double sim_gtime(void)
{
    simhp4_sync_queue_time();
    return simhp4_sim_time;
}

static int32 simhp4_usec_to_instructions(double usecs)
{
    double raw;
    if (usecs <= 0.0)
        return 0;
    raw = (usecs * SIMHP4_GUEST_IPS) / 1000000.0;
    if (raw >= 2147483646.0)
        return 2147483646;
    if (raw < 1.0)
        return 1;
    return (int32)(raw + 0.5);
}

/* Event queue: delta queue semantics taken from SIMH's SCP model, without the
 * desktop async/timer thread layers.  CPU1 remains the single guest owner. */
t_bool sim_is_active(UNIT *uptr)
{
    return (uptr && uptr->next != NULL) ? TRUE : FALSE;
}

t_stat _sim_activate(UNIT *uptr, int32 event_time)
{
    UNIT *cptr, *prvptr;
    int32 accum;

    if (uptr == NULL)
        return SCPE_IERR;
    if (sim_is_active(uptr))
        return SCPE_OK;
    if (event_time < 0)
        event_time = 0;

    simhp4_sync_queue_time();
    prvptr = NULL;
    accum = 0;
    for (cptr = sim_clock_queue; cptr != QUEUE_LIST_END; cptr = cptr->next) {
        if (event_time < (accum + cptr->time))
            break;
        accum += cptr->time;
        prvptr = cptr;
    }
    if (prvptr == NULL) {
        cptr = uptr->next = sim_clock_queue;
        sim_clock_queue = uptr;
    } else {
        cptr = uptr->next = prvptr->next;
        prvptr->next = uptr;
    }
    uptr->time = event_time - accum;
    if (cptr != QUEUE_LIST_END)
        cptr->time -= uptr->time;
    sim_interval = sim_clock_queue->time;
    return SCPE_OK;
}

t_stat sim_activate(UNIT *uptr, int32 event_time)
{
    return _sim_activate(uptr, event_time);
}

t_stat sim_activate_abs(UNIT *uptr, int32 event_time)
{
    sim_cancel(uptr);
    return _sim_activate(uptr, event_time);
}

t_stat sim_cancel(UNIT *uptr)
{
    UNIT *cptr, *next;
    int32 removed_time;

    if (uptr == NULL)
        return SCPE_IERR;
    if (!sim_is_active(uptr))
        return SCPE_OK;

    simhp4_sync_queue_time();
    removed_time = uptr->time;
    next = uptr->next;

    if (sim_clock_queue == uptr) {
        sim_clock_queue = next;
    } else {
        for (cptr = sim_clock_queue; cptr != QUEUE_LIST_END; cptr = cptr->next) {
            if (cptr->next == uptr) {
                cptr->next = next;
                break;
            }
        }
    }
    if (next != QUEUE_LIST_END)
        next->time += removed_time;

    uptr->next = NULL;
    uptr->time = 0;
    uptr->usecs_remaining = 0;

    if (sim_clock_queue != QUEUE_LIST_END)
        sim_interval = sim_clock_queue->time;
    else
        sim_interval = simhp4_noqueue_time = NOQUEUE_WAIT;
    return SCPE_OK;
}

t_stat sim_process_event(void)
{
    t_stat reason = SCPE_OK;

    if (stop_cpu) {
        stop_cpu = FALSE;
        return SCPE_STOP;
    }

    simhp4_sync_queue_time();
    if (sim_interval > 0)
        return SCPE_OK;
    if (sim_clock_queue == QUEUE_LIST_END) {
        sim_interval = simhp4_noqueue_time = NOQUEUE_WAIT;
        return SCPE_OK;
    }

    sim_processing_event = TRUE;
    do {
        UNIT *uptr = sim_clock_queue;
        int32 late = sim_interval; /* zero or negative */

        sim_clock_queue = uptr->next;
        uptr->next = NULL;
        uptr->time = 0;

        if (sim_clock_queue != QUEUE_LIST_END)
            sim_interval = sim_clock_queue->time + late;
        else
            sim_interval = simhp4_noqueue_time = NOQUEUE_WAIT;

        if (uptr->action != NULL)
            reason = uptr->action(uptr);
        else
            reason = SCPE_OK;

        if ((reason == SCPE_OK) && stop_cpu) {
            stop_cpu = FALSE;
            reason = SCPE_STOP;
        }
    } while ((reason == SCPE_OK) &&
             (sim_interval <= 0) &&
             (sim_clock_queue != QUEUE_LIST_END));

    sim_processing_event = FALSE;
    return reason;
}

/* The PDP-7 standard clock is scheduled in guest instruction time.  16,000
 * instructions per 60 Hz tick corresponds to 960 kIPS.  No host busy-wait or
 * 7 MHz-style throttling is introduced. */
t_stat sim_activate_after(UNIT *uptr, uint32 usecs_walltime)
{
    return sim_activate(uptr, simhp4_usec_to_instructions((double)usecs_walltime));
}

t_stat sim_activate_after_d(UNIT *uptr, double usecs_walltime)
{
    return sim_activate(uptr, simhp4_usec_to_instructions(usecs_walltime));
}

t_stat _sim_activate_after(UNIT *uptr, double usecs_walltime)
{
    return sim_activate_after_d(uptr, usecs_walltime);
}

t_stat sim_activate_after_abs(UNIT *uptr, uint32 usecs_walltime)
{
    sim_cancel(uptr);
    return sim_activate_after(uptr, usecs_walltime);
}

t_stat sim_activate_after_abs_d(UNIT *uptr, double usecs_walltime)
{
    sim_cancel(uptr);
    return sim_activate_after_d(uptr, usecs_walltime);
}

t_stat _sim_activate_after_abs(UNIT *uptr, double usecs_walltime)
{
    return sim_activate_after_abs_d(uptr, usecs_walltime);
}

t_stat sim_clock_coschedule(UNIT *uptr, int32 interval)
{
    return sim_activate(uptr, interval);
}

t_stat sim_clock_coschedule_abs(UNIT *uptr, int32 interval)
{
    return sim_activate_abs(uptr, interval);
}

t_stat sim_register_clock_unit(UNIT *uptr)
{
    (void)uptr;
    return SCPE_OK;
}

int32 sim_rtc_init(int32 time)
{
    return (time > 0) ? time : 1;
}

int32 sim_rtc_calb(uint32 ticksper)
{
    if (ticksper == 0)
        return 1;
    return (int32)((SIMHP4_GUEST_IPS + ((double)ticksper / 2.0)) / (double)ticksper);
}

/* Minimal local console.  R0A2 replaces these endpoints with the Tab5
 * keyboard/text terminal queues. */
t_stat sim_poll_kbd(void)
{
    uint8_t ch = 0;

    if (simhp4_tti_input_queue == NULL)
        return SCPE_OK;

    if (xQueueReceive(simhp4_tti_input_queue, &ch, 0) != pdTRUE)
        return SCPE_OK;

    return (t_stat)(SCPE_KFLAG | (int32)ch);
}

t_stat sim_putchar(int32 c)
{
    int ch = c & 0xff;

    if (simhp4_tty_capture_len + 1 < SIMHP4_TTY_CAPTURE_MAX) {
        simhp4_tty_capture[simhp4_tty_capture_len++] = (char)ch;
        simhp4_tty_capture[simhp4_tty_capture_len] = '\0';
    }

    putchar(ch);
    fflush(stdout);

    if (!simhp4_console_watch_seen &&
        (simhp4_console_watch_needle != NULL) &&
        (simhp4_console_watch_needle[0] != '\0') &&
        (strstr(simhp4_tty_capture, simhp4_console_watch_needle) != NULL)) {
        simhp4_console_watch_seen = 1;
        stop_cpu = TRUE;
    }

    return SCPE_OK;
}

t_stat sim_putchar_s(int32 c)
{
    return sim_putchar(c);
}

int32 sim_tt_inpcvt(int32 c, uint32 mode)
{
    uint32 md = mode & TTUF_M_MODE;
    if (md == TTUF_MODE_8B)
        return c & 0377;
    c &= 0177;
    if (md == TTUF_MODE_UC) {
        if ((c >= 'a') && (c <= 'z'))
            c -= ('a' - 'A');
        if (mode & TTUF_KSR)
            c |= 0200;
    }
    return c;
}

int32 sim_tt_outcvt(int32 c, uint32 mode)
{
    uint32 md = mode & TTUF_M_MODE;
    if (md == TTUF_MODE_8B)
        return c & 0377;
    c &= 0177;
    if (md == TTUF_MODE_UC) {
        if ((c >= 'a') && (c <= 'z'))
            c -= ('a' - 'A');
        if ((mode & TTUF_KSR) && (c >= 0140))
            return -1;
    }
    if (((md == TTUF_MODE_UC) || (md == TTUF_MODE_7P)) &&
        ((c == 0177) || ((c < 040) && !((sim_tt_pchar >> c) & 1))))
        return -1;
    return c;
}

t_stat tmxr_set_console_units(UNIT *rxuptr, UNIT *txuptr)
{
    (void)rxuptr;
    (void)txuptr;
    return SCPE_OK;
}

/* Minimal SCP helpers used by cpu_reset/build_dev_tab. */
REG *find_reg(CONST char *cptr, CONST char **optr, DEVICE *dptr)
{
    REG *rptr;
    size_t len = 0;
    if ((cptr == NULL) || (dptr == NULL) || (dptr->registers == NULL))
        return NULL;
    while ((cptr[len] >= 'A' && cptr[len] <= 'Z') ||
           (cptr[len] >= 'a' && cptr[len] <= 'z') ||
           (cptr[len] >= '0' && cptr[len] <= '9') ||
           cptr[len] == '*' || cptr[len] == '_' || cptr[len] == '.')
        ++len;
    for (rptr = dptr->registers; rptr->name != NULL; ++rptr) {
        if ((strlen(rptr->name) == len) && (strncmp(cptr, rptr->name, len) == 0)) {
            if (optr != NULL)
                *optr = cptr + len;
            return rptr;
        }
    }
    return NULL;
}

DEVICE *find_dev_from_unit(UNIT *uptr)
{
    int i;
    if (uptr == NULL)
        return NULL;
    if (uptr->dptr != NULL)
        return uptr->dptr;
    for (i = 0; sim_devices[i] != NULL; ++i) {
        DEVICE *dptr = sim_devices[i];
        if ((dptr->units != NULL) &&
            (uptr >= dptr->units) && (uptr < (dptr->units + dptr->numunits)))
            return dptr;
    }
    return NULL;
}

const char *sim_dname(DEVICE *dptr)
{
    return (dptr && dptr->name) ? dptr->name : "?";
}

uint32 sim_brk_test(t_addr bloc, uint32 btyp)
{
    (void)bloc;
    (void)btyp;
    return 0;
}

/* Open SIMH's PDP-7 core only calls sim_idle() when sim_idle_enab is true.
 * Keep the upstream disabled-path accounting so this remains safe if called
 * directly, but never sleep or throttle the ESP32-P4 host. */
t_bool sim_idle(uint32 tmr, int sin_cyc)
{
    (void)tmr;
    sim_interval -= sin_cyc;
    return FALSE;
}

/* Minimal reset_all() preserving the upstream device-order semantics.
 * This is needed by the PDP-7 CAF instruction.  It intentionally omits SCP
 * desktop-only power/debug decoration, while preserving device reset calls. */
t_stat reset_all(uint32 start)
{
    DEVICE *dptr;
    uint32 i;
    t_stat reason;
    int32 saved_sim_switches = sim_switches;

    for (i = 0; i < start; ++i) {
        if (sim_devices[i] == NULL)
            return SCPE_IERR;
    }

    for (i = start; (dptr = sim_devices[i]) != NULL; ++i) {
        sim_switches = saved_sim_switches;
        if (dptr->reset != NULL) {
            reason = dptr->reset(dptr);
            if (reason != SCPE_OK)
                return reason;
        }
    }

    sim_switches = saved_sim_switches;
    return SCPE_OK;
}

/* Basic output wrappers. */
int Fprintf(FILE *f, const char *fmt, ...)
{
    int rc;
    va_list ap;
    va_start(ap, fmt);
    rc = vfprintf(f, fmt, ap);
    va_end(ap);
    return rc;
}

void sim_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void sim_perror(const char *msg)
{
    perror(msg);
}

/* Called from the host once before guest execution. */
void simhp4_runtime_prepare(void)
{
    int i, u;
    sim_clock_queue = QUEUE_LIST_END;
    sim_interval = NOQUEUE_WAIT;
    simhp4_noqueue_time = NOQUEUE_WAIT;
    simhp4_sim_time = 0.0;
    stop_cpu = FALSE;
    sim_processing_event = FALSE;
    sim_is_running = FALSE;

    for (i = 0; sim_devices[i] != NULL; ++i) {
        DEVICE *dptr = sim_devices[i];
        for (u = 0; (dptr->units != NULL) && (u < (int)dptr->numunits); ++u)
            dptr->units[u].dptr = dptr;
    }
}

const char *simhp4_runtime_profile(void)
{
    return "CPU8K+EAE+CLK+UNIX-TTY+RB09+G2LOCAL/LIVE/event-deltaq/960kIPS/R0A5";
}
