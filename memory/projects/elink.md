# elink

PlatformIO test project for an E-Ink display.

## Hardware

- Controller board: DFRobot FireBeetle 2 ESP32-E (DFR0654)
- Display: 7.5-inch FPC-8612 E-Ink, 800x480
- Display colors: black and white only (not three-color)
- Adapter: Good Display DESPI-C02
- Wiring: BUSY=GPIO14, CS=GPIO13, RST=GPIO21, DC=GPIO22, SCK=GPIO18, MOSI=GPIO23

## Companion backend

- Control Desk / PNG renderer: `d:\Project\Main\1OLD\weather`
- Public site: weather-e-ink.vercel.app
- Firmware requests `DEVICE_BASE_URL/screen.png`

## Important

Future display code and driver selection must target a monochrome panel. Do not use a red/color plane unless the user explicitly changes the hardware.

