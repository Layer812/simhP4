#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int simhp4_usb_keyboard_start(void);
int simhp4_usb_keyboard_connected(void);
unsigned simhp4_usb_keyboard_key_count(void);

#ifdef __cplusplus
}
#endif
