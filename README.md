# tasmota-migrate

Migrate ESP32 devices from [Tasmota](https://github.com/arendst/Tasmota) (safeboot layout) to [ESPHome](https://github.com/esphome/esphome) over the air -- no serial cable required.

An ESPHome "installer" firmware is uploaded via Tasmota's built-in HTTP update endpoint. It automatically repartitions the flash for ESPHome's dual-OTA layout, then accepts the real ESPHome firmware via the standard `esphome upload` command.

## Prerequisites

- ESP32 device running Tasmota with the **safeboot** partition layout
- Device accessible on the network
- ESPHome CLI installed (`pip install esphome`)

## Quick Start

### 1. Create the installer config

Copy `installer.example.yaml` and fill in your WiFi credentials:

```yaml
# installer.yaml
esphome:
  name: tasmota-migrator

esp32:
  board: esp32dev  # change to match your device
  framework:
    type: esp-idf

wifi:
  ssid: "YourNetwork"
  password: "YourPassword"

  ap:
    ssid: "Tasmota-Migrate"

logger:

ota:
  - platform: esphome

external_components:
  - source:
      type: local
      path: components

tasmota_migrate:
```

### 2. Build the installer

```bash
esphome compile installer.yaml
```

### 3. Upload to the Tasmota device

Via curl:

```bash
FIRMWARE=.esphome/build/tasmota-migrator/.pioenvs/tasmota-migrator/firmware.bin
curl -F "file=@${FIRMWARE};filename=firmware.bin" \
     "http://<TASMOTA_IP>/u2?fsz=$(stat -c%s ${FIRMWARE})"
```

Or via the Tasmota web UI: **Firmware Upgrade** > **Upload**.

The device reboots twice automatically (~15 seconds). No action required.

### 4. Upload the real ESPHome firmware

Create your device config using the provided partition table (see `device.example.yaml`):

```yaml
esp32:
  board: esp32dev
  framework:
    type: esp-idf
  partitions: partitions/tasmota_migrate_4MB.csv
```

Then upload:

```bash
esphome upload device.yaml --device <DEVICE_IP>
```

Done. The device is now running ESPHome with full dual-OTA support.

## How It Works

The migration happens in four steps across three reboots:

```
Tasmota running
       |
       | curl upload installer.bin via /u2
       v
Installer boots (1st boot)
  - Detects Tasmota safeboot partition layout
  - Rewrites partition table for ESPHome dual-OTA
  - Writes OTA boot selector
  - Reboots automatically
       |
       v
Installer boots (2nd boot)
  - No safeboot layout found -> migration done
  - ESPHome OTA listener active on port 3232
       |
       | esphome upload device.yaml
       v
Real ESPHome firmware running
  - Future OTA updates work normally
```

The key insight: Tasmota's `app0` partition sits at flash address `0xE0000`. The new partition table keeps `app0` at the same address, so the installer remains bootable after the partition table rewrite. The real ESPHome firmware is then written to `app1` via standard ESPHome OTA, and future updates alternate between both partitions normally.

### Partition Layout

**Before** (Tasmota safeboot):

| Offset | Size | Name | Purpose |
|--------|------|------|---------|
| 0x9000 | 20KB | nvs | Settings |
| 0xE000 | 8KB | otadata | Boot selector |
| 0x10000 | 832KB | safeboot | Recovery firmware (factory) |
| 0xE0000 | 1856KB | app0 | Main firmware (ota_0) |
| 0x2B0000 | 1344KB | spiffs | Filesystem |

**After** (ESPHome, 4MB flash):

| Offset | Size | Name | Purpose |
|--------|------|------|---------|
| 0x9000 | 20KB | nvs | Settings |
| 0xE000 | 8KB | otadata | Boot selector |
| 0x10000 | 4KB | phy_init | PHY calibration |
| 0x11000 | 828KB | spiffs | Filesystem |
| 0xE0000 | 1600KB | app0 | ESPHome OTA slot 0 |
| 0x270000 | 1600KB | app1 | ESPHome OTA slot 1 |

App partition sizes scale automatically with flash size:

| Flash | App partition size |
|-------|--------------------|
| 4MB | 1600KB |
| 8MB | 3648KB |
| 16MB | 7744KB |

## Device Config

Your real ESPHome device config **must** use the matching partition table CSV so that the build-time size checks match the flash layout:

```yaml
esp32:
  partitions: partitions/tasmota_migrate_4MB.csv
```

See `device.example.yaml` for a complete example.

## Limitations

- **ESP32 only** (all variants: ESP32, S2, S3, C3, C6, H2).
- **Tasmota safeboot layout required**. The component detects the layout by scanning for a factory partition named `safeboot`. Other layouts are ignored (component becomes a no-op).
- **Minimum 4MB flash**.
- **No recovery partition after migration**. If the ESPHome firmware is corrupt, serial flashing is needed. This is the same as a standard ESPHome installation.
- **NVS is reset**. Tasmota settings are not carried over.

## Project Structure

```
tasmota-migrate/
  components/
    tasmota_migrate/
      __init__.py              # ESPHome component registration
      tasmota_migrate.h        # C++ header
      tasmota_migrate.cpp      # Repartitioning implementation
  partitions/
    tasmota_migrate_4MB.csv    # Partition table for device builds
  installer.example.yaml       # Installer firmware config
  device.example.yaml          # Real device config example
```

## Technical Details

### Partition Table Format

The ESP32 partition table lives at flash offset `0x8000` (4KB). Each entry is 32 bytes:

```
Bytes 0-1:   Magic (0xAA50)
Byte  2:     Type (0=app, 1=data)
Byte  3:     SubType
Bytes 4-7:   Offset (little-endian)
Bytes 8-11:  Size (little-endian)
Bytes 12-27: Label (null-padded)
Bytes 28-31: Flags
```

Entries are followed by an MD5 trailer (magic `0xEBEB`, 14 bytes `0xFF` padding, 16 bytes MD5 hash of all entries). Remaining space is `0xFF`.

### OTA Data Format

Two 4KB sectors at `0xE000` and `0xF000` store the boot selection. Each holds a 32-byte structure:

```
Bytes 0-3:   ota_seq (sequence number, little-endian)
Bytes 4-27:  Padding (0xFF)
Bytes 28-31: CRC32 of ota_seq (init=0xFFFFFFFF)
```

The bootloader selects: `(highest_valid_seq - 1) % num_ota_partitions`.

The installer writes `ota_seq=1` which selects `(1-1) % 2 = 0` -> `ota_0` at `0xE0000`.

## License

MIT
