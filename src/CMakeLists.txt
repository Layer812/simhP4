idf_component_register(
    SRCS
        "main.cpp"
        "simhp4_sd.cpp"
        "simhp4_usb_keyboard.c"
        "simhp4_frontend_input.c"
        "simhp4_selector.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        simh_pdp7_probe
        M5Unified
        M5GFX
        esp_common
        freertos
        log
        fatfs
        esp_driver_sdmmc
        sdmmc
        esp_driver_gpio
        newlib
        usb
        usb_host_hid

    PRIV_REQUIRES simhp4_remote_tty esp_driver_i2c
)


