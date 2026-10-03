idf_component_register(
    SRCS
        "simhp4_remote_tty.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
        esp_hosted
        esp_wifi_remote
        esp_wifi
        nvs_flash
        esp_netif
        esp_event
        lwip
)

