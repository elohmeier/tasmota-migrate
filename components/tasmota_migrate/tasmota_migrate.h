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

  /// Write the new ESPHome-compatible partition table to flash at 0x8000.
  bool write_partition_table_();

  /// Write otadata at 0xE000 to select ota_0 (app0) for next boot.
  bool write_otadata_();

  /// Build a single partition entry.
  static PartitionEntry make_entry_(uint32_t offset, uint32_t size, uint8_t type, uint8_t subtype, const char *label);
};

}  // namespace tasmota_migrate
}  // namespace esphome
