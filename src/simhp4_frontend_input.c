#include "simhp4_frontend_input.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "simhp4_probe.h"

#define SIMHP4_FRONTEND_QUEUE_DEPTH 16

static QueueHandle_t s_frontend_queue = NULL;
static volatile int s_selector_active = 0;

int simhp4_frontend_input_init(void)
{
    if (s_frontend_queue == NULL)
        s_frontend_queue = xQueueCreate(SIMHP4_FRONTEND_QUEUE_DEPTH,
                                        sizeof(simhp4_frontend_key_t));
    return s_frontend_queue != NULL ? 1 : 0;
}

void simhp4_frontend_set_selector_active(int active)
{
    if (active && s_frontend_queue != NULL)
        xQueueReset(s_frontend_queue);
    __atomic_store_n(&s_selector_active, active ? 1 : 0, __ATOMIC_RELEASE);
}

int simhp4_frontend_selector_active(void)
{
    return __atomic_load_n(&s_selector_active, __ATOMIC_ACQUIRE);
}

static int simhp4_frontend_queue_key(simhp4_frontend_key_t key)
{
    if (s_frontend_queue == NULL || key == SIMHP4_FRONTEND_KEY_NONE)
        return 0;
    return xQueueSend(s_frontend_queue, &key, 0) == pdTRUE ? 1 : 0;
}

int simhp4_frontend_submit_nav(simhp4_frontend_key_t key)
{
    if (!simhp4_frontend_selector_active())
        return 0;
    return simhp4_frontend_queue_key(key);
}

int simhp4_frontend_submit_char(uint8_t ch)
{
    if (!simhp4_frontend_selector_active())
        return simhp4_g2_host_enqueue(ch);

    switch (ch) {
    case '\r':
    case '\n':
        return simhp4_frontend_queue_key(SIMHP4_FRONTEND_KEY_ENTER);
    case 0x1b:
        return simhp4_frontend_queue_key(SIMHP4_FRONTEND_KEY_ESCAPE);
    case '\b':
    case 0x7f:
        return simhp4_frontend_queue_key(SIMHP4_FRONTEND_KEY_BACK);
    default:
        return 1; /* ordinary text is intentionally ignored by the selector */
    }
}

int simhp4_frontend_wait_key(simhp4_frontend_key_t *key, uint32_t timeout_ms)
{
    if (key == NULL || s_frontend_queue == NULL)
        return 0;

    TickType_t ticks = timeout_ms == 0
                     ? 0
                     : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_frontend_queue, key, ticks) == pdTRUE ? 1 : 0;
}

void simhp4_frontend_flush(void)
{
    if (s_frontend_queue != NULL)
        xQueueReset(s_frontend_queue);
}
