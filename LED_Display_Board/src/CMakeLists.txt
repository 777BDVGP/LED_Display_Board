# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        esp_adc
        esp_driver_rmt
        esp_timer
        esp_event
        esp_netif
        esp_wifi
        nvs_flash
        spiffs
        json
        esp_http_server
        esp_http_client
        app_update
        esp_https_ota
        esp_partition
        esp_app_format
        mbedtls
        lwip
)
