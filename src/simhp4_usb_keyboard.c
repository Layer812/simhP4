#include "simhp4_usb_keyboard.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

#include "simhp4_probe.h"

/*
 * SIMHP4 R0A2
 *
 * This is the keyboard-only slice of the same architecture used by RetroP4 /
 * X68K Tab:
 *
 *   CPU0 USB Host library task
 *   CPU0 HID driver background task
 *   CPU0 control task
 *       -> Boot Keyboard report
 *       -> ASCII/control byte
 *       -> simhp4_g2_host_enqueue()
 *   CPU1 unmodified pdp18b_g2tty.c
 *       -> tmxr_getc_ln() local endpoint
 *       -> device 043 GRAPHICS-2 keyboard IOT/interrupt
 *
 * USB tasks never touch PDP-7 registers, interrupt flags or UNIT state.
 */

#define SIMHP4_USB_CTRL_QUEUE_DEPTH 8
#define SIMHP4_USB_LIB_TASK_PRIO    6
#define SIMHP4_USB_HID_TASK_PRIO    6
#define SIMHP4_USB_CTRL_TASK_PRIO   5
#define SIMHP4_USB_REPORT_MAX       64

static const char *TAG = "SIMHP4_USB_KBD";

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} simhp4_usb_ctrl_event_t;

static QueueHandle_t s_ctrl_queue = NULL;
static hid_host_device_handle_t s_keyboard_handle = NULL;
static uint8_t s_prev_keys[HID_KEYBOARD_KEY_MAX];
static volatile unsigned s_key_count = 0;

static bool key_found(const uint8_t *src, uint8_t key, unsigned length)
{
    unsigned i;
    for (i = 0; i < length; ++i)
        if (src[i] == key)
            return true;
    return false;
}

static bool modifier_shift(uint8_t modifier)
{
    return ((modifier & HID_LEFT_SHIFT) != 0) ||
           ((modifier & HID_RIGHT_SHIFT) != 0);
}

static bool modifier_ctrl(uint8_t modifier)
{
    return ((modifier & HID_LEFT_CONTROL) != 0) ||
           ((modifier & HID_RIGHT_CONTROL) != 0);
}

/* Same boot-keyboard ASCII table used by the Espressif IDF 5.5.4 HID example. */
static const uint8_t s_keycode2ascii[57][2] = {
    {0,0},{0,0},{0,0},{0,0},
    {'a','A'},{'b','B'},{'c','C'},{'d','D'},{'e','E'},{'f','F'},
    {'g','G'},{'h','H'},{'i','I'},{'j','J'},{'k','K'},{'l','L'},
    {'m','M'},{'n','N'},{'o','O'},{'p','P'},{'q','Q'},{'r','R'},
    {'s','S'},{'t','T'},{'u','U'},{'v','V'},{'w','W'},{'x','X'},
    {'y','Y'},{'z','Z'},
    {'1','!'},{'2','@'},{'3','#'},{'4','$'},{'5','%'},{'6','^'},
    {'7','&'},{'8','*'},{'9','('},{'0',')'},
    {'\r','\r'},       /* ENTER */
    {0x1b,0x1b},       /* ESC */
    {'\b','\b'},       /* BACKSPACE / HID_KEY_DEL */
    {'\t','\t'},       /* TAB */
    {' ',' '},
    {'-','_'},{'=','+'},{'[','{'},{']','}'},{'\\','|'},
    {'\\','|'},        /* Non-US #/~ position */
    {';',':'},{'\'','"'},{'`','~'},{',','<'},{'.','>'},{'/','?'}
};

static int keycode_to_char(uint8_t modifier, uint8_t keycode, uint8_t *out)
{
    uint8_t ch;
    unsigned shifted = modifier_shift(modifier) ? 1u : 0u;

    if (keycode >= (uint8_t)(sizeof(s_keycode2ascii) / sizeof(s_keycode2ascii[0])))
        return 0;

    ch = s_keycode2ascii[keycode][shifted];
    if (ch == 0)
        return 0;

    if (modifier_ctrl(modifier)) {
        if ((ch >= 'a') && (ch <= 'z'))
            ch = (uint8_t)(ch - 'a' + 1);
        else if ((ch >= 'A') && (ch <= 'Z'))
            ch = (uint8_t)(ch - 'A' + 1);
    }

    *out = ch;
    return 1;
}

static void process_boot_keyboard_report(const uint8_t *data, size_t length)
{
    const hid_keyboard_input_report_boot_t *report;
    unsigned i;

    if (data == NULL || length < sizeof(hid_keyboard_input_report_boot_t))
        return;

    report = (const hid_keyboard_input_report_boot_t *)data;

    for (i = 0; i < HID_KEYBOARD_KEY_MAX; ++i) {
        uint8_t code = report->key[i];
        uint8_t ch = 0;

        if (code <= HID_KEY_ERROR_UNDEFINED)
            continue;
        if (key_found(s_prev_keys, code, HID_KEYBOARD_KEY_MAX))
            continue;

        if (keycode_to_char(report->modifier.val, code, &ch)) {
            if (simhp4_g2_host_enqueue(ch)) {
                ++s_key_count;
            } else {
                ESP_LOGW(TAG, "G2 input FIFO full; key dropped");
            }
        }
    }

    memcpy(s_prev_keys, report->key, HID_KEYBOARD_KEY_MAX);
}

static void interface_callback(hid_host_device_handle_t handle,
                               hid_host_interface_event_t event,
                               void *arg)
{
    hid_host_dev_params_t params;
    (void)arg;

    if (hid_host_device_get_params(handle, &params) != ESP_OK)
        return;

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[SIMHP4_USB_REPORT_MAX];
        size_t length = 0;
        if ((params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) &&
            (params.proto == HID_PROTOCOL_KEYBOARD) &&
            (hid_host_device_get_raw_input_report_data(
                 handle, data, sizeof(data), &length) == ESP_OK)) {
            process_boot_keyboard_report(data, length);
        }
        break;
    }

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        if (handle == s_keyboard_handle) {
            s_keyboard_handle = NULL;
            memset(s_prev_keys, 0, sizeof(s_prev_keys));
            ESP_LOGI(TAG, "USB HID Boot Keyboard DISCONNECTED");
        }
        (void)hid_host_device_close(handle);
        break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "USB HID keyboard transfer error");
        break;

    default:
        break;
    }
}

static void driver_callback(hid_host_device_handle_t handle,
                            hid_host_driver_event_t event,
                            void *arg)
{
    simhp4_usb_ctrl_event_t e;
    (void)arg;

    if (s_ctrl_queue == NULL)
        return;

    e.handle = handle;
    e.event = event;
    (void)xQueueSend(s_ctrl_queue, &e, 0);
}

static void usb_control_task(void *arg)
{
    simhp4_usb_ctrl_event_t e;
    (void)arg;

    for (;;) {
        if (xQueueReceive(s_ctrl_queue, &e, portMAX_DELAY) != pdTRUE)
            continue;

        if (e.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
            hid_host_dev_params_t params;

            if (hid_host_device_get_params(e.handle, &params) != ESP_OK)
                continue;

            /* R0A2 deliberately opens only a real Boot Keyboard interface. */
            if ((params.sub_class != HID_SUBCLASS_BOOT_INTERFACE) ||
                (params.proto != HID_PROTOCOL_KEYBOARD)) {
                continue;
            }

            if (s_keyboard_handle != NULL) {
                ESP_LOGW(TAG, "Second USB keyboard ignored");
                continue;
            }

            {
                const hid_host_device_config_t cfg = {
                    .callback = interface_callback,
                    .callback_arg = NULL
                };

                esp_err_t err = hid_host_device_open(e.handle, &cfg);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "hid_host_device_open failed: %s",
                             esp_err_to_name(err));
                    continue;
                }

                err = hid_class_request_set_protocol(
                    e.handle, HID_REPORT_PROTOCOL_BOOT);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "set BOOT protocol failed: %s",
                             esp_err_to_name(err));
                    (void)hid_host_device_close(e.handle);
                    continue;
                }

                /* Match the proven RetroP4/X68K direct keyboard path. */
                (void)hid_class_request_set_idle(e.handle, 0, 0);

                err = hid_host_device_start(e.handle);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "hid_host_device_start failed: %s",
                             esp_err_to_name(err));
                    (void)hid_host_device_close(e.handle);
                    continue;
                }
            }

            memset(s_prev_keys, 0, sizeof(s_prev_keys));
            s_keyboard_handle = e.handle;
            ESP_LOGI(TAG, "USB HID Boot Keyboard CONNECTED");
        }
    }
}

static void usb_library_task(void *arg)
{
    TaskHandle_t starter = (TaskHandle_t)arg;
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_cfg);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(err));
        xTaskNotifyGive(starter);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "USB Host library installed");
    xTaskNotifyGive(starter);

    for (;;) {
        uint32_t flags = 0;
        (void)usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

int simhp4_usb_keyboard_start(void)
{
    BaseType_t task_ok;
    hid_host_driver_config_t hid_cfg;

    if (s_ctrl_queue != NULL)
        return 1;

    memset(s_prev_keys, 0, sizeof(s_prev_keys));
    s_keyboard_handle = NULL;
    s_key_count = 0;

    s_ctrl_queue = xQueueCreate(
        SIMHP4_USB_CTRL_QUEUE_DEPTH,
        sizeof(simhp4_usb_ctrl_event_t));
    if (s_ctrl_queue == NULL) {
        ESP_LOGE(TAG, "USB control queue allocation failed");
        return 0;
    }

    task_ok = xTaskCreatePinnedToCore(
        usb_control_task,
        "simhp4_usb_ctrl",
        4096,
        NULL,
        SIMHP4_USB_CTRL_TASK_PRIO,
        NULL,
        0);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "USB control task creation failed");
        return 0;
    }

    task_ok = xTaskCreatePinnedToCore(
        usb_library_task,
        "simhp4_usb_lib",
        4096,
        xTaskGetCurrentTaskHandle(),
        SIMHP4_USB_LIB_TASK_PRIO,
        NULL,
        0);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "USB library task creation failed");
        return 0;
    }

    /* Wait only for install completion; HID handling remains asynchronous. */
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) == 0) {
        ESP_LOGE(TAG, "USB Host install timeout");
        return 0;
    }

    memset(&hid_cfg, 0, sizeof(hid_cfg));
    hid_cfg.create_background_task = true;
    hid_cfg.task_priority = SIMHP4_USB_HID_TASK_PRIO;
    hid_cfg.stack_size = 4096;
    hid_cfg.core_id = 0;
    hid_cfg.callback = driver_callback;
    hid_cfg.callback_arg = NULL;

    {
        esp_err_t err = hid_host_install(&hid_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HID Host install failed: %s", esp_err_to_name(err));
            return 0;
        }
    }

    ESP_LOGI(TAG,
             "USB HID host ready on CPU0; keys route to GRAPHICS-2 device 043");
    return 1;
}

int simhp4_usb_keyboard_connected(void)
{
    return (s_keyboard_handle != NULL) ? 1 : 0;
}

unsigned simhp4_usb_keyboard_key_count(void)
{
    return s_key_count;
}
