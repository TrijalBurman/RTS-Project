# ESP32 RTOS Shared Resource Dashboard

This project runs three FreeRTOS tasks on an ESP32, visualizes their shared-resource
contention with LEDs, and hosts a live local dashboard.

## Hardware

Connect LEDs through suitable current-limiting resistors:

- High-priority task: GPIO 25
- Medium-priority task: GPIO 26
- Low-priority task: GPIO 27
- Shared resource: GPIO 33

## Run with PlatformIO

1. Connect an ESP32 development board.
2. From this folder, run `py -m platformio run --target upload`.
3. Open the serial monitor at 115200 baud with `py -m platformio device monitor`.
4. Connect to Wi-Fi network `ESP32-RTOS-Dashboard` using password `esp32rtos`.
5. Open `http://192.168.4.1/`.

## Run with Arduino IDE

Open `RTS_project/RTS_project.ino`, select an ESP32 development board, and upload it.
