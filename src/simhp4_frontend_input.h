#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SIMHP4_FRONTEND_KEY_NONE = 0,
    SIMHP4_FRONTEND_KEY_UP,
    SIMHP4_FRONTEND_KEY_DOWN,
    SIMHP4_FRONTEND_KEY_LEFT,
    SIMHP4_FRONTEND_KEY_RIGHT,
    SIMHP4_FRONTEND_KEY_ENTER,
    SIMHP4_FRONTEND_KEY_ESCAPE,
    SIMHP4_FRONTEND_KEY_BACK
} simhp4_frontend_key_t;

int simhp4_frontend_input_init(void);
void simhp4_frontend_set_selector_active(int active);
int simhp4_frontend_selector_active(void);

int simhp4_frontend_submit_char(uint8_t ch);
int simhp4_frontend_submit_nav(simhp4_frontend_key_t key);
int simhp4_frontend_wait_key(simhp4_frontend_key_t *key, uint32_t timeout_ms);
void simhp4_frontend_flush(void);

#ifdef __cplusplus
}
#endif
