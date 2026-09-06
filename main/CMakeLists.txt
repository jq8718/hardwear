idf_component_register(
    SRCS "lcd_vt.c" "power_mgmt.c" "ota_mqtt.c" "nvs_config.c" "input_map.c" "wifi_mqtt.c" "main.c" "ws2812_encoder.c" "st7789.c" "encoder.c" "i2c_io.c" "state_led.c" "audio.c" "tjpgd.c" "screen_jpeg.c"
    PRIV_REQUIRES esp_driver_rmt esp_driver_spi esp_driver_gpio esp_driver_pcnt esp_hal_pcnt esp_driver_i2c esp_driver_i2s esp_timer esp_wifi esp_netif esp_event nvs_flash mqtt cjson app_update esp_http_client
    INCLUDE_DIRS "."
)
