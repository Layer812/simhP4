#include "simhp4_remote_tty.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_netif.h"
#include "esp_private/wifi.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#define SIMHP4_REMOTE_SSID       "SIMHP4-UNIXV0"
#define SIMHP4_REMOTE_PASS       "pdp7unix"
#define SIMHP4_REMOTE_PORT       10007
#define SIMHP4_REMOTE_RX_DEPTH   256
#define SIMHP4_REMOTE_TX_DEPTH   512

static const char *TAG = "SIMHP4_REMOTE_TTY";

static QueueHandle_t s_rxq;
static QueueHandle_t s_txq;
static TaskHandle_t s_task;

/* R0A9R9: keep the guest's latest primary-TTY output while no TCP client
 * exists, so the historical UNIX V0 login prompt is not lost. */
#define SIMHP4_PRECONNECT_MAX 192
static uint8_t s_preconnect[SIMHP4_PRECONNECT_MAX];
static size_t s_preconnect_head;
static size_t s_preconnect_count;
static portMUX_TYPE s_preconnect_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile int s_network_ready;
static volatile int s_client_connected;
static volatile uint32_t s_rx_drop;
static volatile uint32_t s_tx_drop;

/* R0A9R11: lightweight mirror for the right-side mini console.
 * This stores exactly the primary TTY byte stream already being sent to the
 * remote client.  It does not participate in guest semantics or flow control. */
#define SIMHP4_REMOTE_MIRROR_MAX 1024
static uint8_t s_mirror[SIMHP4_REMOTE_MIRROR_MAX];
static size_t s_mirror_head;
static size_t s_mirror_count;
static volatile uint32_t s_mirror_generation;
static portMUX_TYPE s_mirror_mux = portMUX_INITIALIZER_UNLOCKED;

/* R0A9R8: read-only path telemetry.
 * socket -> Telnet parser -> RX queue -> Open SIMH TTI -> guest
 * guest TTO -> TX queue -> socket */
static volatile uint32_t s_trace_socket_rx;
static volatile uint32_t s_trace_parser_emit;
static volatile uint32_t s_trace_guest_get;
static volatile uint32_t s_trace_tti_deliver;
static volatile uint32_t s_trace_tto_output;
static volatile uint32_t s_trace_guest_put;
static volatile uint32_t s_trace_raw_cr;
static volatile uint32_t s_trace_raw_lf;
static volatile uint32_t s_trace_raw_nul;
static volatile uint32_t s_trace_raw_iac;
static volatile uint8_t s_trace_last_raw;
static volatile uint8_t s_trace_last_emit;
static volatile uint8_t s_trace_last_guest_get;
static volatile uint8_t s_trace_last_tti_raw;
static volatile uint16_t s_trace_last_tti_guest;
static volatile uint8_t s_trace_last_tto;

static bool s_wifi_ap_netif_started;

static void mirror_reset(void)
{
    portENTER_CRITICAL(&s_mirror_mux);
    s_mirror_head = 0;
    s_mirror_count = 0;
    ++s_mirror_generation;
    portEXIT_CRITICAL(&s_mirror_mux);
}

static void mirror_put(uint8_t ch)
{
    portENTER_CRITICAL(&s_mirror_mux);

    if (s_mirror_count < SIMHP4_REMOTE_MIRROR_MAX) {
        const size_t pos = (s_mirror_head + s_mirror_count) % SIMHP4_REMOTE_MIRROR_MAX;
        s_mirror[pos] = ch;
        ++s_mirror_count;
    } else {
        s_mirror[s_mirror_head] = ch;
        s_mirror_head = (s_mirror_head + 1) % SIMHP4_REMOTE_MIRROR_MAX;
    }

    ++s_mirror_generation;
    portEXIT_CRITICAL(&s_mirror_mux);
}

uint32_t simhp4_remote_tty_console_generation(void)
{
    return __atomic_load_n(&s_mirror_generation, __ATOMIC_ACQUIRE);
}

size_t simhp4_remote_tty_console_snapshot(uint8_t *dst, size_t cap)
{
    size_t n;

    if (dst == NULL || cap == 0)
        return 0;

    portENTER_CRITICAL(&s_mirror_mux);

    n = s_mirror_count;
    if (n > cap) {
        const size_t skip = n - cap;
        for (size_t i = 0; i < cap; ++i)
            dst[i] = s_mirror[(s_mirror_head + skip + i) % SIMHP4_REMOTE_MIRROR_MAX];
        n = cap;
    } else {
        for (size_t i = 0; i < n; ++i)
            dst[i] = s_mirror[(s_mirror_head + i) % SIMHP4_REMOTE_MIRROR_MAX];
    }

    portEXIT_CRITICAL(&s_mirror_mux);
    return n;
}

static void set_connected(int yes)
{
    __atomic_store_n(&s_client_connected, yes ? 1 : 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_mirror_generation, 1u, __ATOMIC_RELEASE);
    if (s_rxq) xQueueReset(s_rxq);
    if (s_txq) xQueueReset(s_txq);
}

int simhp4_remote_tty_network_ready(void)
{
    return __atomic_load_n(&s_network_ready, __ATOMIC_ACQUIRE);
}

int simhp4_remote_tty_client_connected(void)
{
    return __atomic_load_n(&s_client_connected, __ATOMIC_ACQUIRE);
}

uint32_t simhp4_remote_tty_rx_drop_count(void)
{
    return __atomic_load_n(&s_rx_drop, __ATOMIC_ACQUIRE);
}

uint32_t simhp4_remote_tty_tx_drop_count(void)
{
    return __atomic_load_n(&s_tx_drop, __ATOMIC_ACQUIRE);
}

static void preconnect_capture(uint8_t ch)
{
    portENTER_CRITICAL(&s_preconnect_mux);
    if (s_preconnect_count < SIMHP4_PRECONNECT_MAX) {
        const size_t pos = (s_preconnect_head + s_preconnect_count) % SIMHP4_PRECONNECT_MAX;
        s_preconnect[pos] = ch;
        ++s_preconnect_count;
    } else {
        s_preconnect[s_preconnect_head] = ch;
        s_preconnect_head = (s_preconnect_head + 1) % SIMHP4_PRECONNECT_MAX;
    }
    portEXIT_CRITICAL(&s_preconnect_mux);
}

static size_t preconnect_take(uint8_t *dst, size_t cap)
{
    size_t n;
    if (dst == NULL || cap == 0)
        return 0;

    portENTER_CRITICAL(&s_preconnect_mux);
    n = s_preconnect_count;
    if (n > cap)
        n = cap;
    for (size_t i = 0; i < n; ++i)
        dst[i] = s_preconnect[(s_preconnect_head + i) % SIMHP4_PRECONNECT_MAX];

    if (n == s_preconnect_count) {
        s_preconnect_head = 0;
        s_preconnect_count = 0;
    } else {
        s_preconnect_head = (s_preconnect_head + n) % SIMHP4_PRECONNECT_MAX;
        s_preconnect_count -= n;
    }
    portEXIT_CRITICAL(&s_preconnect_mux);
    return n;
}

int simhp4_remote_tty_guest_getc(uint8_t *out)
{
    if (out == NULL || s_rxq == NULL)
        return 0;

    if (xQueueReceive(s_rxq, out, 0) != pdTRUE)
        return 0;

    __atomic_add_fetch(&s_trace_guest_get, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_last_guest_get, *out, __ATOMIC_RELAXED);
    return 1;
}

int simhp4_remote_tty_guest_putc(uint8_t ch)
{
    const uint8_t c = (uint8_t)(ch & 0x7f);
    __atomic_add_fetch(&s_trace_guest_put, 1u, __ATOMIC_RELAXED);

    /* Never stall UNIX when disconnected, but retain the latest real guest
     * transcript so the first client can see the historical login prompt. */
    if (!simhp4_remote_tty_client_connected() || s_txq == NULL) {
        preconnect_capture(c);
        return 1;
    }

    mirror_put(c);
    if (xQueueSend(s_txq, &c, 0) == pdTRUE)
        return 1;

    __atomic_add_fetch(&s_tx_drop, 1u, __ATOMIC_RELAXED);
    return 0;
}

void simhp4_remote_tty_trace_tti_delivery(uint8_t raw_host_ch, uint16_t converted_guest_ch)
{
    __atomic_add_fetch(&s_trace_tti_deliver, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_last_tti_raw, raw_host_ch, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_last_tti_guest, converted_guest_ch, __ATOMIC_RELAXED);
}

void simhp4_remote_tty_trace_tto_output(uint8_t ch)
{
    __atomic_add_fetch(&s_trace_tto_output, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_last_tto, (uint8_t)(ch & 0x7f), __ATOMIC_RELAXED);
}

static void rx_emit(uint8_t ch)
{
    if (s_rxq == NULL)
        return;

    __atomic_add_fetch(&s_trace_parser_emit, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_last_emit, ch, __ATOMIC_RELAXED);

    if (xQueueSend(s_rxq, &ch, 0) != pdTRUE)
        __atomic_add_fetch(&s_rx_drop, 1u, __ATOMIC_RELAXED);
}

/* Minimal Telnet/NVT parser.
 * - strips IAC negotiation from the PDP-7 byte stream
 * - normalizes CR LF and CR NUL to a single CR
 * Open SIMH's existing UNIX terminal mode then performs its historical
 * CR->LF mapping; that guest-visible behavior is not duplicated here. */
typedef struct {
    int state;
    int cr_pending;
} telnet_rx_t;

enum {
    TN_DATA = 0,
    TN_IAC,
    TN_IAC_OPT,
    TN_SB,
    TN_SB_IAC
};

static void telnet_feed(telnet_rx_t *p, uint8_t c)
{
    if (p->state == TN_SB) {
        if (c == 255) p->state = TN_SB_IAC;
        return;
    }
    if (p->state == TN_SB_IAC) {
        if (c == 240) p->state = TN_DATA; /* IAC SE */
        else if (c != 255) p->state = TN_SB;
        return;
    }
    if (p->state == TN_IAC_OPT) {
        p->state = TN_DATA;
        return;
    }
    if (p->state == TN_IAC) {
        if (c == 250) {                  /* SB */
            p->state = TN_SB;
            return;
        }
        if (c == 251 || c == 252 || c == 253 || c == 254) {
            p->state = TN_IAC_OPT;       /* WILL/WONT/DO/DONT + option */
            return;
        }
        p->state = TN_DATA;              /* ignore command */
        return;
    }
    if (c == 255) {
        p->state = TN_IAC;
        return;
    }

    if (p->cr_pending) {
        p->cr_pending = 0;
        rx_emit('\r');
        if (c == '\n' || c == 0)
            return;
    }

    if (c == '\r') {
        p->cr_pending = 1;
        return;
    }

    rx_emit(c & 0x7f);
}

static void trace_socket_packet(const uint8_t *buf, int n)
{
    char hex[3 * 24 + 1];
    size_t off = 0;
    const int shown = (n < 24) ? n : 24;

    for (int i = 0; i < n; ++i) {
        const uint8_t c = buf[i];
        __atomic_add_fetch(&s_trace_socket_rx, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&s_trace_last_raw, c, __ATOMIC_RELAXED);
        if (c == '\r') __atomic_add_fetch(&s_trace_raw_cr, 1u, __ATOMIC_RELAXED);
        if (c == '\n') __atomic_add_fetch(&s_trace_raw_lf, 1u, __ATOMIC_RELAXED);
        if (c == 0)    __atomic_add_fetch(&s_trace_raw_nul, 1u, __ATOMIC_RELAXED);
        if (c == 255)  __atomic_add_fetch(&s_trace_raw_iac, 1u, __ATOMIC_RELAXED);
    }

    for (int i = 0; i < shown && off + 4 < sizeof(hex); ++i) {
        const int wrote = snprintf(hex + off, sizeof(hex) - off,
                                   "%s%02X", (i == 0) ? "" : " ", buf[i]);
        if (wrote <= 0)
            break;
        off += (size_t)wrote;
    }
    hex[off] = 0;

    ESP_LOGI(TAG, "R0A9R8 RXRAW n=%d hex=[%s]%s",
             n, hex, (n > shown) ? " ..." : "");
}

static void trace_summary(const telnet_rx_t *parser)
{
    ESP_LOGI(TAG,
             "R0A9R8 TRACE sock_rx=%u emit=%u guest_get=%u tti=%u tto=%u guest_put=%u "
             "rawCR=%u rawLF=%u rawNUL=%u rawIAC=%u rxq=%u txq=%u cr_pending=%d "
             "last_raw=%02X last_emit=%02X last_get=%02X last_tti=%02X->%03X last_tto=%02X "
             "drop=%u/%u",
             (unsigned)__atomic_load_n(&s_trace_socket_rx, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_parser_emit, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_guest_get, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_tti_deliver, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_tto_output, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_guest_put, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_raw_cr, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_raw_lf, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_raw_nul, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_raw_iac, __ATOMIC_RELAXED),
             s_rxq ? (unsigned)uxQueueMessagesWaiting(s_rxq) : 0u,
             s_txq ? (unsigned)uxQueueMessagesWaiting(s_txq) : 0u,
             parser ? parser->cr_pending : -1,
             (unsigned)__atomic_load_n(&s_trace_last_raw, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_last_emit, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_last_guest_get, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_last_tti_raw, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_last_tti_guest, __ATOMIC_RELAXED),
             (unsigned)__atomic_load_n(&s_trace_last_tto, __ATOMIC_RELAXED),
             (unsigned)simhp4_remote_tty_rx_drop_count(),
             (unsigned)simhp4_remote_tty_tx_drop_count());
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const int n = send(fd, buf + off, len - off, 0);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return 0;
    }
    return 1;
}

/* M5Stack Tab5 uses WiFi Remote, not the P4 native WiFi driver.
 * ESP-IDF's ordinary default AP netif helper is not sufficient for this
 * topology.  Mirror the Tab5 UserDemo glue: attach an AP netif explicitly
 * and bind the remote WiFi driver callbacks on WIFI_EVENT_AP_START. */
static void simhp4_wifi_remote_ap_start_handler(
    void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    esp_netif_t *netif = (esp_netif_t *)arg;

    if (s_wifi_ap_netif_started || esp_netif_is_netif_up(netif))
        return;

    wifi_netif_driver_t driver =
        (wifi_netif_driver_t)esp_netif_get_io_driver(netif);
    uint8_t mac[6];

    esp_err_t e = esp_wifi_get_if_mac(driver, mac);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R4 AP start: esp_wifi_get_if_mac: %s",
                 esp_err_to_name(e));
        return;
    }

    if (esp_wifi_is_if_ready_when_started(driver)) {
        e = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "R0A9R4 AP start: register rxcb: %s",
                     esp_err_to_name(e));
            return;
        }
    }

    e = esp_wifi_internal_reg_netstack_buf_cb(
        esp_netif_netstack_buf_ref, esp_netif_netstack_buf_free);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R4 AP start: netstack callbacks: %s",
                 esp_err_to_name(e));
        return;
    }

    e = esp_netif_set_mac(netif, mac);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R4 AP start: esp_netif_set_mac: %s",
                 esp_err_to_name(e));
        return;
    }

    esp_netif_action_start(netif, base, event_id, data);
    s_wifi_ap_netif_started = true;
    __atomic_store_n(&s_network_ready, 1, __ATOMIC_RELEASE);
    ESP_LOGI(TAG,
             "SIMHP4_R0A9R4 WIFI AP READY ssid=%s password=%s ip=192.168.4.1 tcp=%d",
             SIMHP4_REMOTE_SSID, SIMHP4_REMOTE_PASS, SIMHP4_REMOTE_PORT);
}

static void simhp4_wifi_remote_ap_stop_handler(
    void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    esp_netif_t *netif = (esp_netif_t *)arg;

    if (!s_wifi_ap_netif_started && !esp_netif_is_netif_up(netif))
        return;

    esp_netif_action_stop(netif, base, event_id, data);
    s_wifi_ap_netif_started = false;
    __atomic_store_n(&s_network_ready, 0, __ATOMIC_RELEASE);
}

static esp_netif_t *simhp4_create_wifi_remote_ap_netif(void)
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    esp_netif_t *netif = esp_netif_new(&cfg);
    if (netif == NULL) {
        ESP_LOGE(TAG, "R0A9R4 AP netif allocation failed");
        return NULL;
    }

    esp_err_t e = esp_netif_attach_wifi_ap(netif);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R4 AP attach failed: %s", esp_err_to_name(e));
        esp_netif_destroy(netif);
        return NULL;
    }

    e = esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_AP_START,
        simhp4_wifi_remote_ap_start_handler, netif);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R4 AP start handler register failed: %s",
                 esp_err_to_name(e));
        esp_netif_destroy(netif);
        return NULL;
    }

    e = esp_event_handler_register(
        WIFI_EVENT, WIFI_EVENT_AP_STOP,
        simhp4_wifi_remote_ap_stop_handler, netif);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R4 AP stop handler register failed: %s",
                 esp_err_to_name(e));
        (void)esp_event_handler_unregister(
            WIFI_EVENT, WIFI_EVENT_AP_START,
            simhp4_wifi_remote_ap_start_handler);
        esp_netif_destroy(netif);
        return NULL;
    }

    return netif;
}

static int wifi_remote_ap_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        e = nvs_flash_erase();
        if (e == ESP_OK)
            e = nvs_flash_init();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9 NVS init failed: %s", esp_err_to_name(e));
        return 0;
    }

    e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "R0A9 esp_netif_init failed: %s", esp_err_to_name(e));
        return 0;
    }

    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "R0A9 event loop init failed: %s", esp_err_to_name(e));
        return 0;
    }

    /* esp_hosted 1.4.0 initializes itself from its component constructor.
     * Do not include esp_hosted.h here: its public umbrella header leaks
     * private SDMMC dependencies on ESP-IDF 5.5.4, and this pinned version
     * has no esp_hosted_connect_to_slave() API.  The first standard
     * esp_wifi_init() below performs the Hosted reconfigure/handshake. */
    ESP_LOGI(TAG,
             "R0A9R4 Tab5 C6 WiFi Remote: constructor ready; starting AP after M5.begin");

    esp_netif_t *ap = simhp4_create_wifi_remote_ap_netif();
    if (ap == NULL) {
        ESP_LOGE(TAG, "R0A9R4 FIRST FAILURE: create WiFi Remote AP netif");
        return 0;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R4 FIRST FAILURE: esp_wifi_init: %s", esp_err_to_name(e));
        return 0;
    }

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    memcpy(wc.ap.ssid, SIMHP4_REMOTE_SSID, sizeof(SIMHP4_REMOTE_SSID) - 1);
    memcpy(wc.ap.password, SIMHP4_REMOTE_PASS, sizeof(SIMHP4_REMOTE_PASS) - 1);
    wc.ap.ssid_len = sizeof(SIMHP4_REMOTE_SSID) - 1;
    wc.ap.channel = 6;
    wc.ap.max_connection = 2;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;

    e = esp_wifi_set_mode(WIFI_MODE_AP);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R4 FIRST FAILURE: esp_wifi_set_mode(AP): %s", esp_err_to_name(e));
        return 0;
    }

    e = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R4 FIRST FAILURE: esp_wifi_set_config(AP): %s", esp_err_to_name(e));
        return 0;
    }

    e = esp_wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R4 FIRST FAILURE: esp_wifi_start: %s", esp_err_to_name(e));
        return 0;
    }

    /* R0A9R6 falsifier: force the minimum supported WiFi TX ceiling.
     * esp_wifi_set_max_tx_power() uses 0.25 dBm units; 8 == 2 dBm.
     * This is intentionally extreme for one A/B run. */
    const int8_t tx_power_req = 8;
    e = esp_wifi_set_max_tx_power(tx_power_req);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R6 TX2DBM FIRST FAILURE: esp_wifi_set_max_tx_power(%d): %s",
                 (int)tx_power_req, esp_err_to_name(e));
        return 0;
    }

    int8_t tx_power_actual = -1;
    e = esp_wifi_get_max_tx_power(&tx_power_actual);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "R0A9R6 TX2DBM FIRST FAILURE: esp_wifi_get_max_tx_power: %s",
                 esp_err_to_name(e));
        return 0;
    }

    ESP_LOGI(TAG,
             "SIMHP4_R0A9R6 TX POWER A/B req=%d actual=%d quarter-dBm (%.2f dBm)",
             (int)tx_power_req, (int)tx_power_actual,
             ((double)tx_power_actual) * 0.25);

    ESP_LOGI(TAG, "SIMHP4_R0A9R4 AP start accepted; waiting for AP_START event");
    return 1;
}

static int make_listener(void)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed errno=%d", errno);
        return -1;
    }

    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(SIMHP4_REMOTE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "bind port=%d failed errno=%d", SIMHP4_REMOTE_PORT, errno);
        close(fd);
        return -1;
    }

    if (listen(fd, 1) != 0) {
        ESP_LOGE(TAG, "listen failed errno=%d", errno);
        close(fd);
        return -1;
    }

    return fd;
}

static void serve_client(int fd)
{
    /* WILL ECHO, WILL SUPPRESS-GO-AHEAD, DO SUPPRESS-GO-AHEAD. */
    const uint8_t hello[] = {255,251,1, 255,251,3, 255,253,3};
    (void)send_all(fd, hello, sizeof(hello));

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 20000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    telnet_rx_t parser;
    memset(&parser, 0, sizeof(parser));

    uint8_t preconnect[SIMHP4_PRECONNECT_MAX];
    const size_t preconnect_len = preconnect_take(preconnect, sizeof(preconnect));

    mirror_reset();
    for (size_t i = 0; i < preconnect_len; ++i)
        mirror_put(preconnect[i]);

    if (preconnect_len > 0) {
        if (!send_all(fd, preconnect, preconnect_len))
            return;
        ESP_LOGI(TAG, "SIMHP4_R0A9R9 replayed %u pre-connect TTY bytes (historical prompt)",
                 (unsigned)preconnect_len);
    } else {
        ESP_LOGI(TAG, "SIMHP4_R0A9R9 no pre-connect TTY transcript available");
    }

    __atomic_store_n(&s_trace_socket_rx, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_parser_emit, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_guest_get, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_tti_deliver, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_tto_output, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_guest_put, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_raw_cr, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_raw_lf, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_raw_nul, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_trace_raw_iac, 0u, __ATOMIC_RELAXED);

    TickType_t next_trace = xTaskGetTickCount() + pdMS_TO_TICKS(2000);

    set_connected(1);
    ESP_LOGI(TAG,
             "SIMHP4_R0A9R9 REMOTE TTY CONNECTED; pre-connect guest output replay attempted");

    for (;;) {
        uint8_t outbuf[128];
        size_t outn = 0;
        while (outn < sizeof(outbuf) &&
               xQueueReceive(s_txq, &outbuf[outn], 0) == pdTRUE) {
            ++outn;
        }
        if (outn && !send_all(fd, outbuf, outn))
            break;

        uint8_t inbuf[64];
        const int n = recv(fd, inbuf, sizeof(inbuf), 0);
        if (n > 0) {
            trace_socket_packet(inbuf, n);
            for (int i = 0; i < n; ++i)
                telnet_feed(&parser, inbuf[i]);
        } else if (n == 0) {
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }

        if ((int32_t)(xTaskGetTickCount() - next_trace) >= 0) {
            trace_summary(&parser);
            next_trace = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        }

        vTaskDelay(1);
    }

    trace_summary(&parser);
    if (parser.cr_pending)
        rx_emit('\r');

    set_connected(0);
    ESP_LOGI(TAG,
             "SIMHP4_R0A9R4 REMOTE TTY DISCONNECTED rx_drop=%u tx_drop=%u",
             (unsigned)simhp4_remote_tty_rx_drop_count(),
             (unsigned)simhp4_remote_tty_tx_drop_count());
}

static void remote_task(void *arg)
{
    (void)arg;

    /* M5.begin() already initialized the Tab5 IO expanders and WLAN_PWR_EN.
     * Let the on-board C6 rail/ROM settle before its SDIO reset/handshake. */
    vTaskDelay(pdMS_TO_TICKS(250));

    if (!wifi_remote_ap_init()) {
        ESP_LOGE(TAG, "SIMHP4_R0A9R4 remote TTY unavailable; local G2 remains operational");
        vTaskDelete(NULL);
        return;
    }

    const int listener = make_listener();
    if (listener < 0) {
        ESP_LOGE(TAG, "SIMHP4_R0A9R4 TCP listener failed; local G2 remains operational");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SIMHP4_R0A9R4 TTY listener ready on 192.168.4.1:%d", SIMHP4_REMOTE_PORT);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        const int client = accept(listener, (struct sockaddr *)&peer, &plen);
        if (client < 0) {
            if (errno != EINTR)
                vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(TAG, "remote client %s", inet_ntoa(peer.sin_addr));
        serve_client(client);
        shutdown(client, SHUT_RDWR);
        close(client);
    }
}

int simhp4_remote_tty_start(void)
{
    if (s_task != NULL)
        return 1;

    if (s_rxq == NULL)
        s_rxq = xQueueCreate(SIMHP4_REMOTE_RX_DEPTH, sizeof(uint8_t));
    if (s_txq == NULL)
        s_txq = xQueueCreate(SIMHP4_REMOTE_TX_DEPTH, sizeof(uint8_t));

    if (s_rxq == NULL || s_txq == NULL) {
        ESP_LOGE(TAG, "R0A9R4 remote TTY queue allocation failed");
        return 0;
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(
        remote_task,
        "simhp4_remote_tty",
        8192,
        NULL,
        2,
        &s_task,
        0);

    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "R0A9R4 remote TTY task creation failed");
        return 0;
    }

    ESP_LOGI(TAG,
             "SIMHP4_R0A9R4 remote TTY host task started CPU0; local UNIX boot does not wait for WiFi");
    return 1;
}

