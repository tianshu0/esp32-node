# esp32-node: ESP32-C3 传感器节点固件（NimBLE Peripheral + I2C 传感器）
# 工程顶层 CMakeLists（组件内显式列源文件，不使用 GLOB）
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_node)
