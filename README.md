# con-venience Firmware

con-venience is a wearable social device for fursuiters: two wearers touch their devices together to exchange contact information.

This repository migrates [con-venience-firmware](https://github.com/HuanYitiao/con-venience-firmware) (Arduino + PlatformIO, with working hardware and core features) to ESP-IDF. The work has two parts: first the hardware drivers in `middleware`, then the `application` built on top of them.

**Current status:** M0 to M4 are done. Buttons and the RTC timer wake the board from a real power-off with the right wake reason; all 5 buttons give click and long press events; VBAT reads within 0.5 % of a multimeter, and the battery level survives a reset and is forgotten after a battery reinsert. Not yet tested: the `power_sleep` fallback paths (a button still held at sleep, sleeping inside the U1 hold window), the M1 control run (keep-alive disabled, expect power-off at about 6 s), and stepping the battery level down. Next is M5, the display.

## Hardware

- MCU: ESP32-C6 DevKit, soldered directly onto the board (a move to a bare module is planned)
- Display: JLX256128G-931, ST75256, 256×128, 4 gray levels
- Storage: MicroSD
- I/O expander: MCP23017
- Audio: MAX98357A
- Real-time clock: PCF85063A
- Power: 3× AA alkaline cells, or USB

Pin definitions are in [board_pins.h](middleware/board/include/board_pins.h).

## Build

Requires ESP-IDF v6.1. The target is `esp32c6`.

```bash
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash
idf.py -p /dev/ttyACM1 monitor
```

Configuration changes go in [sdkconfig.defaults](sdkconfig.defaults).

## Migration principles

- **Validate module by module:** each driver is verified on hardware before the next one starts.
- **Stay compatible:** the ACOM frame format, BLE GATT protocol, packing format and SD directory layout do not change. New firmware can then pair with old-firmware devices for comparison, and existing SD cards keep working.
- **One-way dependencies:** `application` may depend on `middleware`, never the reverse.
- **Keep the task model simple:** start with one app task and one event queue. Buttons, BLE and HTTP only post events to the queue. Do not split into many tasks up front.
- **Code style:** C with classes. Both C and C++ are fine.

## Directory layout

`middleware/board` exists. Everything else is planned.

```
con-venience-ESP-IDF-firmware/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── main/                   # app_main only: boot order
├── middleware/             # must not depend on application
│   ├── board/              # pin map, I2C/SPI bus handles
│   ├── mcp23017/
│   ├── pcf85063a/
│   ├── power/              # keep-alive, power rails, VBAT, shutdown
│   ├── button/
│   ├── st75256/            # display bus, init, backlight
│   ├── sdcard/
│   ├── audio/
│   ├── ble_link/
│   ├── acom/
│   └── wifi_ap/
└── application/
    ├── ui/                 # LVGL glue, theme, fonts, pages
    ├── contacts/           # JSON, contacts, packing
    ├── fsm/
    ├── pairing/
    └── portal/             # web routes, profile upload, time sync
```

A few layering problems in the old project get split up during the migration:

- `display.h` depends on `fsm.h` and `storage.h`, and `display.cpp` also depends on `wifi_config`.
- `Contact` is defined in `storage`. It moves to `contacts`.
- `display.cpp` splits into two layers: `st75256` (bus and init) and `ui` (LVGL glue and pages).

## Plan

Status: ✅ done, 🚧 in progress, ⬜ not started.

### Middleware

| Status | Step | Module | Main work | Acceptance |
|---|---|---|---|---|
| ✅ | M0 | Project skeleton | Directories, partition table, `sdkconfig.defaults`, pin map | CI passes, logs appear on serial |
| ✅ | M1 | `board` + `mcp23017` + `power` keep-alive | `i2c_master` driver, port the BANK switch and `powerEarlyInit` | Stays powered for more than 6 s on battery |
| ✅ | M2 | `pcf85063a` + shutdown/wake | Time read/write, countdown timer, `powerSleep` | Buttons and the RTC both wake the device, with the correct wake reason |
| ✅ | M3 | `button` | INT interrupt notifies a task, debounce and long press | Click and long press work on all 5 buttons |
| ✅ | M4 | VBAT + NVS | Calibrated `esp_adc` reads, battery level stored in NVS | Reading matches a multimeter within an acceptable error |
| ⬜ | M5 | `st75256` | SPI bus, init sequence, LEDC backlight, an interface that writes 2bpp data to a window, not tied to a graphics library | 4-level gray test pattern displays correctly |
| ⬜ | M6 | `sdcard` | SPI shared with the display, mount/unmount, card detect | Old cards read and write, alternating display and SD access causes no errors |
| ⬜ | M7 | `audio` | The I2S code mostly carries over, remove the Arduino calls | Startup sound plays with no pop |
| ⬜ | M8 | `ble_link` | Rewrite the transport on NimBLE, GATT protocol unchanged | Two-way transfer with old firmware succeeds |
| ⬜ | M9 | `acom` | Single-wire UART, open-drain output | MAC exchange with an old-firmware device succeeds |
| ⬜ | M10 | `wifi_ap` | SoftAP, DNS, HTTP infrastructure | Captive portal opens when a phone connects |

BLE comes before ACOM because ACOM sends the BLE address and address type, which are only available once the NimBLE host has started.

M0 builds locally. CI and serial logs are not verified yet.

The self-test from the old `test_main.cpp` moves over after all middleware modules are done, behind a `CONFIG_CONVENIENCE_SELFTEST` option. Its I2C check can use `board_i2c_scan()`.

### Application

| Status | Step | Content | Acceptance |
|---|---|---|---|
| ⬜ | A1 | `ui`: bring up LVGL, validate the home page and friends list on the real screen, then build all pages with fake data | Text and animations are legible on the real screen, features and layout match the old firmware |
| ⬜ | A2 | `contacts`: profile read/write, friends list, delete and archive, packing | All data on an old card reads back |
| ⬜ | A3 | `fsm` + main loop: `fsm` handles page flow only, LVGL handles in-page focus, driven by the event queue | Full menu flow works |
| ⬜ | A4 | `pairing`: connect ACOM, BLE and contact saving | New–new and new–old pairing both succeed |
| ⬜ | A5 | `portal`: routes, profile upload, time sync, first-time setup | A blank card completes setup |
| ⬜ | A6 | Full-system integration: low battery, no SD card, stay-on over USB, auto sleep | Regression checklist passes, shutdown current measured |

## Dependency replacements

| Arduino | ESP-IDF | Notes |
|---|---|---|
| Wire | `i2c_master` | Reads must return an error, not 0xFF as the old code does |
| SPI / SD | `spi_master` + `esp_vfs_fat_sdspi_mount` | Paths need the mount point prefix |
| Preferences | `nvs_flash` | Only the battery level uses it, same namespace and key as before (`power` / `batLevel`). `main` calls `nvs_flash_init` |
| NimBLE-Arduino | IDF's built-in NimBLE (C API) | Consider `esp-nimble-cpp` only if this does not work out |
| WebServer / DNSServer | `esp_http_server` + the DNS component from the captive_portal example | No ready-made multipart parser |
| ArduinoJson | `espressif/cjson` or `bblanchon/arduinojson` | v6.1 no longer bundles a json component |
| U8g2 + hand-written drawing | `lvgl/lvgl` | Driven directly from the app task, without `esp_lvgl_port`. Not yet compiled against v6.1 |
| QRCode | Vendor ricmoo's C source | Keeps the generated codes unchanged |
| Adafruit NeoPixel | `led_strip` (already in use) | Only the old self-test used it |

**`timebase` is not migrated.** The old main program does not use it, and it does not suit this board:

- An external 32k crystal needs GPIO0/1, and GPIO1 is the VBAT ADC input.
- During shutdown the ESP32 is held in reset through EN, so the RTC domain cannot keep time.

The PCF85063A remains the source of wall-clock time.

## Pitfalls

### Power and timing

- **Boot order:** the first thing `app_main` does must be `board_i2c_init`, `mcp23017_init`, `pcf85063a_init` and `power_early_init`. `pcf85063a_init` may run before the keep-alive because it never clears a wake flag. `button_init` must come after `power_early_init`: reading GPIOB clears the INTF bits the wake reason is taken from. Do not use `ESP_ERROR_CHECK` on this path, or a single I2C error could cause a reboot loop.
- **OLATA races:** `power` and `display` both read-modify-write OLATA. Shared registers must only be changed through `mcp23017_update_reg`, which holds a lock across the read and the write.
- **Back-feeding through pins:** `gpio_reset_pin()` enables a pull-up, so using it to release the I2S and SD pins before power-off would back-feed the unpowered rail. Use `gpio_config` to set them as floating inputs. `sdspi_host.c` also enables pull-ups, so M6 must check which pins are affected and measure shutdown current.
- **Shutdown fallback path:** the end of `power_sleep`, which calls `esp_restart` with SLEEP_REQ still set if the MCU is still running, must stay as it is. Wake reason detection depends on it.
- **`power_sleep` needs `button_init`:** the button pull-ups (IODIRB, GPPUB) are set only by `button_init`, and a fresh MCP after a battery insert has none. Without them the buttons float, so `power_sleep` cannot wait for a release and a button wake is unreliable. The button task keeps running during `power_sleep`, so events can still arrive while it shuts down.
- **Testing sleep:** the USB-JTAG port disappears while the board is off, so a monitor does not follow it through a sleep and reflashing only works while it is awake (reinsert the battery or press a button first). To see what happened during a wake, log to NVS and read it on a later boot. The sleep test is in `main.c` behind `M2_SLEEP_TEST`.

### Buses and peripherals

- **Shared SPI:** the old code initializes SD before the display, while the LCD CS is not yet driven. The IDF version must drive CS high or add the display device before mounting SD.
- **Display transfers:** the old code runs one transaction per byte. Send each window as a batch, with DC controlled separately.
- **ACOM single wire:** `uart_set_pin` supports the same pin for TX and RX. Call `gpio_od_enable` after configuring it. The header warns that open-drain should be applied ahead of time, or the two ends can drive against each other. Keep the echo filtering.
- **Button INT:** the line is wire-ORed with the RTC. If an interrupt is not cleared, no further falling edge arrives, so `button` reads GPIOB every second while idle as a fallback. While a button is held or bouncing it polls every 10 ms instead of waiting for edges, so a release lost in a bounce is still seen. Events fire on release: LONG_PRESS after 300 ms, otherwise CLICK. A button already held at `button_init` (the wake press) gives no event.
- **Watchdog:** the C6 is single-core. The main loop must block or call `vTaskDelay`, or the task watchdog fires.

### Display (LVGL)

- **Render format:** LVGL has no 2bpp render target. Render as L8 and convert to the ST75256's vertically packed 2bpp format in `flush_cb`.
- **Refresh areas:** the display has one page per 4 rows, so dirty areas must be aligned to 4 rows with `LV_EVENT_INVALIDATE_AREA`.
- **Threading:** LVGL is not thread-safe. All calls stay in the app task.
- **Responsibilities:** `fsm` owns page flow and business state. Focus movement in lists and menus uses LVGL's keypad and groups.
- **Text:** pure black and white, no gray. Generate 1bpp bitmap fonts with `lv_font_conv --bpp 1` rather than using LVGL's built-in anti-aliased fonts. Gray is for avatars, backgrounds and decoration.
- **Animation:** STN panels respond slowly, so sliding animations may ghost. Tune each animation's duration and path on the real screen. Avoid opacity fades, since there are only 4 gray levels.
- **Custom content:** convert avatars to L8 on load. Pre-generate the dithered battery bands as images or draw them on a canvas. Draw QR codes on a canvas.
- **Memory:** measure with Wi-Fi and BLE both active.

### IDF configuration

| Setting | IDF default | This project | Status |
|---|---|---|---|
| Flash size | 2MB | 16MB, measured with `esptool flash-id` | Set |
| Partition table | Default single app | Custom, 3MB app partition | Set |
| Main task stack | 3584 bytes | 8192 bytes, may become a dedicated task later | Set |
| `max_uri_handlers` | 8 | At least 12: the old firmware has 12 routes plus a not-found handler | M10 |
| NimBLE preferred MTU | 256 | 517 | M8 |

## Open questions

- **Flash size on a bare module:** check again and update `sdkconfig.defaults` when moving off the DevKit.
- **Profile upload format:** decide at A5. If `profile-generator.html` can change, one raw POST per file is simplest. To stay compatible with the old page on existing cards, write a small multipart parser.

## Decisions

- BLE uses IDF's built-in NimBLE first, and changes only if that does not work out.
- Code style is C with classes.
- The UI uses LVGL, with ready-made components for menus and animations wherever possible.
- The soldered DevKit stays for now, with a move to a module later.

## License

To be decided. The original project uses the MIT License.
