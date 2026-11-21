# con-venience Firmware

[![CI](https://github.com/HuanYitiao/con-venience-ESP-IDF-firmware/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/HuanYitiao/con-venience-ESP-IDF-firmware/actions/workflows/ci.yml)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1-E7352C)](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/)
[![Target](https://img.shields.io/badge/target-ESP32--C6-blue)](https://www.espressif.com/en/products/socs/esp32-c6)
[![Last commit](https://img.shields.io/github/last-commit/HuanYitiao/con-venience-ESP-IDF-firmware)](https://github.com/HuanYitiao/con-venience-ESP-IDF-firmware/commits/main)

con-venience is a wearable social device for fursuiters: two wearers touch their devices together to exchange contact cards.

This is its ESP-IDF firmware. It replaces the Arduino firmware, [con-venience-firmware](https://github.com/HuanYitiao/con-venience-firmware), does everything the old firmware does, pairs with devices that still run it, and reads and writes the same SD cards. How the port went, and the pitfalls found on the way, are in the [migration log](docs/migration.md).

## Plan

| Status | Step | Content |
|---|---|---|
| ⬜ | R1 | First release: version number, release build settings, CI that publishes the binaries on a tag, flashing guide |
| ⬜ | R2 | Self-test from the old `test_main.cpp`, behind `CONFIG_CONVENIENCE_SELFTEST` |
| ⬜ | R3 | Hardware checks not done yet (see below) |
| ⬜ | R4 | Next PCB: bare ESP32-C6 module, SD pull-ups |

Status: ✅ done, 🚧 in progress, ⬜ not started.

**R1, still to decide:** the first version number, whether the partition table gets two OTA slots now (changing it later means erasing the whole flash), whether a release ships an SD card template with the setup pages, and whether there is a changelog.

**R3, not yet tested on the PCB:**

- The `power_sleep` fallback paths: a button still held at sleep, and sleeping inside the U1 hold window.
- Power-off at about 6 s with the keep-alive disabled (the M1 control run).
- The battery level stepping down over a long run on cells.
- The hourly RTC wake that checks the battery and goes straight back to sleep.
- The shutdown current.
- The setup portal on iOS.

**R4:** the card side of the SD level shifter has no pull-ups, so some cards fail to write; see "SD pull-ups" in the [migration log](docs/migration.md#buses-and-peripherals). Moving to a module also means checking the flash size in [sdkconfig.defaults](sdkconfig.defaults) again.

## Hardware

- MCU: ESP32-C6 DevKit, soldered onto the board
- Display: JLX256128G-931, ST75256, 256×128, 4 gray levels
- Storage: MicroSD
- I/O expander: MCP23017
- Audio: MAX98357A
- Real-time clock: PCF85063A
- Power: 3× AA alkaline cells, or USB

Pins are in [board_pins.h](middleware/board/include/board_pins.h), datasheets in [docs/drivers](docs/drivers).

## Build and flash

Requires ESP-IDF v6.1. The target is `esp32c6`.

```bash
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash
idf.py -p /dev/ttyACM1 monitor
```

The board shows up as two serial ports: flash through the first, read the log on the second. While the board is off the USB port is gone, so press a button (or reinsert the cells) before flashing.

Configuration changes go in [sdkconfig.defaults](sdkconfig.defaults).

### Hardware tests

[tests/hw_tests](tests/hw_tests) holds one test per driver and for the contacts store. Set `HW_TEST` in [hw_tests.h](tests/hw_tests/include/hw_tests.h) to the test you want and flash; the board then runs only that test. Set it back to `HW_TEST_NONE` before committing.

## SD card

The card is FAT32. The firmware does not format it, and cards from the old firmware work as they are.

```
/self_profile/profile.json         own profile
/self_profile/avatar.bin           own avatar, 128×128, 2 bits per pixel
/friends_profiles/<uuid>/          one directory per friend, same files
/friends_deleted/                  deleted friends, to restore by hand on a PC
/web/profile-generator.html        setup page, served by the Wi-Fi portal
/web/jszip.min.js
```

The `web` files are not in this repository yet; copy them from a card set up for the old firmware.

## Using the device

| Page | UP / DOWN | RIGHT | LEFT | PAIR |
|---|---|---|---|---|
| Home | | Menu | | Click: show a link's QR code. Hold: start pairing |
| Menus and lists | Move | Open | Back | |
| Friends list | Move | Open the card. Hold: delete | Back | |
| Own profile | | Links | Back | |
| Pairing wait | | | | Hold: cancel |
| A friend's card | | | Back | Hold: back |

- **First-time setup:** a card without an own profile (no name or no avatar) starts on the Wi-Fi setup page. Join the device's Wi-Fi with the QR code; the setup page opens on the phone. Make the profile there and upload it. The device can only leave this page once a profile is saved. If the phone does not join, remove the device's network from the phone: the password changes every time the portal starts.
- **Pairing:** hold PAIR on both devices, then touch them together. The other wearer's card shows after about 6 s and is saved to the friends list.
- **Settings:** Wi-Fi setup (change the profile, set the clock from the phone), the clock, auto sleep, and Reset Profile, which moves the profile and the friends to backup folders on the card and reboots.
- **Power:** any button wakes the device. It powers off after 60 s without use, or 5 min on the Wi-Fi page. With auto sleep off, or on USB, it stays on. On a low battery it shows a warning and powers off until a button is pressed.

## Layout

```
main/            app_main: boot order
middleware/      drivers; must not depend on application
application/     ui, contacts, fsm, pairing, portal, supervisor
tests/hw_tests/  hardware tests, one at a time (HW_TEST)
tools/           pcf2lvgl.py: bitmap fonts for LVGL
docs/            migration log, datasheets
```

The [migration log](docs/migration.md#directory-layout) has the full tree, and notes on each module under "Pitfalls".

## License

Covered by the license of the con-venience project.
