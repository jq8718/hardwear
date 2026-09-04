idf_component_register(
    SRCS "main.c" "ws2812_encoder.c" "st7789.c" "encoder.c"
    PRIV_REQUIRES esp_driver_rmt esp_driver_spi esp_driver_gpio esp_driver_pcnt esp_hal_pcnt
    INCLUDE_DIRS "."
)
