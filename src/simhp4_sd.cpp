#include "simhp4_sd.h"

#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "SIMHP4_SD";
static const char *MOUNT = "/sdcard";
static sdmmc_card_t *s_card = nullptr;

const char *simhp4_sd_mountpoint(void)
{
    return MOUNT;
}

int simhp4_sd_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};

    /* Tab5 microSD is wired to ESP32-P4 dedicated SDMMC slot 0.
     * Keep ESP-Hosted/C6 on slot 1 so both devices can coexist. */
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = 20000;
    host.flags |= SDMMC_HOST_FLAG_4BIT;

    /* Match the official Tab5 BSP: SDMMC slot 0 IO power uses on-chip LDO4. */
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    static sd_pwr_ctrl_handle_t pwr_ctrl_handle = nullptr;
    if (pwr_ctrl_handle == nullptr) {
        esp_err_t pwr_err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
        if (pwr_err != ESP_OK) {
            ESP_LOGW(TAG, "SD LDO4 init failed: %s", esp_err_to_name(pwr_err));
            return 0;
        }
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;

    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0  = GPIO_NUM_39;
    slot.d1  = GPIO_NUM_40;
    slot.d2  = GPIO_NUM_41;
    slot.d3  = GPIO_NUM_42;
    slot.width = 4;
#ifdef SDMMC_SLOT_FLAG_INTERNAL_PULLUP
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
#endif

    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 16;
    mount_cfg.allocation_unit_size = 16 * 1024;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount unavailable: %s", esp_err_to_name(err));
        return 0;
    }

    ESP_LOGI(TAG, "SD mounted at %s slot=0 dedicated GPIO39-44 LDO4", MOUNT);
    return 1;
}
