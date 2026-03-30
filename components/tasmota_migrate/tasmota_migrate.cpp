#include "tasmota_migrate.h"
#include "esphome/core/log.h"

#include <cstring>
#include <esp_flash.h>
#include <esp_rom_crc.h>
#include <esp_rom_md5.h>
#include <esp_system.h>

namespace esphome {
namespace tasmota_migrate {

static const char *const TAG = "tasmota_migrate";

// Flash layout constants
static const uint32_t PARTITION_TABLE_OFFSET = 0x8000;
static const uint32_t PARTITION_TABLE_SIZE = 0x1000;  // 4KB
static const uint32_t OTADATA_OFFSET = 0xE000;
static const uint32_t OTADATA_SIZE = 0x2000;  // 8KB (two 4KB sectors)
static const uint32_t APP_START = 0xE0000;    // Tasmota app0 / our app0

// Partition table magic numbers
static const uint16_t PART_MAGIC = 0x50AA;
static const uint16_t MD5_MAGIC = 0xEBEB;

// Partition types
static const uint8_t TYPE_APP = 0x00;
static const uint8_t TYPE_DATA = 0x01;

// Partition subtypes
static const uint8_t SUBTYPE_APP_FACTORY = 0x00;
static const uint8_t SUBTYPE_APP_OTA_0 = 0x10;
static const uint8_t SUBTYPE_APP_OTA_1 = 0x11;
static const uint8_t SUBTYPE_DATA_OTA = 0x00;
static const uint8_t SUBTYPE_DATA_PHY = 0x01;
static const uint8_t SUBTYPE_DATA_NVS = 0x02;
static const uint8_t SUBTYPE_DATA_SPIFFS = 0x82;

// 64KB alignment mask for app partitions
static const uint32_t ALIGN_64K = 0xFFFF0000;

float TasmotaMigrateComponent::get_setup_priority() const {
  // Run very early, before OTA and other network components
  return setup_priority::HARDWARE;
}

PartitionEntry TasmotaMigrateComponent::make_entry_(uint32_t offset, uint32_t size, uint8_t type, uint8_t subtype,
                                                     const char *label) {
  PartitionEntry e{};
  e.magic = PART_MAGIC;
  e.type = type;
  e.subtype = subtype;
  e.offset = offset;
  e.size = size;
  std::strncpy(e.label, label, sizeof(e.label));
  e.flags = 0;
  return e;
}

bool TasmotaMigrateComponent::is_tasmota_safeboot_layout_() {
  uint8_t buf[PARTITION_TABLE_SIZE];
  esp_err_t err = esp_flash_read(nullptr, buf, PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read partition table: %d", err);
    return false;
  }

  // Scan for a factory partition labeled "safeboot"
  for (int i = 0; i < 95; i++) {
    auto *entry = reinterpret_cast<PartitionEntry *>(buf + i * 32);
    if (entry->magic != PART_MAGIC)
      break;

    if (entry->type == TYPE_APP && entry->subtype == SUBTYPE_APP_FACTORY) {
      if (std::strncmp(entry->label, "safeboot", sizeof(entry->label)) == 0) {
        ESP_LOGI(TAG, "Found Tasmota safeboot partition at 0x%06X (%uKB)", entry->offset, entry->size / 1024);
        return true;
      }
    }
  }

  return false;
}

bool TasmotaMigrateComponent::write_partition_table_() {
  // Detect flash size
  uint32_t flash_size = 0;
  esp_err_t err = esp_flash_get_size(nullptr, &flash_size);
  if (err != ESP_OK || flash_size == 0) {
    ESP_LOGE(TAG, "Failed to detect flash size: %d", err);
    return false;
  }
  ESP_LOGI(TAG, "Flash size: %uMB", flash_size / (1024 * 1024));

  if (flash_size < 0x400000) {
    ESP_LOGE(TAG, "Flash too small (%u bytes), need at least 4MB", flash_size);
    return false;
  }

  // Compute app partition sizes
  // Both app partitions start at APP_START and fill to end of flash, split equally
  uint32_t available = flash_size - APP_START;
  uint32_t app_size = (available / 2) & ALIGN_64K;  // 64KB-aligned

  if (app_size < 0x80000) {  // 512KB minimum
    ESP_LOGE(TAG, "App partition too small: %uKB", app_size / 1024);
    return false;
  }

  uint32_t app1_offset = APP_START + app_size;

  ESP_LOGI(TAG, "New layout: app0=0x%06X(%uKB) app1=0x%06X(%uKB)", APP_START, app_size / 1024, app1_offset,
           app_size / 1024);

  // Build partition entries
  PartitionEntry entries[6];
  int n = 0;

  // nvs: 0x9000, 20KB
  entries[n++] = make_entry_(0x9000, 0x5000, TYPE_DATA, SUBTYPE_DATA_NVS, "nvs");
  // otadata: 0xE000, 8KB
  entries[n++] = make_entry_(0xE000, 0x2000, TYPE_DATA, SUBTYPE_DATA_OTA, "otadata");
  // phy_init: 0x10000, 4KB
  entries[n++] = make_entry_(0x10000, 0x1000, TYPE_DATA, SUBTYPE_DATA_PHY, "phy_init");
  // spiffs: 0x11000, fill gap to APP_START
  uint32_t spiffs_size = APP_START - 0x11000;
  entries[n++] = make_entry_(0x11000, spiffs_size, TYPE_DATA, SUBTYPE_DATA_SPIFFS, "spiffs");
  // app0 (ota_0)
  entries[n++] = make_entry_(APP_START, app_size, TYPE_APP, SUBTYPE_APP_OTA_0, "app0");
  // app1 (ota_1)
  entries[n++] = make_entry_(app1_offset, app_size, TYPE_APP, SUBTYPE_APP_OTA_1, "app1");

  // Serialize entries to binary
  uint32_t entries_size = n * sizeof(PartitionEntry);

  // Compute MD5 over all entries
  uint8_t md5_hash[16];
  md5_context_t md5_ctx;
  esp_rom_md5_init(&md5_ctx);
  esp_rom_md5_update(&md5_ctx, reinterpret_cast<uint8_t *>(entries), entries_size);
  esp_rom_md5_final(md5_hash, &md5_ctx);

  // Build MD5 trailer entry (32 bytes)
  uint8_t md5_entry[32];
  std::memset(md5_entry, 0xFF, sizeof(md5_entry));
  md5_entry[0] = 0xEB;
  md5_entry[1] = 0xEB;
  std::memcpy(md5_entry + 16, md5_hash, 16);

  // Assemble the full 4KB partition table
  uint8_t table[PARTITION_TABLE_SIZE];
  std::memset(table, 0xFF, PARTITION_TABLE_SIZE);
  std::memcpy(table, entries, entries_size);
  std::memcpy(table + entries_size, md5_entry, sizeof(md5_entry));

  // Erase and write
  err = esp_flash_erase_region(nullptr, PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to erase partition table: %d", err);
    return false;
  }

  err = esp_flash_write(nullptr, table, PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write partition table: %d", err);
    return false;
  }

  ESP_LOGI(TAG, "Partition table written successfully (%d entries)", n);
  return true;
}

bool TasmotaMigrateComponent::write_otadata_() {
  // Write otadata to boot ota_0 (app0 at APP_START).
  // With 2 OTA partitions: (ota_seq - 1) % 2 = partition number.
  // ota_seq=1 → (1-1) % 2 = 0 → boots ota_0.
  uint32_t ota_seq = 1;
  uint32_t crc = esp_rom_crc32_le(0xFFFFFFFF, reinterpret_cast<const uint8_t *>(&ota_seq), sizeof(ota_seq));

  // Build the 32-byte otadata structure
  uint8_t otadata[32];
  std::memset(otadata, 0xFF, sizeof(otadata));
  std::memcpy(otadata + 0, &ota_seq, 4);  // ota_seq at offset 0
  std::memcpy(otadata + 28, &crc, 4);     // CRC32 at offset 28

  // Erase both otadata sectors (0xE000 and 0xF000)
  esp_err_t err = esp_flash_erase_region(nullptr, OTADATA_OFFSET, OTADATA_SIZE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to erase otadata: %d", err);
    return false;
  }

  // Write to first sector only
  err = esp_flash_write(nullptr, otadata, OTADATA_OFFSET, sizeof(otadata));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write otadata: %d", err);
    return false;
  }

  ESP_LOGI(TAG, "OTA data written: ota_seq=1 (boot ota_0 at 0x%06X)", APP_START);
  return true;
}

void TasmotaMigrateComponent::setup() {
  if (!this->is_tasmota_safeboot_layout_()) {
    ESP_LOGI(TAG, "No Tasmota safeboot layout detected. Ready for normal operation.");
    return;
  }

  ESP_LOGW(TAG, "=========================================");
  ESP_LOGW(TAG, "Tasmota safeboot layout detected!");
  ESP_LOGW(TAG, "Repartitioning for ESPHome dual-OTA...");
  ESP_LOGW(TAG, "=========================================");

  if (!this->write_partition_table_()) {
    ESP_LOGE(TAG, "FAILED to write partition table! Aborting migration.");
    this->mark_failed();
    return;
  }

  if (!this->write_otadata_()) {
    ESP_LOGE(TAG, "FAILED to write OTA data! Aborting migration.");
    this->mark_failed();
    return;
  }

  ESP_LOGW(TAG, "=========================================");
  ESP_LOGW(TAG, "Repartitioning complete!");
  ESP_LOGW(TAG, "Rebooting in 2 seconds...");
  ESP_LOGW(TAG, "After reboot, upload ESPHome firmware:");
  ESP_LOGW(TAG, "  esphome upload <config> --device <ip>");
  ESP_LOGW(TAG, "=========================================");

  // Brief delay to allow log output to flush
  delay(2000);
  esp_restart();
}

}  // namespace tasmota_migrate
}  // namespace esphome
