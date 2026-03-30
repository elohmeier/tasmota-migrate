import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, PLATFORM_ESP32

CODEOWNERS = ["@esphome"]
DEPENDENCIES = ["wifi"]

CONF_TASMOTA_MIGRATE = "tasmota_migrate"

tasmota_migrate_ns = cg.esphome_ns.namespace("tasmota_migrate")
TasmotaMigrateComponent = tasmota_migrate_ns.class_(
    "TasmotaMigrateComponent", cg.Component
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TasmotaMigrateComponent),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
