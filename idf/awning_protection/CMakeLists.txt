# Top-level ESP-IDF project file for awning-protection.
#
# A deliberately minimal ESP32-H2 Zigbee sleepy end device that solves four things and
# nothing else: (1) join/rejoin a deCONZ network, (2) deep sleep between wakes, (3) measure
# battery, (4) report ONE occupancy value ("awning protected"). No accelerometer metrics, no
# multi-endpoint fan-out — a single endpoint so the DDF matches cleanly in deCONZ.
#
# Build:  idf.py set-target esp32h2 && idf.py build && idf.py -p <PORT> flash monitor
# Managed components (esp-zigbee-lib / esp-zboss-lib) are pulled in via main/idf_component.yml
# on the first build.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(awning_protection)
