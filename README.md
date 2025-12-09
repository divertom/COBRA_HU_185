# COBRA_HU_185 Project

This is a copy of the ESP32-S3-LCD-1.85-Test project with the following functionality removed:
- Audio (PCM5101 driver and audio playback)
- Microphone (MIC_Driver and speech recognition)
- Touch input and gesture recognition

## Remaining Functionality

- LCD Display (ST77916)
- LVGL Graphics Library
- SD Card support
- I2C drivers
- PCF85063 RTC
- QMI8658 IMU
- Battery monitoring
- Power key handling
- Wireless connectivity
- EXIO (TCA9554PWR)

## Build Instructions

1. Open the project in ESP-IDF
2. Run `idf.py build` to build the project
3. Run `idf.py flash` to flash to device

## Notes

- All audio, microphone, touch input, and gesture dependencies have been removed from idf_component.yml

