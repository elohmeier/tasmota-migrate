#pragma once

#include "esphome/core/component.h"

#include <cstdint>

namespace esphome {
namespace tasmota_migrate {

/// Partition entry as stored in the ESP32 partition table (32 bytes).
struct PartitionEntry {
  uint16_t magic;
  uint8_t type;
  uint8_t subtype;
  uint32_t offset;
  uint32_t size;
  char label[16];
  uint32_t flags;
} __attribute__((packed));

static_assert(sizeof(PartitionEntry) == 32, "PartitionEntry must be 32 bytes");

class TasmotaMigrateComponent : public Component {
 public:
  void setup() override;
  float get_setup_priority() const override;

 protected:
  /// Check if current partition table has a Tasmota safeboot factory partition.
  bool is_tasmota_safeboot_layout_();

  /// Copy the running firmware from src to dst (flash-to-flash, sector by sector).
  bool copy_firmware_(uint32_t dst, uint32_t src, uint32_t size);

  /// Get the size of the firmware image at the given flash address by parsing ESP-IDF image headers.
  int32_t get_image_size_(uint32_t addr);

  /// Write the stock ESPHome partition table to flash at 0x8000.
  bool write_partition_table_(uint32_t app_size);

  /// Write otadata at 0x9000 to select ota_0 (app0) for next boot.
  bool write_otadata_(uint32_t otadata_offset);

  /// Build a single partition entry.
  static PartitionEntry make_entry_(uint32_t offset, uint32_t size, uint8_t type, uint8_t subtype, const char *label);
};

}  // namespace tasmota_migrate
}  // namespace esphome
