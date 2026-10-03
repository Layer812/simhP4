/*
 * simhP4 / RetroP4 integration
 * Copyright (c) 2026 simhP4 contributors
 *
 * Upstream SIMH-derived portions retain their original copyright notices
 * and license terms.  See LICENSES/SIMH_LICENSE.txt.
 */
#include "driver/i2c_master.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include <M5Unified.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "simhp4_sd.h"
#include "simhp4_probe.h"
#include "simhp4_media.h"
#include "simhp4_usb_keyboard.h"
#include "simhp4_frontend_input.h"
#include "simhp4_selector.h"
#include "simhp4_remote_tty.h"
#include "simhp4_type340_surface.h"
#include "simhp4_st_migration.h"

static const char *TAG = "SIMHP4";
static const char *UNIX_DISK_PATH = "/sdcard/UNIXV0.DSK";
static const char *UNIX_BOOT_PATH = "/sdcard/BOOT.RIM";

static const size_t UNIX_IMAGE_EXPECTED_SIZE = 4096000u;
static const size_t UNIX_BOOT_EXPECTED_SIZE = 69u;

typedef struct {
    int ok;
    int disk_load_rc;
    int disk_words;
    int boot_load_rc;
    int boot_words;
    int g2_attach_rc;
    int reason;
    int pc;
    int rb_status;
    int shell_seen;
    int core;
    char console_tail[512];
} simhp4_unix_result_t;

static QueueHandle_t s_result_queue = nullptr;

static int write_embedded_file(const char *path,
                               const unsigned char *start,
                               const unsigned char *end,
                               size_t expected_size)
{
    struct stat st = {};
    const size_t size = (size_t)(end - start);

    if (size != expected_size) {
        ESP_LOGE(TAG,
                 "R0A1F embedded asset size mismatch path=%s got=%u expected=%u",
                 path, (unsigned)size, (unsigned)expected_size);
        return 0;
    }

    /* Preserve a previously booted/writable UNIX disk if its size is right. */
    if ((stat(path, &st) == 0) && S_ISREG(st.st_mode) &&
        ((size_t)st.st_size == expected_size)) {
        ESP_LOGI(TAG, "R0A1F SD asset already present path=%s size=%u",
                 path, (unsigned)expected_size);
        return 1;
    }

    FILE *fp = std::fopen(path, "wb");
    if (fp == nullptr) {
        const int e = errno;
        ESP_LOGE(TAG, "R0A1F fopen failed path=%s errno=%d (%s)",
                 path, e, std::strerror(e));
        return 0;
    }

    const unsigned char *p = start;
    size_t remain = size;
    while (remain != 0) {
        const size_t chunk = (remain > 16384u) ? 16384u : remain;
        if (std::fwrite(p, 1, chunk, fp) != chunk) {
            const int e = errno;
            ESP_LOGE(TAG, "R0A1F fwrite failed path=%s errno=%d (%s)",
                     path, e, std::strerror(e));
            std::fclose(fp);
            return 0;
        }
        p += chunk;
        remain -= chunk;
    }

    if (std::fflush(fp) != 0) {
        const int e = errno;
        ESP_LOGE(TAG, "R0A1F fflush failed path=%s errno=%d (%s)",
                 path, e, std::strerror(e));
        std::fclose(fp);
        return 0;
    }

    if (std::fclose(fp) != 0) {
        const int e = errno;
        ESP_LOGE(TAG, "R0A1F fclose failed path=%s errno=%d (%s)",
                 path, e, std::strerror(e));
        return 0;
    }

    ESP_LOGI(TAG, "R0A1F SD asset seeded path=%s size=%u",
             path, (unsigned)size);
    return 1;
}

/* -------------------------------------------------------------------------
 * SIMHP4_R0A8 A164 bridge
 *
 * Ported from the proven px68k Tab5 path:
 *   address      0x6D
 *   ExtPort1     SDA=GPIO0, SCL=GPIO1
 *   Normal mode  REG_MODE=0
 *   KEY_EVENT    0x20
 *   event byte   bit7=press, bits6:4=row, bits3:0=column
 *   empty        0xFF
 *   service      CPU0, direct KEY_EVENT drain every real 10 ms
 *
 * A164 is mounted on the opposite physical edge of Tab5, so the usable
 * orientation is 180 degrees from the normal tablet orientation.
 *
 * CPU0 owns A164 I2C and physical display orientation.
 * CPU1 remains the sole Open SIMH guest/device owner.
 * USB HID remains enabled in parallel; both sources enqueue into G2IN.
 * ------------------------------------------------------------------------- */
#define SIMHP4_A164_ADDR             0x6Du
#define SIMHP4_A164_REG_INT_CFG      0x00u
#define SIMHP4_A164_REG_INT_STAT     0x01u
#define SIMHP4_A164_REG_EVENT_NUM    0x02u
#define SIMHP4_A164_REG_MODE         0x10u
#define SIMHP4_A164_REG_KEY_EVENT    0x20u
#define SIMHP4_A164_REG_FW_VERSION   0xFEu
#define SIMHP4_A164_MODE_NORMAL      0u
#define SIMHP4_A164_EVENT_EMPTY      0xFFu
#define SIMHP4_A164_ROWS             5u
#define SIMHP4_A164_COLS             14u
#define SIMHP4_A164_KEYS             70u
#define SIMHP4_A164_I2C_HZ           400000u
#define SIMHP4_A164_SDA_GPIO         GPIO_NUM_0
#define SIMHP4_A164_SCL_GPIO         GPIO_NUM_1

static i2c_master_bus_handle_t s_a164_bus = NULL;
static i2c_master_dev_handle_t s_a164_dev = NULL;
static TaskHandle_t s_a164_task = NULL;

static volatile bool s_a164_present = false;
static volatile uint32_t s_a164_ui_generation = 0;
static uint8_t s_a164_base_rotation = 0;
static bool s_a164_sym_down = false;
static bool s_a164_aa_down = false;
static bool s_a164_ctrl_down = false;
static bool s_a164_alt_down = false;
static unsigned s_a164_read_failures = 0;

static bool simhp4_a164_present()
{
    return __atomic_load_n(&s_a164_present, __ATOMIC_ACQUIRE);
}

static uint32_t simhp4_a164_ui_generation()
{
    return __atomic_load_n(&s_a164_ui_generation, __ATOMIC_ACQUIRE);
}

static void simhp4_a164_publish_present(bool present)
{
    const bool old = __atomic_exchange_n(&s_a164_present, present, __ATOMIC_ACQ_REL);
    if (old != present) {
        __atomic_add_fetch(&s_a164_ui_generation, 1u, __ATOMIC_ACQ_REL);
        ESP_LOGI(TAG,
                 "SIMHP4_R0A8 A164 %s; display orientation target=%s",
                 present ? "ATTACHED" : "DETACHED",
                 present ? "180deg" : "normal");
    }
}

static uint8_t simhp4_a164_target_rotation()
{
    const uint8_t base = s_a164_base_rotation;
    if (!simhp4_a164_present())
        return base;

    /* Preserve any high mirror bits used by M5GFX; rotate low 2 bits by 180. */
    return (uint8_t)((base & ~3u) | ((base + 2u) & 3u));
}

static bool simhp4_a164_i2c_init()
{
    if (s_a164_bus != NULL && s_a164_dev != NULL)
        return true;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = (i2c_port_num_t)-1,
        .sda_io_num = SIMHP4_A164_SDA_GPIO,
        .scl_io_num = SIMHP4_A164_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
            .allow_pd = false,
        },
    };

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t e = i2c_new_master_bus(&bus_cfg, &bus);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "SIMHP4_R0A8 A164 I2C bus create failed: %s", esp_err_to_name(e));
        return false;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SIMHP4_A164_ADDR,
        .scl_speed_hz = SIMHP4_A164_I2C_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };

    i2c_master_dev_handle_t dev = NULL;
    e = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (e != ESP_OK) {
        (void)i2c_del_master_bus(bus);
        ESP_LOGW(TAG, "SIMHP4_R0A8 A164 I2C add-device failed: %s", esp_err_to_name(e));
        return false;
    }

    s_a164_bus = bus;
    s_a164_dev = dev;
    return true;
}

static bool simhp4_a164_read(uint8_t reg, uint8_t *dst, size_t len)
{
    if (s_a164_dev == NULL || dst == NULL || len == 0)
        return false;

    const esp_err_t e = i2c_master_transmit_receive(
        s_a164_dev, &reg, 1, dst, len, 20);
    return e == ESP_OK;
}

static bool simhp4_a164_write8(uint8_t reg, uint8_t value)
{
    if (s_a164_dev == NULL)
        return false;

    const uint8_t tx[2] = {reg, value};
    return i2c_master_transmit(s_a164_dev, tx, sizeof(tx), 20) == ESP_OK;
}

static void simhp4_a164_reset_modifiers()
{
    s_a164_sym_down = false;
    s_a164_aa_down = false;
    s_a164_ctrl_down = false;
    s_a164_alt_down = false;
}

static bool simhp4_a164_detect_and_configure()
{
    if (!simhp4_a164_i2c_init())
        return false;

    esp_err_t e = ESP_FAIL;
    for (unsigned attempt = 0; attempt < 3u; ++attempt) {
        e = i2c_master_probe(s_a164_bus, SIMHP4_A164_ADDR, 20);
        if (e == ESP_OK)
            break;
        if (e == ESP_ERR_TIMEOUT || e == ESP_ERR_INVALID_STATE)
            (void)i2c_master_bus_reset(s_a164_bus);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (e != ESP_OK)
        return false;

    uint8_t fw = 0xffu;
    if (!simhp4_a164_read(SIMHP4_A164_REG_FW_VERSION, &fw, 1))
        return false;

    uint8_t mode_before = 0xffu;
    (void)simhp4_a164_read(SIMHP4_A164_REG_MODE, &mode_before, 1);

    if (!simhp4_a164_write8(SIMHP4_A164_REG_MODE, SIMHP4_A164_MODE_NORMAL))
        return false;

    /* CONFIG_FREERTOS_HZ is 100 in the proven Tab5 setup: one real tick. */
    vTaskDelay(1);

    if (!simhp4_a164_write8(SIMHP4_A164_REG_INT_STAT, 0u))
        return false;
    if (!simhp4_a164_write8(SIMHP4_A164_REG_EVENT_NUM, 0u))
        return false;
    if (!simhp4_a164_write8(SIMHP4_A164_REG_INT_CFG, 0x01u))
        return false;

    uint8_t mode = 0xffu;
    uint8_t intcfg = 0xffu;
    if (!simhp4_a164_read(SIMHP4_A164_REG_MODE, &mode, 1) ||
        !simhp4_a164_read(SIMHP4_A164_REG_INT_CFG, &intcfg, 1) ||
        mode != SIMHP4_A164_MODE_NORMAL ||
        (intcfg & 0x01u) == 0u) {
        return false;
    }

    simhp4_a164_reset_modifiers();
    s_a164_read_failures = 0;
    simhp4_a164_publish_present(true);

    ESP_LOGI(TAG,
             "SIMHP4_R0A8 A164 detected fw=0x%02X mode %u->NORMAL intcfg=0x%02X orientation=180deg",
             (unsigned)fw, (unsigned)mode_before, (unsigned)intcfg);
    return true;
}

/* Intended character for each physical A164 key.
 * Zero entries are modifiers or keys without a useful UNIX V0 terminal byte.
 *
 * row0: Esc 1 2 3 4 5 6 7 8 9 0 - + Del
 * row1: ` ! @ # $ %% ^ & * ( ) [ ] \
 * row2: Tab q w e r t y u i o p ; ' Backspace
 * row3: Sym Aa a s d f g h j k l Up _ Enter
 * row4: Ctrl Alt z x c v b n m . Left Down Right Space
 */
static const uint8_t s_a164_ascii_base[SIMHP4_A164_KEYS] = {
    0x1B,'1','2','3','4','5','6','7','8','9','0','-','+',0x7F,
    '`','!','@','#','$','%','^','&','*','(',')','[',']','\\',
    '\t','q','w','e','r','t','y','u','i','o','p',';','\'','\b',
    0,0,'a','s','d','f','g','h','j','k','l',0,'_','\r',
    0,0,'z','x','c','v','b','n','m','.',0,0,0,' '
};

static const uint8_t s_a164_ascii_sym[SIMHP4_A164_KEYS] = {
    0x1B,'1','2','3','4','5','6','7','8','9','0','-','+',0x7F,
    '~','?','@','#','$','%','^','&','/','<','>','{','}','|',
    '\t','q','w','e','r','t','y','u','i','o','p',':','"','\b',
    0,0,'a','s','d','f','g','h','j','k','l',0,'=','\r',
    0,0,'z','x','c','v','b','n','m',',',0,0,0,' '
};

extern "C" void simhp4_g2_button_set(unsigned logical_button, int down);

static void simhp4_a164_emit_key(unsigned idx, bool down)
{
    const unsigned k_sym  = 3u * SIMHP4_A164_COLS + 0u;
    const unsigned k_aa   = 3u * SIMHP4_A164_COLS + 1u;
    const unsigned k_ctrl = 4u * SIMHP4_A164_COLS + 0u;
    const unsigned k_alt  = 4u * SIMHP4_A164_COLS + 1u;

    if (idx == k_sym)  { s_a164_sym_down = down;  return; }
    if (idx == k_aa)   { s_a164_aa_down = down;   return; }
    if (idx == k_ctrl) { s_a164_ctrl_down = down; return; }
    if (idx == k_alt)  { s_a164_alt_down = down;  return; }

    /* S0 selector owns host navigation before a guest is started.  Consume
     * only explicit navigation keys here; when selector mode is off the
     * Release Final A164 behavior below is unchanged. */
    if (simhp4_frontend_selector_active()) {
        if (!down)
            return;

        const unsigned k_esc   = 0u * SIMHP4_A164_COLS + 0u;
        const unsigned k_del   = 0u * SIMHP4_A164_COLS + 13u;
        const unsigned k_bksp  = 2u * SIMHP4_A164_COLS + 13u;
        const unsigned k_up    = 3u * SIMHP4_A164_COLS + 11u;
        const unsigned k_enter = 3u * SIMHP4_A164_COLS + 13u;
        const unsigned k_left  = 4u * SIMHP4_A164_COLS + 10u;
        const unsigned k_down  = 4u * SIMHP4_A164_COLS + 11u;
        const unsigned k_right = 4u * SIMHP4_A164_COLS + 12u;

        simhp4_frontend_key_t nav = SIMHP4_FRONTEND_KEY_NONE;
        if      (idx == k_up)                    nav = SIMHP4_FRONTEND_KEY_UP;
        else if (idx == k_down)                  nav = SIMHP4_FRONTEND_KEY_DOWN;
        else if (idx == k_left)                  nav = SIMHP4_FRONTEND_KEY_LEFT;
        else if (idx == k_right)                 nav = SIMHP4_FRONTEND_KEY_RIGHT;
        else if (idx == k_enter)                 nav = SIMHP4_FRONTEND_KEY_ENTER;
        else if (idx == k_esc)                   nav = SIMHP4_FRONTEND_KEY_ESCAPE;
        else if (idx == k_del || idx == k_bksp) nav = SIMHP4_FRONTEND_KEY_BACK;

        if (nav != SIMHP4_FRONTEND_KEY_NONE)
            (void)simhp4_frontend_submit_nav(nav);
        return;
    }

    /* SIMHP4_R0A9R19B_A164_HELD_SPACE_TRAVEL
     * A164 Normal mode supplies explicit make/break events, so the continuous
     * Space Travel controls are modeled as levels, not synthetic key-repeat.
     * Historical digits 3..6 and cursor aliases drive the same PB2..PB5:
     *   3/Down=PB2, 4/Up=PB3, 5/Right=PB4, 6/Left=PB5.
     * 7/8 and Z/X intentionally remain one-shot scale steps in the normal
     * character path below.  A break event clears a held PB even after the
     * captured display has just been released. */
    {
        const unsigned k_3     = 0u * SIMHP4_A164_COLS + 3u;
        const unsigned k_4     = 0u * SIMHP4_A164_COLS + 4u;
        const unsigned k_5     = 0u * SIMHP4_A164_COLS + 5u;
        const unsigned k_6     = 0u * SIMHP4_A164_COLS + 6u;
        const unsigned k_up    = 3u * SIMHP4_A164_COLS + 11u;
        const unsigned k_left  = 4u * SIMHP4_A164_COLS + 10u;
        const unsigned k_down  = 4u * SIMHP4_A164_COLS + 11u;
        const unsigned k_right = 4u * SIMHP4_A164_COLS + 12u;
        int pb = -1;
        if      (idx == k_3 || idx == k_down)  pb = 2;
        else if (idx == k_4 || idx == k_up)    pb = 3;
        else if (idx == k_5 || idx == k_right) pb = 4;
        else if (idx == k_6 || idx == k_left)  pb = 5;
        if (pb >= 0) {
            if (simhp4_type340_surface_is_active()) {
                simhp4_g2_button_set((unsigned)pb, down ? 1 : 0);
                return;
            }
            if (!down)
                simhp4_g2_button_set((unsigned)pb, 0);
        }
    }

    /* Terminal input is edge-triggered on key press. */
    if (!down || idx >= SIMHP4_A164_KEYS)
        return;

    uint8_t ch = s_a164_sym_down
               ? s_a164_ascii_sym[idx]
               : s_a164_ascii_base[idx];

    if (ch == 0)
        return;

    if (s_a164_aa_down && ch >= 'a' && ch <= 'z')
        ch = (uint8_t)(ch - 'a' + 'A');

    if (s_a164_ctrl_down) {
        uint8_t u = ch;
        if (u >= 'a' && u <= 'z')
            u = (uint8_t)(u - 'a' + 'A');
        if (u >= '@' && u <= '_')
            ch = (uint8_t)(u & 0x1Fu);
    }

    /* Alt is deliberately not interpreted by UNIX V0; preserve the key
     * stream rather than inventing an escape-prefix convention. */
    (void)s_a164_alt_down;

    if (!simhp4_frontend_submit_char(ch)) {
        ESP_LOGW(TAG, "SIMHP4_R0A8 A164 -> frontend input queue full ch=%02x", (unsigned)ch);
    }
}

static void simhp4_a164_process_raw(uint8_t raw)
{
    if (raw == SIMHP4_A164_EVENT_EMPTY)
        return;

    const bool down = (raw & 0x80u) != 0u;
    const unsigned row = (raw >> 4) & 0x07u;
    const unsigned col = raw & 0x0Fu;

    if (row >= SIMHP4_A164_ROWS || col >= SIMHP4_A164_COLS)
        return;

    const unsigned idx = row * SIMHP4_A164_COLS + col;
    simhp4_a164_emit_key(idx, down);
}

static bool simhp4_a164_drain()
{
    bool hit = false;

    for (unsigned n = 0; n < 32u; ++n) {
        uint8_t raw = SIMHP4_A164_EVENT_EMPTY;
        if (!simhp4_a164_read(SIMHP4_A164_REG_KEY_EVENT, &raw, 1)) {
            ++s_a164_read_failures;
            return false;
        }

        s_a164_read_failures = 0;

        if (raw == SIMHP4_A164_EVENT_EMPTY)
            break;

        hit = true;
        simhp4_a164_process_raw(raw);
    }

    if (hit)
        (void)simhp4_a164_write8(SIMHP4_A164_REG_INT_STAT, 0u);

    return true;
}

static void simhp4_a164_task(void *arg)
{
    (void)arg;
    unsigned tick10 = 0u;

    for (;;) {
        if (!simhp4_a164_present()) {
            if ((tick10++ % 100u) == 0u)
                (void)simhp4_a164_detect_and_configure();

            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        (void)simhp4_a164_drain();

        if (s_a164_read_failures >= 10u) {
            simhp4_a164_reset_modifiers();
            s_a164_read_failures = 0;
            simhp4_a164_publish_present(false);
        }

        ++tick10;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool simhp4_a164_start()
{
    /* Capture the native tablet orientation before A164 changes anything. */
    s_a164_base_rotation = M5.Display.getRotation();

    if (!simhp4_a164_i2c_init()) {
        ESP_LOGW(TAG, "SIMHP4_R0A8 A164 host unavailable; USB HID remains active");
        return false;
    }

    /* Synchronous first probe: if the keyboard is already mounted, the first
     * branded frame is drawn in the correct 180-degree orientation. */
    (void)simhp4_a164_detect_and_configure();

    if (s_a164_task != NULL)
        return true;

    const BaseType_t ok = xTaskCreatePinnedToCore(
        simhp4_a164_task,
        "simhp4_a164",
        4096,
        NULL,
        3,
        &s_a164_task,
        0);

    if (ok != pdPASS) {
        s_a164_task = NULL;
        ESP_LOGE(TAG, "SIMHP4_R0A8 A164 CPU0 task create failed");
        return false;
    }

    ESP_LOGI(TAG,
             "SIMHP4_R0A8 A164 service READY CPU0 prio=3 direct KEY_EVENT/10ms present=%d",
             simhp4_a164_present() ? 1 : 0);
    return true;
}
struct simhp4_crt_ui_t {
    int term_x;
    int term_y;
    int term_w;
    int term_h;
    int cursor_x;
    int cursor_y;
    int cell_w;
    int line_h;
    float term_size;
};

static simhp4_crt_ui_t s_crt = {};

static uint16_t simhp4_crt_bg()
{
    return M5.Display.color565(0, 5, 2);
}

static uint16_t simhp4_crt_panel()
{
    return M5.Display.color565(0, 10, 5);
}

static uint16_t simhp4_crt_glow_outer()
{
    return M5.Display.color565(0, 42, 18);
}

static uint16_t simhp4_crt_glow_mid()
{
    return M5.Display.color565(0, 126, 56);
}

static uint16_t simhp4_crt_core()
{
    return M5.Display.color565(112, 255, 164);
}

static void simhp4_neon_text(int x, int y, const char *text, float size)
{
    const uint16_t outer = simhp4_crt_glow_outer();
    const uint16_t mid = simhp4_crt_glow_mid();
    const uint16_t core = simhp4_crt_core();

    M5.Display.setTextSize(size);

    M5.Display.setTextColor(outer);
    M5.Display.drawString(text, x - 2, y);
    M5.Display.drawString(text, x + 2, y);
    M5.Display.drawString(text, x, y - 2);
    M5.Display.drawString(text, x, y + 2);

    M5.Display.setTextColor(mid);
    M5.Display.drawString(text, x - 1, y);
    M5.Display.drawString(text, x + 1, y);
    M5.Display.drawString(text, x, y - 1);
    M5.Display.drawString(text, x, y + 1);

    M5.Display.setTextColor(core);
    M5.Display.drawString(text, x, y);
}

static void simhp4_neon_line(int x0, int y0, int x1, int y1)
{
    const uint16_t outer = simhp4_crt_glow_outer();
    const uint16_t core = simhp4_crt_core();

    M5.Display.drawLine(x0 - 1, y0, x1 - 1, y1, outer);
    M5.Display.drawLine(x0 + 1, y0, x1 + 1, y1, outer);
    M5.Display.drawLine(x0, y0 - 1, x1, y1 - 1, outer);
    M5.Display.drawLine(x0, y0 + 1, x1, y1 + 1, outer);
    M5.Display.drawLine(x0, y0, x1, y1, core);
}

static void simhp4_neon_rect(int x, int y, int w, int h)
{
    M5.Display.drawRect(x - 2, y - 2, w + 4, h + 4, simhp4_crt_glow_outer());
    M5.Display.drawRect(x - 1, y - 1, w + 2, h + 2, simhp4_crt_glow_mid());
    M5.Display.drawRect(x, y, w, h, simhp4_crt_core());
}

typedef struct {
    int x;
    int y;
    int w;
    int h;
    uint32_t last_generation;
    int last_connected;
    TickType_t next_refresh;
} simhp4_remote_pane_t;

static simhp4_remote_pane_t s_remote_pane = {};

static void simhp4_remote_pane_clear_body()
{
    if (s_remote_pane.w <= 0 || s_remote_pane.h <= 0)
        return;

    M5.Display.fillRect(s_remote_pane.x + 3,
                        s_remote_pane.y + 30,
                        s_remote_pane.w - 6,
                        s_remote_pane.h - 33,
                        simhp4_crt_bg());
}

static void simhp4_remote_pane_draw(bool force)
{
    if (s_remote_pane.w <= 0 || s_remote_pane.h <= 0)
        return;

    const TickType_t now = xTaskGetTickCount();
    if (!force && (int32_t)(now - s_remote_pane.next_refresh) < 0)
        return;
    s_remote_pane.next_refresh = now + pdMS_TO_TICKS(100);

    const int connected = simhp4_remote_tty_client_connected();
    const uint32_t generation = simhp4_remote_tty_console_generation();

    if (!force &&
        connected == s_remote_pane.last_connected &&
        generation == s_remote_pane.last_generation)
        return;

    s_remote_pane.last_connected = connected;
    s_remote_pane.last_generation = generation;

    M5.Display.fillRect(s_remote_pane.x,
                        s_remote_pane.y,
                        s_remote_pane.w,
                        s_remote_pane.h,
                        simhp4_crt_bg());
    simhp4_neon_rect(s_remote_pane.x,
                     s_remote_pane.y,
                     s_remote_pane.w,
                     s_remote_pane.h);

    simhp4_neon_text(s_remote_pane.x + 10,
                     s_remote_pane.y + 6,
                     "REMOTE TTY",
                     1.25f);

    if (!connected) {
        simhp4_remote_pane_clear_body();
        M5.Display.setTextSize(1.45f);
        M5.Display.setTextColor(simhp4_crt_glow_mid());
        const char *msg = "NOT CONNECTED";
        const int tw = M5.Display.textWidth(msg);
        const int tx = s_remote_pane.x + (s_remote_pane.w - tw) / 2;
        const int ty = s_remote_pane.y + s_remote_pane.h / 2;
        M5.Display.drawString(msg, tx, ty);
        return;
    }

    uint8_t bytes[1024];
    const size_t n = simhp4_remote_tty_console_snapshot(bytes, sizeof(bytes));

    M5.Display.setTextSize(1.05f);
    const int cell_w = M5.Display.textWidth("M") + 1;
    const int line_h = M5.Display.fontHeight() + 2;
    int cols = (s_remote_pane.w - 16) / cell_w;
    int rows = (s_remote_pane.h - 42) / line_h;

    if (cols < 8) cols = 8;
    if (cols > 44) cols = 44;
    if (rows < 3) rows = 3;
    if (rows > 20) rows = 20;

    static char lines[20][45];
    for (int r = 0; r < 20; ++r)
        for (int c = 0; c < 45; ++c)
            lines[r][c] = '\0';

    int row = 0;
    int col = 0;

    auto newline = [&]() {
        col = 0;
        ++row;
        if (row >= rows) {
            for (int r = 1; r < rows; ++r)
                std::memcpy(lines[r - 1], lines[r], sizeof(lines[r]));
            std::memset(lines[rows - 1], 0, sizeof(lines[rows - 1]));
            row = rows - 1;
        }
    };

    for (size_t i = 0; i < n; ++i) {
        const uint8_t ch = bytes[i] & 0x7f;

        if (ch == '\f') {
            for (int r = 0; r < rows; ++r)
                std::memset(lines[r], 0, sizeof(lines[r]));
            row = 0;
            col = 0;
            continue;
        }
        if (ch == '\r') {
            col = 0;
            continue;
        }
        if (ch == '\n') {
            newline();
            continue;
        }
        if (ch == '\b') {
            if (col > 0) {
                --col;
                lines[row][col] = '\0';
            }
            continue;
        }
        if (ch == '\t') {
            int spaces = 8 - (col & 7);
            while (spaces-- > 0) {
                if (col >= cols)
                    newline();
                lines[row][col++] = ' ';
                lines[row][col] = '\0';
            }
            continue;
        }
        if (ch < 040 || ch >= 0177)
            continue;

        if (col >= cols)
            newline();
        lines[row][col++] = (char)ch;
        lines[row][col] = '\0';
    }

    simhp4_remote_pane_clear_body();

    M5.Display.setTextSize(1.05f);
    M5.Display.setTextColor(simhp4_crt_core());

    const int tx = s_remote_pane.x + 8;
    const int ty = s_remote_pane.y + 32;
    for (int r = 0; r < rows; ++r) {
        if (lines[r][0] != '\0')
            M5.Display.drawString(lines[r], tx, ty + r * line_h);
    }
}

static void simhp4_draw_crt_chrome()
{
    const int W = M5.Display.width();
    const int H = M5.Display.height();
    const int margin = (H >= 600) ? 24 : 16;
    const int header_h = (H >= 600) ? 70 : 54;
    const int right_w = (W * 27) / 100;
    const int gap = (W >= 1000) ? 22 : 14;

    M5.Display.fillScreen(simhp4_crt_bg());

    simhp4_neon_rect(margin / 2,
                     margin / 2,
                     W - margin,
                     H - margin);

    simhp4_neon_text(margin + 12,
                     margin,
                     "SIMHP4   PDP-7   UNIX V0",
                     (H >= 600) ? 3.0f : 2.4f);

    /* SIMHP4_RELEASE_FINAL_2026
     * Keep the on-screen ownership statement scoped to the simhP4 integration.
     * Detailed upstream copyright/license notices remain in source/LICENSES. */
    const char *copyright1 = "simhP4 © 2026 simhP4 contributors";
    const char *copyright2 = "Open SIMH based";
    const float copyright_size = (H >= 600) ? 1.35f : 1.10f;
    M5.Display.setTextSize(copyright_size);
    const int cr1_w = M5.Display.textWidth(copyright1);
    simhp4_neon_text(W - margin - 12 - cr1_w,
                     margin + 4,
                     copyright1,
                     copyright_size);
    M5.Display.setTextSize(copyright_size);
    const int cr2_w = M5.Display.textWidth(copyright2);
    simhp4_neon_text(W - margin - 12 - cr2_w,
                     margin + ((H >= 600) ? 30 : 22),
                     copyright2,
                     copyright_size);

    const int sep_y = margin + header_h;
    simhp4_neon_line(margin, sep_y, W - margin, sep_y);

    const int right_x = W - margin - right_w;
    simhp4_neon_line(right_x - gap / 2,
                     sep_y,
                     right_x - gap / 2,
                     H - margin);

    s_crt.term_x = margin + 12;
    s_crt.term_y = sep_y + 14;
    s_crt.term_w = right_x - gap - s_crt.term_x;
    s_crt.term_h = H - margin - 14 - s_crt.term_y;
    s_crt.term_size = (H >= 600) ? 2.7f : 2.3f;

    M5.Display.setTextSize(s_crt.term_size);
    s_crt.cell_w = M5.Display.textWidth("M") + 2;
    s_crt.line_h = M5.Display.fontHeight() + ((H >= 600) ? 6 : 4);
    s_crt.cursor_x = s_crt.term_x + 4;
    s_crt.cursor_y = s_crt.term_y + 4;

    M5.Display.fillRect(s_crt.term_x,
                        s_crt.term_y,
                        s_crt.term_w,
                        s_crt.term_h,
                        simhp4_crt_bg());

    M5.Display.setScrollRect(s_crt.term_x,
                             s_crt.term_y,
                             s_crt.term_w,
                             s_crt.term_h,
                             simhp4_crt_bg());
    M5.Display.setTextScroll(false);

    const int rx = right_x + 8;
    const int ry = sep_y + 20;
    simhp4_neon_text(rx, ry,
                     simhp4_a164_present() ? "A164 KBD    ON / 180 DEG" : "A164 KBD    --",
                     1.35f);
    simhp4_neon_text(rx, ry + 30, "USB HID     ON", 1.35f);
    simhp4_neon_text(rx, ry + 60, "G2 LOCAL    CONSOLE", 1.35f);
    simhp4_neon_text(rx, ry + 90, "RB09        ATTACHED", 1.35f);
    simhp4_neon_text(rx, ry + 120, "CPU1        PDP-7 RUN", 1.35f);

    simhp4_neon_line(rx, ry + 150, W - margin - 10, ry + 150);

    const int art_y = ry + 164;
    const int art_h = H - margin - art_y - 12;
    if (art_h > 120) {
        s_remote_pane.x = rx;
        s_remote_pane.y = art_y;
        s_remote_pane.w = right_w - 24;
        s_remote_pane.h = art_h;
        s_remote_pane.last_generation = UINT32_MAX;
        s_remote_pane.last_connected = -1;
        s_remote_pane.next_refresh = 0;
        simhp4_remote_pane_draw(true);
    }

    ESP_LOGI(TAG,
             "SIMHP4_R0A9R11 UI local CRT + remote mini TTY W=%d H=%d terminal=%d,%d %dx%d cell=%dx%d",
             W, H,
             s_crt.term_x, s_crt.term_y,
             s_crt.term_w, s_crt.term_h,
             s_crt.cell_w, s_crt.line_h);
}

static void simhp4_crt_clear_terminal()
{
    M5.Display.fillRect(s_crt.term_x,
                        s_crt.term_y,
                        s_crt.term_w,
                        s_crt.term_h,
                        simhp4_crt_bg());
    s_crt.cursor_x = s_crt.term_x + 4;
    s_crt.cursor_y = s_crt.term_y + 4;
}

static void simhp4_crt_newline()
{
    s_crt.cursor_x = s_crt.term_x + 4;
    s_crt.cursor_y += s_crt.line_h;

    const int bottom = s_crt.term_y + s_crt.term_h - 4;
    if (s_crt.cursor_y + s_crt.line_h > bottom) {
        M5.Display.setScrollRect(s_crt.term_x,
                                 s_crt.term_y,
                                 s_crt.term_w,
                                 s_crt.term_h,
                                 simhp4_crt_bg());
        M5.Display.scroll(0, -s_crt.line_h);
        s_crt.cursor_y -= s_crt.line_h;
    }
}

static void simhp4_crt_draw_char(uint8_t ch)
{
    if (s_crt.cursor_x + s_crt.cell_w >
        s_crt.term_x + s_crt.term_w - 4) {
        simhp4_crt_newline();
    }

    const int x = s_crt.cursor_x;
    const int y = s_crt.cursor_y;

    M5.Display.fillRect(x,
                        y,
                        s_crt.cell_w,
                        s_crt.line_h,
                        simhp4_crt_bg());

    M5.Display.setTextSize(s_crt.term_size);

    M5.Display.setTextColor(simhp4_crt_glow_outer());
    M5.Display.drawChar(ch, x - 2, y);
    M5.Display.drawChar(ch, x + 2, y);
    M5.Display.drawChar(ch, x, y - 2);
    M5.Display.drawChar(ch, x, y + 2);

    M5.Display.setTextColor(simhp4_crt_glow_mid());
    M5.Display.drawChar(ch, x - 1, y);
    M5.Display.drawChar(ch, x + 1, y);
    M5.Display.drawChar(ch, x, y - 1);
    M5.Display.drawChar(ch, x, y + 1);

    M5.Display.setTextColor(simhp4_crt_core());
    M5.Display.drawChar(ch, x, y);

    s_crt.cursor_x += s_crt.cell_w;
}

static void simhp4_crt_putc(uint8_t ch)
{
    switch (ch) {
    case '\f':
        simhp4_crt_clear_terminal();
        break;

    case '\r':
        s_crt.cursor_x = s_crt.term_x + 4;
        break;

    case '\n':
        simhp4_crt_newline();
        break;

    case '\b':
        if (s_crt.cursor_x > s_crt.term_x + 4) {
            s_crt.cursor_x -= s_crt.cell_w;
            M5.Display.fillRect(s_crt.cursor_x,
                                s_crt.cursor_y,
                                s_crt.cell_w,
                                s_crt.line_h,
                                simhp4_crt_bg());
        }
        break;

    case '\t': {
        const int col = (s_crt.cursor_x - (s_crt.term_x + 4)) / s_crt.cell_w;
        const int spaces = 8 - (col & 7);
        for (int i = 0; i < spaces; ++i)
            simhp4_crt_draw_char(' ');
        break;
    }

    default:
        if (ch >= 040 && ch < 0177)
            simhp4_crt_draw_char(ch);
        break;
    }
}

static uint32_t s_type340_last_generation = UINT32_MAX;
static bool s_type340_was_active = false;
/* SIMHP4_R0A9R19_DIFF_PRESENTER
 * CPU0 keeps the last presented 8-bit intensity image in PSRAM.  Subsequent
 * frames update only changed pixels instead of clearing the whole G2 pane. */
static uint8_t *s_type340_present_shadow = nullptr;
static bool s_type340_present_shadow_valid = false;
static bool s_type340_present_shadow_warned = false;

static void simhp4_type340_present(bool force)
{
    const bool active = simhp4_type340_surface_is_active() != 0;

    if (!active) {
        if (s_type340_was_active) {
            s_type340_was_active = false;
            s_type340_last_generation = UINT32_MAX;
            s_type340_present_shadow_valid = false;

            /* G2OUT forces a full text-screen resend when the captured
             * Type 340 program releases the display.  Restore chrome first. */
            simhp4_draw_crt_chrome();
            ESP_LOGI(TAG,
                     "SIMHP4_R0A9R13 TYPE340 released; local G2 text presenter restored");
        }
        return;
    }

    uint32_t generation = 0;
    const uint8_t *frame = simhp4_type340_surface_acquire(&generation);
    if (frame == nullptr)
        return;

    if (!force &&
        s_type340_was_active &&
        generation == s_type340_last_generation) {
        simhp4_type340_surface_release();
        return;
    }

    s_type340_was_active = true;
    s_type340_last_generation = generation;

    const int gw = SIMHP4_TYPE340_SURFACE_W;
    const int gh = SIMHP4_TYPE340_SURFACE_H;
    const int gx = s_crt.term_x + (s_crt.term_w - gw) / 2;
    const int gy = s_crt.term_y + (s_crt.term_h - gh) / 2;

    uint16_t pal[8];
    pal[0] = simhp4_crt_bg();
    pal[1] = M5.Display.color565(0, 32, 12);
    pal[2] = M5.Display.color565(0, 54, 20);
    pal[3] = M5.Display.color565(0, 78, 28);
    pal[4] = M5.Display.color565(0, 106, 38);
    pal[5] = M5.Display.color565(0, 138, 52);
    pal[6] = M5.Display.color565(24, 188, 82);
    pal[7] = simhp4_crt_core();

    const size_t frame_bytes = (size_t)gw * (size_t)gh;
    if (s_type340_present_shadow == nullptr) {
        s_type340_present_shadow = (uint8_t *)heap_caps_malloc(
            frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_type340_present_shadow != nullptr) {
            memset(s_type340_present_shadow, 0, frame_bytes);
            ESP_LOGI(TAG,
                     "SIMHP4_R0A9R19 graphics diff shadow READY bytes=%u PSRAM",
                     (unsigned)frame_bytes);
        } else if (!s_type340_present_shadow_warned) {
            s_type340_present_shadow_warned = true;
            ESP_LOGW(TAG,
                     "SIMHP4_R0A9R19 graphics diff shadow allocation failed; using full redraw fallback");
        }
    }

    const bool diff_ok = !force &&
                         s_type340_present_shadow != nullptr &&
                         s_type340_present_shadow_valid;

    M5.Display.startWrite();
    if (!diff_ok) {
        /* First graphics frame (or allocation fallback): clear exactly once. */
        M5.Display.fillRect(s_crt.term_x,
                            s_crt.term_y,
                            s_crt.term_w,
                            s_crt.term_h,
                            simhp4_crt_bg());
    }

    for (int y = 0; y < gh; ++y) {
        const uint8_t *row = frame + (size_t)y * (size_t)gw;
        const uint8_t *oldrow = s_type340_present_shadow != nullptr
                              ? s_type340_present_shadow + (size_t)y * (size_t)gw
                              : nullptr;
        for (int x = 0; x < gw; ++x) {
            const uint8_t v = row[x] & 7u;
            if (diff_ok && v == (oldrow[x] & 7u))
                continue;
            if (v != 0 || diff_ok)
                M5.Display.drawPixel(gx + x, gy + y, pal[v]);
        }
    }
    M5.Display.endWrite();

    if (s_type340_present_shadow != nullptr) {
        memcpy(s_type340_present_shadow, frame, frame_bytes);
        s_type340_present_shadow_valid = true;
    }

    simhp4_type340_surface_release();

    if (generation == 1) {
        ESP_LOGI(TAG,
                 "SIMHP4_R0A9R13 TYPE340 first frame presented %dx%d inside local G2 pane",
                 gw, gh);
    }
}

static void simhp4_g2_display_task(void *arg)
{
    (void)arg;
    bool shell_announced = false;
    uint32_t a164_generation = simhp4_a164_ui_generation();

    M5.Display.setRotation(simhp4_a164_target_rotation());

    /* Static chrome and the REMOTE TTY mini console are drawn after the
     * final physical orientation is known. */
    simhp4_draw_crt_chrome();

    for (;;) {
        uint8_t ch = 0;
        /* SIMHP4_R0A9R19C_GRAPHICS_CADENCE
         * Captured graphics publishes at ~24 guest loops/s, while the old
         * 50ms host wait capped observation/presentation near 10Hz after draw
         * cost.  Poll graphics generation every 10ms only while captured;
         * keep the historical 50ms blocking wait for ordinary text mode. */
        const uint32_t g2_wait_ms =
            simhp4_type340_surface_is_active() ? 10u : 50u;
        const int got_g2 = simhp4_g2_host_dequeue_timed(&ch, g2_wait_ms);

        const uint32_t gen_now = simhp4_a164_ui_generation();
        if (gen_now != a164_generation) {
            a164_generation = gen_now;
            M5.Display.setRotation(simhp4_a164_target_rotation());
            simhp4_draw_crt_chrome();
            ESP_LOGI(TAG,
                     "SIMHP4_R0A8 UI redrawn after A164 hotplug orientation=%u present=%d",
                     (unsigned)M5.Display.getRotation(),
                     simhp4_a164_present() ? 1 : 0);
        }

        simhp4_remote_pane_draw(false);
        simhp4_type340_present(false);

        if (!got_g2)
            continue;

        if (!simhp4_type340_surface_is_active())
            simhp4_crt_putc(ch);

        if (!shell_announced && simhp4_g2_watch_hit()) {
            shell_announced = true;
            ESP_LOGI(TAG,
                     "SIMHP4_R0A7 PASS: live UNIX V0 GRAPHICS-2 shell @ reached; neon CRT presenter remains live");
        }
    }
}

static void simhp4_guest_task(void *arg)
{
    (void)arg;
    simhp4_unix_result_t result = {};
    result.core = xPortGetCoreID();

    ESP_LOGI(TAG, "SIMHP4_R0A5 guest task entered core=%d", result.core);
    simhp4_runtime_prepare();
    ESP_LOGI(TAG, "SIMHP4_R0A5 guest runtime prepared core=%d", result.core);

    result.ok = simhp4_pdp7_unixv0_g2_interactive(
        UNIX_BOOT_PATH,
        UNIX_DISK_PATH,
        &result.disk_load_rc,
        &result.disk_words,
        &result.boot_load_rc,
        &result.boot_words,
        &result.g2_attach_rc,
        &result.reason,
        &result.pc,
        &result.rb_status,
        &result.shell_seen);

    const char *console = simhp4_g2_capture_get();
    if (console != nullptr) {
        const size_t len = std::strlen(console);
        const char *tail = console;
        if (len >= sizeof(result.console_tail))
            tail = console + len - (sizeof(result.console_tail) - 1);
        std::snprintf(result.console_tail,
                      sizeof(result.console_tail),
                      "%s", tail);
    }

    ESP_LOGI(TAG,
             "SIMHP4_R0A5 guest RETURNED unexpectedly core=%d ok=%d disk_rc=%d disk_words=%d boot_rc=%d boot_words=%d g2_rc=%d shell=%d reason=%d pc=%06o rb_sta=%06o keys=%u g2_in_drop=%u g2_out_drop=%u",
             result.core, result.ok,
             result.disk_load_rc, result.disk_words,
             result.boot_load_rc, result.boot_words,
             result.g2_attach_rc, result.shell_seen, result.reason,
             static_cast<unsigned>(result.pc),
             static_cast<unsigned>(result.rb_status),
             simhp4_usb_keyboard_key_count(),
             static_cast<unsigned>(simhp4_g2_input_drop_count()),
             static_cast<unsigned>(simhp4_g2_output_drop_count()));

    if (s_result_queue != nullptr)
        xQueueSend(s_result_queue, &result, portMAX_DELAY);

    vTaskDelete(nullptr);
}

extern "C" void app_main(void)
{
    auto cfg = M5.config();
    M5.begin(cfg);

    if (M5.Display.width() < M5.Display.height())
        M5.Display.setRotation(1);

    M5.Display.setSwapBytes(true);
    M5.Display.setBrightness(255);
    M5.Display.fillScreen(0x0000);
    M5.Display.setTextColor(0xFFFF, 0x0000);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(24, 24);

    if (!simhp4_frontend_input_init()) {
        ESP_LOGE(TAG, "S0 frontend input queue allocation failed");
        M5.Display.println("FAIL: selector input");
        for (;;) {
            M5.update();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    /* Arm selector ownership before USB/A164 tasks can publish a key. */
    simhp4_frontend_set_selector_active(1);

    const char *probe = simhp4_pdp7_compile_probe_version();
    const char *machine = simhp4_pdp7_machine_name();
    const char *runtime = simhp4_runtime_profile();
    const int devices = simhp4_pdp7_linked_device_count();

    const unsigned char *image_embed_data = simhp4_unix_image_data();
    const size_t image_embed_size = simhp4_unix_image_size();
    const unsigned char *boot_embed_data = simhp4_unix_boot_data();
    const size_t boot_embed_size = simhp4_unix_boot_size();

    const int sd_ok = simhp4_sd_mount();
    const int disk_seed_ok = sd_ok ?
        write_embedded_file(UNIX_DISK_PATH,
                            image_embed_data,
                            image_embed_data + image_embed_size,
                            UNIX_IMAGE_EXPECTED_SIZE) : 0;
    const int boot_seed_ok = sd_ok ?
        write_embedded_file(UNIX_BOOT_PATH,
                            boot_embed_data,
                            boot_embed_data + boot_embed_size,
                            UNIX_BOOT_EXPECTED_SIZE) : 0;

    /* SIMHP4_R0A9R15_SPACE_TRAVEL_LAYOUT_FIX
     * R14E proved the exact historical bad st layout.  Replace system/st only
     * when the entire 2927-word file matches that exact known-bad binary. */
    const int st_layout_state = (sd_ok && disk_seed_ok) ?
        simhp4_st_migrate_bad_layout(UNIX_DISK_PATH) : -1;
    ESP_LOGI(TAG,
             "SIMHP4_R0A9R15 st_layout_state=%d (2=replaced 1=already-good 0=skip -1=error)",
             st_layout_state);

    const int g2_local_ok = simhp4_g2_local_init();
    const int type340_ok = g2_local_ok ? simhp4_type340_surface_init() : 0;
    const int a164_task_ok = g2_local_ok ? simhp4_a164_start() : 0;
    const int usb_kbd_ok = g2_local_ok ? simhp4_usb_keyboard_start() : 0;

    /* R0A9 starts the primary TTI/TTO network bridge asynchronously.
     * A WiFi/C6 failure must never prevent the local G2 seat from booting. */
    const int remote_tty_task_ok = simhp4_remote_tty_start();

    ESP_LOGI(TAG,
             "SIMHP4_R0A8 input sources A164_task=%d A164_present=%d USB_HID=%d",
             a164_task_ok,
             simhp4_a164_present() ? 1 : 0,
             usb_kbd_ok);
    ESP_LOGI(TAG,
             "SIMHP4_R0A9 remote tty task=%d AP=SIMHP4-UNIXV0 IP=192.168.4.1 port=10007 local_G2_unchanged=1",
             remote_tty_task_ok);
    ESP_LOGI(TAG,
             "SIMHP4 RELEASE FINAL GRAPHIC-II surface=%d; held A164 3..6/arrows; edge 7/8/Z/X; diff presenter + R19C cadence",
             type340_ok);

    ESP_LOGI(TAG,
             "SIMHP4_R0A5 host core=%d probe=%s machine=%s devices=%d runtime=%s commit=%s sd=%d disk_seed=%d boot_seed=%d embedded_image=%u embedded_boot=%u",
             xPortGetCoreID(), probe, machine, devices, runtime,
             SIMHP4_SIMH_COMMIT, sd_ok, disk_seed_ok, boot_seed_ok,
             (unsigned)image_embed_size, (unsigned)boot_embed_size);
    ESP_LOGI(TAG,
             "SIMHP4_R0A5 local seat core=%d g2_local=%d usb_hid=%d policy=CPU0-HID->local-TMXR->CPU1-G2IN;CPU1-G2OUT->queue->CPU0-M5GFX",
             xPortGetCoreID(), g2_local_ok, usb_kbd_ok);

    const simhp4_selector_choice_t boot_choice = simhp4_selector_run();
    ESP_LOGI(TAG,
             "SIMHP4_S0 selector machine=%d os=%d",
             (int)boot_choice.machine, (int)boot_choice.os);

    if (boot_choice.machine == SIMHP4_MACHINE_MICROVAX2) {
        ESP_LOGI(TAG,
                 "SIMHP4_S0 MicroVAX II / 4.3BSD selected; VAX core is intentionally not started in selector phase");
        simhp4_selector_show_not_ready(boot_choice);
        for (;;) {
            M5.update();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    M5.Display.fillScreen(0x0000);
    M5.Display.setTextColor(0xFFFF, 0x0000);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(24, 24);

    M5.Display.println("RetroP4 SIMH");
    M5.Display.println("R0A5 / UNIX V0 LIVE");
    M5.Display.println("");
    M5.Display.printf("Machine: %s\n", machine);
    M5.Display.printf("Devices: %d\n", devices);
    M5.Display.printf("SD: %s\n", sd_ok ? "mounted" : "FAIL");
    M5.Display.printf("UNIX disk: %s\n", disk_seed_ok ? "ready" : "FAIL");
    M5.Display.printf("boot.rim: %s\n", boot_seed_ok ? "ready" : "FAIL");
    M5.Display.printf("G2 local: %s\n", g2_local_ok ? "ready" : "FAIL");
    M5.Display.printf("Type340: %s\n", type340_ok ? "ready" : "FAIL");
    M5.Display.printf("Space Travel: %s\n",
                      st_layout_state == 2 ? "fixed" :
                      st_layout_state == 1 ? "layout ready" :
                      st_layout_state == 0 ? "layout unknown" : "migration error");
    M5.Display.printf("USB HID: %s\n", usb_kbd_ok ? "host ready" : "FAIL");
    M5.Display.println("");

    if (!sd_ok || !disk_seed_ok || !boot_seed_ok ||
        !g2_local_ok || !type340_ok || !usb_kbd_ok) {
        M5.Display.println("R0A1F: MEDIA FAIL");
        ESP_LOGE(TAG,
                 "R0A9R13 host setup failure sd=%d disk=%d boot=%d g2=%d type340=%d usb=%d",
                 sd_ok, disk_seed_ok, boot_seed_ok,
                 g2_local_ok, type340_ok, usb_kbd_ok);
        for (;;) {
            M5.update();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    s_result_queue = xQueueCreate(1, sizeof(simhp4_unix_result_t));
    if (s_result_queue == nullptr) {
        M5.Display.println("FAIL: result queue");
        ESP_LOGE(TAG, "R0A1F result queue allocation failed");
        for (;;) {
            M5.update();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
/* CPU1 is deliberately dedicated to continuously-running PDP-7 execution.
     *
     * Do NOT manually delete IDLE1 from TWDT.  ESP-IDF registers a per-core
     * idle hook together with the idle subscription; deleting only the task
     * entry leaves that hook calling esp_task_wdt_reset(), which produces an
     * endless "task not found" flood.
     *
     * Use the public reconfigure API instead.  This updates the idle-core mask
     * and, inside ESP-IDF 5.5.4, unsubscribes IDLE1 AND deregisters its idle
     * hook as one operation.  TWDT remains enabled and IDLE0 remains watched. */
    esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = (1U << 0),
#if CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#else
        .trigger_panic = false,
#endif
    };

    esp_err_t twdt_reconfig = esp_task_wdt_reconfigure(&twdt_cfg);
    ESP_LOGI(TAG,
             "SIMHP4_R0A5R2 TWDT reconfigure idle_core_mask=0x%02x rc=%s",
             static_cast<unsigned>(twdt_cfg.idle_core_mask),
             esp_err_to_name(twdt_reconfig));

    /* Start the CPU1 guest FIRST.
     *
     * R0A3 started the higher-priority CPU0 presenter first.  Its 1 ms delay
     * could collapse to a zero-tick yield, allowing it to starve app_main
     * before the CPU1 task was created.  The observed state was exactly:
     * CPU0=simhp4_g2_displ, CPU1=IDLE1, followed by a full G2 input FIFO.
     *
     * R0A4 removed that ordering hazard; R0A5 keeps it. */
    BaseType_t created = xTaskCreatePinnedToCore(
        simhp4_guest_task,
        "simh_pdp7_unixv0",
        16384,
        nullptr,
        5,
        nullptr,
        1);

    if (created != pdPASS) {
        M5.Display.println("FAIL: CPU1 task");
        ESP_LOGE(TAG, "R0A4 CPU1 guest task creation failed");
    } else {
        ESP_LOGI(TAG, "SIMHP4_R0A5 CPU1 guest task created");

        BaseType_t display_created = xTaskCreatePinnedToCore(
            simhp4_g2_display_task,
            "simhp4_g2_display",
            4096,
            nullptr,
            2,
            nullptr,
            0);

        if (display_created != pdPASS) {
            ESP_LOGE(TAG, "R0A5 G2 display task creation failed");
            for (;;) {
                M5.update();
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        ESP_LOGI(TAG, "SIMHP4_R0A5 CPU0 G2 display task created (blocking queue)");

        simhp4_unix_result_t result = {};
        if (xQueueReceive(s_result_queue, &result, portMAX_DELAY) == pdTRUE) {
            ESP_LOGE(TAG, "SIMHP4_R0A5 GUEST STOPPED - this is not a shell PASS");
            ESP_LOGI(TAG, "SIMHP4_R0A5 G2 capture tail begin");
            std::printf("%s\n", result.console_tail);
            ESP_LOGI(TAG, "SIMHP4_R0A5 G2 capture tail end");
            ESP_LOGE(TAG,
                     "SIMHP4_R0A5 FAIL: guest returned disk_rc=%d boot_rc=%d g2_rc=%d shell_seen=%d reason=%d pc=%06o rb_sta=%06o keys=%u in_drop=%u out_drop=%u",
                     result.disk_load_rc, result.boot_load_rc,
                     result.g2_attach_rc, result.shell_seen, result.reason,
                     static_cast<unsigned>(result.pc),
                     static_cast<unsigned>(result.rb_status),
                     simhp4_usb_keyboard_key_count(),
                     static_cast<unsigned>(simhp4_g2_input_drop_count()),
                     static_cast<unsigned>(simhp4_g2_output_drop_count()));
        } else {
            ESP_LOGE(TAG, "R0A5 result queue failure");
            ESP_LOGE(TAG,
                     "SIMHP4_R0A5 unexpected result-queue wait failure");
        }
    }

    for (;;) {
        M5.update();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}






