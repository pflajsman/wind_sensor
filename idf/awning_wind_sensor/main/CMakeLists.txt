# main component: the Zigbee app + the reused ADXL345 driver.
#
# adxl345.c is the SAME register-level I2C driver logic as the Arduino build (it uses only
# the IDF i2c + gpio APIs, no Arduino). Keep the two in sync, or better, once this port is
# the source of truth, delete the Arduino copy.
idf_component_register(
    SRCS
        "awning_wind_sensor.c"
        "adxl345.c"
        "battery.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2c   # driver/i2c_master.h (new I2C master API)
        esp_adc          # battery: adc_oneshot + adc_cali
        driver           # driver/gpio.h
        esp_timer        # one-shot deep-sleep timer
        nvs_flash        # nvs_flash_init / partition
        espressif__esp-zigbee-lib
)
