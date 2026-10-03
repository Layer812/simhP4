#include "simhp4_media.h"

extern const unsigned char image_fs_start[]
    __asm__("_binary_image_fs_start");
extern const unsigned char image_fs_end[]
    __asm__("_binary_image_fs_end");
extern const unsigned char boot_rim_start[]
    __asm__("_binary_boot_rim_start");
extern const unsigned char boot_rim_end[]
    __asm__("_binary_boot_rim_end");

const unsigned char *simhp4_unix_image_data(void) { return image_fs_start; }
size_t simhp4_unix_image_size(void) { return (size_t)(image_fs_end - image_fs_start); }
const unsigned char *simhp4_unix_boot_data(void) { return boot_rim_start; }
size_t simhp4_unix_boot_size(void) { return (size_t)(boot_rim_end - boot_rim_start); }
