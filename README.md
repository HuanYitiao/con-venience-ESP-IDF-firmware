# con-venience Firmware

[![CI](https://github.com/HuanYitiao/con-venience-ESP-IDF-firmware/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/HuanYitiao/con-venience-ESP-IDF-firmware/actions/workflows/ci.yml)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1-E7352C)](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c6/)
[![Target](https://img.shields.io/badge/target-ESP32--C6-blue)](https://www.espressif.com/en/products/socs/esp32-c6)
[![Last commit](https://img.shields.io/github/last-commit/HuanYitiao/con-venience-ESP-IDF-firmware)](https://github.com/HuanYitiao/con-venience-ESP-IDF-firmware/commits/main)

con-venience is a wearable social device for fursuiters: two wearers touch their devices together to exchange contact information.

This repository migrates [con-venience-firmware](https://github.com/HuanYitiao/con-venience-firmware) (Arduino + PlatformIO, with working hardware and core features) to ESP-IDF. The work has two parts: first the hardware drivers in `middleware`, then the `application` built on top of them.

**Current status:** the middleware (M0 to M10) is done. Buttons and the RTC timer wake the board from a real power-off with the right wake reason; all 5 buttons give click and long press events; VBAT reads within 0.5 % of a multimeter, and the battery level survives a reset and is forgotten after a battery reinsert. Not yet tested: the `power_sleep` fallback paths (a button still held at sleep, sleeping inside the U1 hold window), the M1 control run (keep-alive disabled, expect power-off at about 6 s), and stepping the battery level down. The display shows the 4-level gray test pattern with the right orientation and pixel order. A 16 GB card with old-firmware data mounts, reads, writes and survives 50 rounds of alternating display and SD access; an old 4 GB card reads but fails to write (see the SD pull-up note). The startup beep and melodies play with no click at start or end. ACOM exchanges addresses with an old-firmware device, acked both ways, repeatably. BLE transfers a test profile both ways with an old-firmware device; NimBLE's address matches the BT MAC sent over ACOM. Only one BLE role is tested so far (the role follows the two MACs), the other needs a peer on the other side of the comparison. The Wi-Fi portal opens by itself on an Android phone, and a 2 MB upload arrives complete from the phone's browser; BLE and the AP together leave about 200 KB of heap free. Next is the application, starting with A1.

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

`main/`, `tests/`, `tools/` and `middleware/` exist; in `application/`, `ui` and `contacts` have started.

```
con-venience-ESP-IDF-firmware/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── main/                   # app_main: boot order
├── tests/hw_tests/         # milestone hardware tests, one at a time (HW_TEST)
├── tools/                  # pcf2lvgl.py: bitmap fonts for LVGL
├── middleware/             # must not depend on application
│   ├── board/              # pin map, I2C/SPI bus handles
│   ├── mcp23017/
│   ├── pcf85063a/
│   ├── power/              # keep-alive, power rails, VBAT, shutdown
│   ├── button/
│   ├── st75256/            # display bus, init, backlight
│   ├── sdcard/
│   ├── max98357a/          # I2S amp, tones
│   ├── acom/               # contact link, BLE address exchange
│   ├── ble_link/           # profile transfer over NimBLE
│   ├── dns_server/         # copied from the IDF captive portal example
│   └── wifi_ap/            # soft AP, captive portal, HTTP server
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
| ✅ | M5 | `st75256` | SPI bus, init sequence, LEDC backlight, an interface that writes 2bpp data to a window, not tied to a graphics library | 4-level gray test pattern displays correctly |
| ✅ | M6 | `sdcard` | SPI shared with the display, mount/unmount, card detect | Old cards read and write, alternating display and SD access causes no errors |
| ✅ | M7 | `max98357a` | The I2S code mostly carries over, remove the Arduino calls | Startup sound plays with no pop |
| ✅ | M8 | `acom` | Single-wire UART, open-drain output | MAC exchange with an old-firmware device succeeds |
| ✅ | M9 | `ble_link` | Rewrite the transport on NimBLE, GATT protocol unchanged | Two-way transfer with old firmware succeeds |
| ✅ | M10 | `wifi_ap` | SoftAP, DNS, HTTP infrastructure | Captive portal opens when a phone connects |

ACOM comes before BLE. ACOM sends the BLE address and address type, but `acom` takes them as parameters instead of asking NimBLE (the old code called `NimBLEDevice::getAddress()` directly), so it does not depend on `ble_link`. For the M8 test the address comes from `esp_read_mac(..., ESP_MAC_BT)` with type 0 (public). M9 must check that this matches the address NimBLE advertises, including byte order: the old code reverses `addr->val`. `pairing` connects the two.

CI builds every push to `main` and every pull request with ESP-IDF v6.1 for the ESP32-C6. It only checks that the firmware builds; hardware tests are run by hand on the PCB.

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
- **Back-feeding through pins:** `gpio_reset_pin()` enables a pull-up, so using it to release the I2S and SD pins before power-off would back-feed the unpowered rail. Use `gpio_config` to set them as floating inputs. `sdspi_host.c` only enables pull-ups on CD, WP and INT, which are not used here; unmount leaves SD CS as a floating input. Shutdown current is not measured yet.
- **Shutdown fallback path:** the end of `power_sleep`, which calls `esp_restart` with SLEEP_REQ still set if the MCU is still running, must stay as it is. Wake reason detection depends on it.
- **`power_sleep` needs `button_init`:** the button pull-ups (IODIRB, GPPUB) are set only by `button_init`, and a fresh MCP after a battery insert has none. Without them the buttons float, so `power_sleep` cannot wait for a release and a button wake is unreliable. The button task keeps running during `power_sleep`, so events can still arrive while it shuts down.
- **Testing sleep:** the USB-JTAG port disappears while the board is off, so a monitor does not follow it through a sleep and reflashing only works while it is awake (reinsert the battery or press a button first). To see what happened during a wake, log to NVS and read it on a later boot. The sleep test is `HW_TEST_SLEEP` in `tests/hw_tests/include/hw_tests.h`.

### Buses and peripherals

- **SD pull-ups (hardware):** the card side of U17 has no pull-ups. SPI_MISO_ISOL (DAT0) floats whenever the card releases DO, and DAT1/DAT2 are left open, against the SD spec. A good card works; an old 4 GB card accepts a write, then never seems to leave busy, and reads time out afterwards. Next PCB revision: 10k to 47k pull-ups to +3.3V_SD on DAT0, DAT1, DAT2 and CMD. Software side: the MISO pull-up on GPIO10 keeps the ESP side from floating while U17 is off, and `wait_for_miso = -1` skips the driver's busy poll with CS high, which reads nothing on this board because CS also enables U17.
- **Writes and power loss:** FAT has no journal. A reset or power loss while a file or the FAT is being written can leave a broken file or a damaged file system (seen in the M6 test: `idf.py flash monitor` resets the chip twice, and the first run gets cut off mid-test). `contacts` must write to a temporary file and rename it over the old one once it is complete.
- **Speaker:** the small speaker barely reproduces a few hundred Hz. A square wave is heard mostly through its harmonics, a sine at 262 Hz is almost silent; the same sine two octaves up is clearly audible. UI sounds use square waves, ideally with fundamentals of 1 to 4 kHz. `max98357a` also holds the tone generator (waveforms, notes); move that up to `application` once it exists and keep the driver to I2S, amp sequencing and PCM writes.
- **Shared SPI:** the old code initializes SD before the display, while the LCD CS is not yet driven. `st75256_init` adds the display device, which drives its CS high, so it must run before the SD card is mounted. `power_periph_on` must come before `board_spi_init`: driving the bus into an unpowered SD card back-feeds it.
- **Display transfers:** `st75256` uses the `esp_lcd` SPI panel IO (no panel driver; IDF has none for the ST75256). Each window goes out as one DMA transfer, and `st75256_write` blocks until it is done. LVGL's `flush_cb` will want an async variant.
- **Display tuning:** the panel is a passive-matrix STN, so dense content causes crosstalk along rows and columns; the old firmware shows the same. Vop is tuned to 318 (old: 313), and the gray levels to light 0x08, dark 0x0B. The old firmware's gray level command sent a 01..1F ramp; per the datasheet (9.2.31) only parameters 4-6 (light) and 9, 12-14 (dark) count and must be equal. The old firmware's display control byte 0x20, commented "N-line off" as in u8g2, is in fact LF = 16: inversion every 17 lines (datasheet 9.2.5); the driver now takes the line count (`n_line`). The rest are the old values. To retune, select `HW_TEST_DISPLAY_TUNE` in `tests/hw_tests/include/hw_tests.h` and adjust with the buttons; the log shows the frame rate in Hz.
- **ACOM single wire:** `uart_set_pin` supports the same pin for TX and RX. Call `gpio_od_enable` after configuring it. The header warns that open-drain should be applied ahead of time, or the two ends can drive against each other. Keep the echo filtering.
- **Button INT:** the line is wire-ORed with the RTC. If an interrupt is not cleared, no further falling edge arrives, so `button` reads GPIOB every second while idle as a fallback. While a button is held or bouncing it polls every 10 ms instead of waiting for edges, so a release lost in a bounce is still seen. Events fire on release: LONG_PRESS after 300 ms, otherwise CLICK. A button already held at `button_init` (the wake press) gives no event.
- **Watchdog:** the C6 is single-core. The main loop must block or call `vTaskDelay`, or the task watchdog fires.

### Wi-Fi portal

- **Android routing:** a Wi-Fi without internet does not become the default network while mobile data is on. A browser then sends `http://192.168.4.1/` over mobile data and the page never loads. The captive-portal sign-in window is bound to the Wi-Fi and does load. Choosing "stay connected" when Android asks, or turning mobile data off, makes the browser work too.
- **Sign-in window:** it is a system WebView, not a browser. On Android, `<input type="file">` does nothing there (tested), so photos cannot be picked. Its user agent contains `; wv)`. `portal` should detect that and ask for the page in the browser at the step that needs a photo; everything else can stay in the window. iOS is not tested yet.
- **Uploads:** shrink the avatar to 128x128 2bpp (4096 bytes) in the page before sending, as the old page did. A raw 2 MB upload took about 9 s.
- **Captive detection:** DNS answers every name with 192.168.4.1, every unknown path is a 302 to `http://192.168.4.1/` with a body (iOS needs one), and DHCP option 114 carries the same URL (`WIFI_AP_DHCP_CAPTIVE_URI`). Faking a 204 answer to the probes does not help: Android also probes over HTTPS and then reports limited connectivity.
- **Password:** new on every start and kept out of NVS. It is generated while Wi-Fi runs briefly as an idle station, because `esp_fill_random` is only truly random with the RF on.
- **Memory (measured):** 314 KB free at boot, 286 KB with BLE, 215 KB with the AP up, 197 KB lowest during an upload.

### Display (LVGL)

- **Render format:** LVGL has no 2bpp render target. Render as L8 and convert to the ST75256's vertically packed 2bpp format in `flush_cb`.
- **Refresh areas:** the display has one page per 4 rows, so dirty areas must be aligned to 4 rows with `LV_EVENT_INVALIDATE_AREA`.
- **Threading:** LVGL is not thread-safe. All calls stay in the app task.
- **Responsibilities:** `fsm` owns page flow and business state. Focus movement in lists and menus uses LVGL's keypad and groups.
- **Crosstalk and gray:** the hardware grays sit on the steep part of the STN's response, so they take crosstalk worst: a large black area changes the gray in the same rows (seen as a band across gray swatches next to a black bar). UI text and widgets are black and white only. Gray is for avatars and decoration, kept out of rows with large black areas (including a focused item's black background). If avatars still suffer, compare them against monochrome mode (0xF0 0x10) with dithering, which the old avatar format already allows.
- **Text:** pure black and white, no gray. The fonts are X11 misc-fixed bitmap fonts (public domain, the family the old firmware used through u8g2): 7x14 bold for names and menus, 6x12 for the rest. `tools/pcf2lvgl.py` turns the system's `.pcf.gz` files into 1bpp LVGL fonts in `application/ui/fonts/` (no Node.js needed; `lv_font_conv` only reads outline fonts). They cover Latin with Latin-1 and Extended-A, Greek and Cyrillic; no CJK for now. Gray is for avatars, backgrounds and decoration.
- **Contact card:** laid out as the old design: avatar left, black name bar and light gray details right. The name bar shares rows with the avatar; with a real avatar the crosstalk is visible but mild, partly because avatars tend to be plain at the top. Long text ends in dots for now; scrolling must step, not glide.
- **Animation:** the STN takes a few hundred ms to settle (measured: a jump leaves a ghost for just under 0.5 s). A smooth animation, redrawn every 33 ms, smears badly. Change the screen in jumps, at most a few steps, and avoid opacity fades, since there are only 4 gray levels.
- **Focus:** the mono theme only draws a focus outline, which containers clip. Focusable widgets get an explicit style for `LV_STATE_FOCUSED`: black background, white text.
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

Covered by the license of the con-venience project.
