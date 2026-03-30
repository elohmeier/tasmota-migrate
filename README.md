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

```mermaid
flowchart TD
    A["<b>Tasmota running</b>"] -->|"curl upload installer.bin via /u2"| B

    B["<b>Installer boots (1st boot)</b>
    Detects Tasmota safeboot layout
    Rewrites partition table for ESPHome dual-OTA
    Writes OTA boot selector
    Reboots automatically"]

    B --> C

    C["<b>Installer boots (2nd boot)</b>
    No safeboot layout found — migration done
    ESPHome OTA listener active on port 3232"]

    C -->|"esphome upload device.yaml"| D

    D["<b>Real ESPHome firmware running</b>
    Future OTA updates work normally"]

    style A fill:#e44,color:#fff
    style B fill:#f90,color:#fff
    style C fill:#f90,color:#fff
    style D fill:#2a2,color:#fff
```

The key insight: Tasmota's `app0` partition sits at flash address `0xE0000`. The new partition table keeps `app0` at the same address, so the installer remains bootable after the partition table rewrite. The real ESPHome firmware is then written to `app1` via standard ESPHome OTA, and future updates alternate between both partitions normally.

### Partition Layout

The flash is repartitioned in-place. `app0` stays at the same physical address so the installer remains bootable across the rewrite:

```mermaid
block-beta
  columns 6

  block:before:6
    columns 6
    bh["<b>Before</b> (Tasmota safeboot)"]:6
    bnvs["nvs\n20KB"]
    bota["otadata\n8KB"]
    bsafe["safeboot\n832KB"]:2
    bapp0["app0 (ota_0)\n1856KB"]:2
    bspiffs["spiffs\n1344KB"]:2
    bpad1[" "]:4
  end

  space:6

  block:after:6
    columns 6
    ah["<b>After</b> (ESPHome, 4MB)"]:6
    anvs["nvs\n20KB"]
    aota["otadata\n8KB"]
    aphy["phy\n4KB"]
    aspiffs["spiffs\n828KB"]
    aapp0["app0 (ota_0)\n1600KB"]:2
    aapp1["app1 (ota_1)\n1600KB"]:2
    apad1[" "]:2
  end

  style bsafe fill:#e44,color:#fff
  style bapp0 fill:#f90,color:#fff
  style bspiffs fill:#69c,color:#fff
  style bnvs fill:#888,color:#fff
  style bota fill:#888,color:#fff

  style aapp0 fill:#2a2,color:#fff
  style aapp1 fill:#2a2,color:#fff
  style aspiffs fill:#69c,color:#fff
  style anvs fill:#888,color:#fff
  style aota fill:#888,color:#fff
  style aphy fill:#888,color:#fff

  style bpad1 fill:none,stroke:none
  style apad1 fill:none,stroke:none
```

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
