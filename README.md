| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C6 | ESP32-H2 | ESP32-S2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- |

# E-bike dashboard for Waveshare ESP32-S3-Touch-LCD-2

Project version: `0.10.0` (first OTA-enabled release).

Ready-to-flash ESP-IDF project using LVGL 8.4. It supports ESP-IDF 5.5.2 and explicitly
declares the LEDC driver component required by ESP-IDF 6.0.2. The LCD is configured in 320 x 240
landscape orientation and starts directly on the e-bike dashboard.

## Build and flash

1. Open this project folder in VS Code.
2. Select the installed ESP-IDF 5.5.2 configuration.
3. Connect the board and select its COM port.
4. Use **ESP-IDF: Build, Flash and Monitor**.

The required managed components are pinned in `dependencies.lock`. The first configuration
step may still check the Espressif Component Registry, but it should reuse the included
`managed_components` directory.

The project uses two 4 MB OTA application slots on the board's 16 MB flash, plus the required
`otadata` partition. Migrating from v09 changes the partition table and therefore requires one
full USB flash. Subsequent application releases can be installed safely over Wi-Fi with rollback.

There is no simulated ride data. The dashboard starts at zero with a red `NO VESC` status and
polls the controller every 200 ms. A valid CRC-checked response changes the status to `VESC`.
If the controller disconnects, the last measurements stay visible, current changes to `--.- A`,
and the status returns to `NO VESC`.

## VESC CAN connection

The ESP32-S3 has a CAN/TWAI controller but no physical CAN transceiver. Use a 3.3 V transceiver
such as an SN65HVD230 between the Waveshare board and the VESC. Never connect ESP32 GPIO pins
directly to CANH or CANL.

| Waveshare board | CAN transceiver | VESC/bus |
| --- | --- | --- |
| GPIO17 | TXD | - |
| GPIO18 | RXD | - |
| 3V3 | VCC | - |
| GND | GND | CAN ground |
| - | CANH | CANH |
| - | CANL | CANL |

For a bare SN65HVD230, connect its `RS` pin to GND for normal high-speed operation. Many
ready-made breakout boards already do this. Do not feed the VESC connector's 5 V pin into the
ESP32 board's 3.3 V rail; only CANH, CANL and a common ground are required between the two
powered devices.

Use 120 ohm termination only at the two physical ends of the bus. With power removed, about
60 ohms measured between CANH and CANL indicates that both end terminators are present.

In VESC Tool set:

- CAN mode: `VESC`;
- CAN baud rate: `500 kbit/s`;
- controller ID: `0` (or change `EBIKE_VESC_CONTROLLER_ID`);
- correct motor poles, gearing, wheel diameter, battery type/cells and capacity so the VESC's
  speed, distance, battery percentage and remaining Wh are meaningful.

All project-side settings are in `main/ebike_can_config.h`. The dashboard defaults to CAN ID
120 and must not use the same ID as the controller. GPIO17/18 can also be changed there.

The implementation includes:

- `COMM_GET_VALUES_SETUP_SELECTIVE` polling;
- VESC fragmented response reassembly;
- CRC-16 validation before live data is accepted;
- parsing of broadcast STATUS 1 through 5 frames as a fallback;
- 1.2 second link timeout;
- TWAI bus-off detection and automatic recovery;
- thread-safe transfer from the CAN task to LVGL.

Every five seconds the Serial Monitor prints CAN diagnostics: link state, transmitted polls,
transmit errors, valid received packets, CRC errors, malformed packets and bus-off events. On a
healthy powered connection, `tx` and `rx` should both increase and the log should report
`CAN link is up`. If `tx` rises but `rx` remains zero, first check CANH/CANL, common ground,
500 kbit/s and the VESC controller ID.

This firmware is telemetry-only. ECO/NORM/SPORT remain touch-selectable application profiles,
but they do not yet write configuration or control commands to the VESC. This avoids changing
motor limits until the exact speed, battery-current and motor-current limits for each mode are
defined.

## Touch controls and acceleration

The four dashboard actions use the measured raw CST816 coordinates before LVGL rotation or object
hit-testing. The selected mode is filled with colour:

- **ECO** displays PAS/profile 2.
- **NORM** displays PAS/profile 3.
- **SPORT** displays PAS/profile 5.

The callback registered with `ebike_ui_set_mode_change_callback()` logs the selection and is the
place for a future safe VESC profile command. Each initial touch also prints its corrected
320 x 240 coordinate in Serial Monitor.

Tap the left temperature gauge to switch between `MOTOR` and `FET`. Both values continue to
update from VESC in the background; only the displayed channel changes.

Tap the bottom-left distance block to cycle `TRIP 1 -> TRIP 2 -> ODO`. Hold `TRIP 1` or `TRIP 2`
to reset it. The two local counters accumulate VESC absolute distance and are saved to NVS every
30 seconds. `ODO` is the global odometer reported by VESC and cannot be reset from the dashboard.

The bottom-center value is live longitudinal acceleration from QMI8658 in m/s2. A startup and
slow-moving bias filter removes gravity and PCB mounting angle, while a faster filter keeps the
number readable. If the board is mounted with another edge facing forward, change
`EBIKE_LONGITUDINAL_ACCEL_AXIS` and `EBIKE_LONGITUDINAL_ACCEL_SIGN` near the top of
`main/main.c`.

## microSD VESC logging

The onboard microSD slot shares SPI2 MOSI/SCLK with the display and uses a separate chip select:

| Signal | GPIO |
| --- | --- |
| MOSI | 38 |
| SCLK | 39 |
| MISO | 40 |
| SD CS | 41 |
| LCD CS | 45 |

Insert a FAT32-formatted card before startup. A new `log000.csv` through `log999.csv` file is
created on every boot. Samples are written at 10 Hz and flushed every five seconds. The CSV stores
time, VESC link state, decoded throttle percentage, motor current, battery/input current, duty
cycle, input voltage, MOSFET temperature, motor temperature and ERPM. The dashboard reads throttle
with the read-only `COMM_GET_DECODED_ADC` request; it does not send a throttle or motor command.
If the card is absent or cannot mount, the rest of the dashboard continues normally.

The LCD and microSD share a binary bus semaphore. An LCD flush holds it until the DMA-completion
callback, while each buffered CSV operation holds it through any resulting FATFS write. This is
required because separate chip-select signals do not by themselves serialize two tasks using the
same ESP32 SPI peripheral.

## Wi-Fi log download and OTA

The display always creates its own WPA2 Wi-Fi access point after boot:

- network: `Ebike-Logs`;
- password: `ebike-logs`;
- page: `http://192.168.4.1/`.

Open that address on a connected phone or computer to see every `logNNN.csv` file and download it.
The file currently being recorded is marked in green. Before a page or download is opened, the
active CSV is flushed so completed samples are included. Downloads are read from SD in 4096-byte
blocks, releasing the shared SPI semaphore between blocks, so LVGL can continue refreshing the
screen instead of waiting for an entire file transfer. Downloading does not delete or stop the log.

The same page now accepts home Wi-Fi or phone-hotspot credentials. The ESP operates in AP+STA mode,
so `Ebike-Logs` remains available while the station connection supplies internet access for OTA.
Credentials are stored only in local NVS and are not compiled into the firmware.

The stable manifest is read from the latest public GitHub Release. The dashboard checks product,
hardware identifier, semantic version, exact byte count and SHA-256 before ESP-IDF validates and
boots the image from the inactive OTA slot. Installation is manual and is rejected while VESC
telemetry indicates movement or material battery current. `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
returns to the previous slot if the new firmware cannot complete startup. Detailed setup and the
one-time USB migration are in `docs/OTA_SETUP.md`.

Touch actions are decoded directly from raw CST816 coordinates before LVGL applies any hit testing.
Tap the large speed area (`x=100..220`, `y=40..140`; measured centre `160,85`) to cycle
`ECO -> NORM -> SPORT`. Tap the actual bottom-left TRIP block (`x=0..112`, `y=188..239`)
to cycle `TRIP 1 -> TRIP 2 -> ODO`.
Each successful touch prints `Raw touch action: ...` to the serial terminal.

## VESC Tool mobile bridge

The ESP32 advertises a BLE peripheral named `VESC-E`. It includes the Nordic UART Service UUID
in the 31-byte advertisement so the Android VESC Tool scan does not filter it out. It implements
the Nordic UART
Service UUIDs used by VESC Tool and bridges complete VESC packet payloads to controller CAN ID 0.
In VESC Tool, scan for BLE devices and connect to `VESC-E`; realtime data, terminal commands,
Motor Configuration and App Configuration are transported through the standard VESC fragmented
CAN protocol. The bridge uses CAN ID 253, separate from the dashboard telemetry CAN ID, so long
configuration replies cannot overwrite the dashboard's reassembly buffer.

Dashboard polling and the BLE bridge remain active together. The VESC controller target remains
CAN ID 0. Dashboard replies use sender ID 120 and VESC Tool replies use sender ID 253, with
independent reassembly buffers. A phone connection therefore needs no manual SCREEN/APP selector
and does not stop temperature or link updates on the display.

Firmware updates through this new bridge have not been hardware-validated and should initially be
performed through the controller's direct USB connection. Configuration writes should first be
tested with the wheel off the ground and the throttle untouched.

## Host protocol test

The pure C VESC packet parser has a host-side test that does not require ESP-IDF:

```sh
cc -std=c11 -Wall -Wextra -Werror -Imain \
  tests/test_vesc_can_protocol.c main/vesc_can_protocol.c -lm -o /tmp/vesc_can_test
/tmp/vesc_can_test
```

## Display orientation

The original Waveshare example was portrait. This project changes LVGL to 320 x 240 and rotates
the ST7789 panel and CST816 touch coordinates to landscape. If the chosen physical mounting
places the USB connector on the opposite side, swap the panel mirror arguments in
`display_init()` and adjust the matching touch mirror flags in `touch_init()`.
