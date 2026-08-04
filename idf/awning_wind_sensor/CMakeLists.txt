# Top-level ESP-IDF project file for the awning wind sensor (native esp-zigbee-sdk port).
#
# This replaces the arduino-esp32 build for the deep-sleep Zigbee end device, whose
# post-sleep rejoin proved unreliable in the Arduino Zigbee wrapper. On native ESP-IDF we
# control the sleep + commissioning signals directly (esp_zb_sleep_enable, the
# DEVICE_REBOOT rejoin path), which is the vendor-supported way to do deep sleep + rejoin.
#
# Build:  idf.py set-target esp32h2 && idf.py build && idf.py -p <PORT> flash monitor
# The esp-zigbee-lib / esp-zboss-lib managed components are pulled in via
# main/idf_component.yml on the first build.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(awning_wind_sensor)
