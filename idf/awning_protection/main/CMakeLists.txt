# main component: the minimal Zigbee app + the reused battery driver.
idf_component_register(
    SRCS
        "awning_protection.c"
        "battery.c"
        "adxl345.c"
        "ui.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2c   # driver/i2c_master.h (ADXL345 bus)
        esp_adc          # battery: adc_oneshot + adc_cali
        driver           # driver/gpio.h
        esp_timer        # commissioning + reset timers
        nvs_flash        # nvs_flash_init / partition
        esp_pm           # PM locks while the LED blinks / button is held
        esp_driver_rmt   # SK6812 status LED (via led_strip)
        espressif__led_strip
        espressif__esp-zigbee-lib
)
