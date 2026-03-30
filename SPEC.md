# Tasmota-to-ESPHome Migration via OTA

## Problem

Devices running Tasmota with the safeboot partition layout cannot be migrated to
ESPHome without physical access for serial flashing. Tasmota's HTTP OTA endpoint
(`/u2`) accepts any valid ESP-IDF application binary, but ESPHome requires a
dual-OTA partition layout that differs from Tasmota's.

## Goal

Provide a one-shot ESPHome "installer" firmware that:

1. Is uploaded to a Tasmota safeboot device via its `/u2` HTTP endpoint
2. Automatically repartitions the flash for ESPHome's dual-OTA layout
3. Accepts the real ESPHome firmware via standard `esphome upload`

No serial access required. Two automatic reboots, then one `esphome upload`.

## Architecture

### Tasmota Safeboot Partition Layout (before migration)

```
Offset    Size     Name       Type    SubType
0x9000    20KB     nvs        data    nvs
0xE000    8KB      otadata    data    ota
0x10000   832KB    safeboot   app     factory
0xE0000   1856KB   app0       app     ota_0      ← installer lands here
0x2B0000  1344KB   spiffs     data    spiffs
```

The installer is uploaded via `/u2` and written to `app0` at 0xE0000.

### Target ESPHome Partition Layout (after migration, 4MB flash)

```
Offset    Size     Name       Type    SubType
0x9000    20KB     nvs        data    nvs
0xE000    8KB      otadata    data    ota
0x10000   4KB      phy_init   data    phy
0x11000   828KB    spiffs     data    spiffs
0xE0000   1600KB   app0       app     ota_0      ← same physical address
0x270000  1600KB   app1       app     ota_1
```

Key constraint: `app0` stays at 0xE0000 so the installer remains bootable after
the partition table rewrite.

### Dynamic Sizing

The layout scales to any flash size. App partitions fill the space from 0xE0000
to end of flash, split equally and 64KB-aligned:

| Flash | app0 size | app1 size | spiffs |
|-------|-----------|-----------|--------|
| 4MB   | 1600KB    | 1600KB    | 828KB  |
| 8MB   | 3648KB    | 3648KB    | 828KB  |
| 16MB  | 7744KB    | 7744KB    | 828KB  |

Formula:
```
app_size = ((flash_size - 0xE0000) / 2) & ~0xFFFF
```

## Migration Flow

```
┌─────────────────────────────────────────────────────────┐
│ Step 1: Upload installer to Tasmota                     │
│                                                         │
│   curl -F "file=@installer.bin"                         │
│        "http://<tasmota>/u2?fsz=$(stat -c%s installer)" │
│                                                         │
│   Tasmota validates 0xE9 magic, writes to app0,         │
│   sets boot partition, reboots.                         │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│ Step 2: Automatic repartition (no user action)          │
│                                                         │
│   Installer boots from app0 at 0xE0000.                 │
│   Detects Tasmota safeboot layout (factory partition     │
│   named "safeboot" at 0x10000).                         │
│                                                         │
│   Writes new partition table to 0x8000:                  │
│     nvs, otadata, phy_init, spiffs, app0, app1          │
│                                                         │
│   Writes otadata (ota_seq=1 → boots ota_0 at 0xE0000). │
│   Reboots automatically.                                │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│ Step 3: Installer boots with new partition table        │
│                                                         │
│   Installer runs from app0 (0xE0000), same binary.      │
│   No "safeboot" partition found → migration complete.    │
│   Standard ESPHome OTA listener active on port 3232.    │
│                                                         │
│   esp_ota_get_next_update_partition() → app1 (0x270000) │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│ Step 4: Upload real ESPHome firmware                    │
│                                                         │
│   esphome upload device.yaml --device <ip>              │
│                                                         │
│   Standard ESPHome OTA writes to app1 (0x270000).       │
│   Sets boot partition to app1. Reboots.                 │
│   Real ESPHome running. Future OTA alternates app0/1.   │
└─────────────────────────────────────────────────────────┘
```

Total reboots: 3 (Tasmota→installer, repartition, OTA upload).
User actions: 2 (curl upload, esphome upload).

## Implementation

### Component: `tasmota_migrate`

A custom ESPHome component with a single C++ class `TasmotaMigrateComponent`
that runs during `setup()`:

```
setup()
  ├── Read partition table from flash at 0x8000
  ├── Scan for factory partition labeled "safeboot"
  │
  ├── [Found] Tasmota layout detected:
  │   ├── Detect flash size via esp_flash_get_size()
  │   ├── Compute app partition sizes (64KB-aligned)
  │   ├── Build new partition table binary (entries + MD5)
  │   ├── Erase + write partition table at 0x8000
  │   ├── Erase + write otadata at 0xE000 (ota_seq=1 → app0)
  │   ├── Log completion
  │   └── esp_restart()
  │
  └── [Not found] Already migrated:
      └── Log "ready for OTA" and return
```

After the automatic reboot, the component finds no "safeboot" partition and
does nothing. The standard ESPHome OTA component handles firmware uploads.

### Partition Table Binary Format

Each entry is 32 bytes:
```
Offset  Size  Field
0       2     magic (0x50AA little-endian)
2       1     type (0=app, 1=data)
3       1     subtype
4       4     offset (little-endian)
8       4     size (little-endian)
12      16    label (null-padded)
28      4     flags
```

After all entries, an MD5 trailer:
```
Offset  Size  Field
0       2     magic (0xEBEB)
2       14    padding (0xFF)
16      16    MD5 of all preceding entries
```

Remaining bytes in the 4KB sector are 0xFF.

### OTA Data Format

Two 4KB sectors at 0xE000 and 0xF000. Each sector holds:
```
Offset  Size  Field
0       4     ota_seq (little-endian)
4       20    padding (0xFF)
24      4     ota_state (0xFFFFFFFF)
28      4     CRC32 of ota_seq (init=0xFFFFFFFF)
```

Boot selection: `(max_valid_seq - 1) % num_ota_partitions`

To boot ota_0 with 2 OTA partitions: write ota_seq=1, CRC32(1).
Both sectors erased first, then seq written to sector 0.

## Constraints

- **ESP32 only** (all variants: ESP32, S2, S3, C3, C6, H2)
- **Safeboot layout required**: the component detects layout by scanning for a
  factory partition named "safeboot". Non-safeboot Tasmota or other firmware
  is ignored (component becomes a no-op).
- **app0 must be at 0xE0000**: this is standard for all Tasmota safeboot builds.
- **Minimum flash 4MB**: required for two 1600KB app partitions.
- **No recovery partition**: after migration, there is no factory/safeboot
  fallback. If the ESPHome firmware is corrupt, serial flash is needed.
  (Trade-off: maximizes app partition size.)
- **NVS is reset**: Tasmota's NVS data uses different keys. The new NVS
  partition overlaps the old one at 0x9000 but ESPHome will initialize fresh.

## Files

```
tasmota-migrate/
├── SPEC.md                          # This document
├── components/
│   └── tasmota_migrate/
│       ├── __init__.py              # ESPHome component registration
│       ├── tasmota_migrate.h        # C++ header
│       └── tasmota_migrate.cpp      # C++ implementation
├── partitions/
│   └── tasmota_migrate_4MB.csv      # Matching partition table for builds
├── installer.example.yaml           # Installer firmware config
└── device.example.yaml              # Real device config with partitions
```

## User Guide

### Prerequisites

- Tasmota device with safeboot partition layout (ESP32)
- Device accessible on the network
- ESPHome CLI installed

### Step 1: Build the installer

Create `installer.yaml` with your WiFi credentials (see `installer.example.yaml`).

```bash
esphome compile installer.yaml
```

The output binary is at:
`.esphome/build/tasmota-migrator/.pioenvs/tasmota-migrator/firmware.bin`

### Step 2: Upload to Tasmota

```bash
FIRMWARE=.esphome/build/tasmota-migrator/.pioenvs/tasmota-migrator/firmware.bin
curl -F "file=@${FIRMWARE};filename=firmware.bin" \
     "http://<tasmota-ip>/u2?fsz=$(stat -c%s ${FIRMWARE})"
```

Or use the Tasmota web UI: Firmware Upgrade → Upload.

The device will reboot twice (~15 seconds). The installer LED (if configured)
will blink during repartitioning.

### Step 3: Upload real ESPHome firmware

```bash
esphome upload device.yaml --device <device-ip>
```

The device reboots into the real ESPHome firmware. Migration complete.

### Step 4: Verify

Check the ESPHome logs to confirm the device is running with the correct
partition layout. Future OTA updates work normally via `esphome upload`.
