# ESP32 Pan-Tilt Camera

This Arduino project controls an ESP32-CAM with pan-tilt servos.

## Features
- Live video streaming
- Pan & tilt servo control
- Voice command integration (planned)

## Wiring
- Servo X → GPIO 12
- Servo Y → GPIO 13
- Camera pins: default ESP32-CAM setup

## How to Use
1. Install the ESP32 board in Arduino IDE.
2. Upload `esp32_pan_tilt.ino` to your ESP32-CAM.
3. Connect to the WiFi shown in Serial Monitor.
