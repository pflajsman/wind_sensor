# main component: the minimal Zigbee app + the reused battery driver.
idf_component_register(
    SRCS
        "awning_protection.c"
        "battery.c"
        "adxl345.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2c   # driver/i2c_master.h (ADXL345 bus)
        esp_adc          # battery: adc_oneshot + adc_cali
        driver           # driver/gpio.h
        esp_timer        # one-shot deep-sleep timer
        nvs_flash        # nvs_flash_init / partition
        espressif__esp-zigbee-lib
)
