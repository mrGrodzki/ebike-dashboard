# OTA build and update setup

The firmware is pinned to ESP-IDF 5.5.2 and targets the Waveshare
ESP32-S3-Touch-LCD-2. GitHub Actions performs a clean build for every change to `main`.
Only tags matching `vMAJOR.MINOR.PATCH` publish a stable GitHub Release.

## One-time repository setup

1. Create a **public** GitHub repository named `mrGrodzki/ebike-dashboard`.
2. Push this project to its `main` branch.
3. Open the repository `Actions` tab and confirm the `Build firmware` workflow is green.
4. Create the initial stable tag only after the USB build has been hardware-tested:

   ```sh
   git tag v0.10.0
   git push origin v0.10.0
   ```

The release workflow compiles in `espressif/idf:v5.5.2`, runs the host CAN protocol test,
checks that the application fits a 4 MiB OTA slot, creates `manifest.json`, calculates the
exact binary SHA-256 and attaches these files to the release:

- `ebike-dashboard.bin`, for OTA;
- `manifest.json`, read by the display;
- `first-usb-flash.zip`, required once to migrate from the old factory partition table.

The repository must be public because the ESP does not carry a GitHub access token. The firmware
contains no Wi-Fi password. Wi-Fi credentials are entered locally at `192.168.4.1` and stored in
the ESP32 NVS partition.

## First USB migration

The old v09 layout has one 2 MiB `factory` application. The OTA layout has `otadata` plus two
4 MiB slots. A partition-table change cannot be installed as an application-only OTA update.
Download and extract `first-usb-flash.zip`, then flash it once over USB.

Windows ESP-IDF terminal:

```bat
FLASH-WINDOWS.bat COM5
```

Linux or macOS:

```sh
chmod +x flash-linux-macos.sh
./flash-linux-macos.sh /dev/ttyACM0
```

After boot, connect a phone to `Ebike-Logs`, password `ebike-logs`, and open
`http://192.168.4.1/`. Enter the home Wi-Fi or phone-hotspot credentials. The local access point
stays available while the ESP also connects to the internet as a station.

## Runtime safety

The dashboard checks the release manifest after an internet connection becomes available and then
every six hours. Installation remains manual. The install request is rejected when VESC telemetry
shows wheel motion, ERPM above the stopped threshold or more than 2 A of battery current.

The downloaded binary must match product, hardware, release version, byte count and SHA-256. ESP-IDF
then validates the application image before selecting the inactive slot for the next boot. Rollback
is enabled. A newly installed image is marked valid only after display initialization and essential
tasks have started; otherwise the bootloader returns to the previous working slot.
