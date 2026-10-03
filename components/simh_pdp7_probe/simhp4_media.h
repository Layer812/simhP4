#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
const unsigned char *simhp4_unix_image_data(void);
size_t simhp4_unix_image_size(void);
const unsigned char *simhp4_unix_boot_data(void);
size_t simhp4_unix_boot_size(void);
#ifdef __cplusplus
}
#endif
