import esphome.codegen as cg
from esphome.components import text_sensor
from esphome.const import CONF_ENTITY_CATEGORY, CONF_ICON, CONF_TYPE, ENTITY_CATEGORY_DIAGNOSTIC

from .. import CONF_LEVOIT_HUMIDIFIER_ID, levoit_humidifier_ns, typed_schema

TextSensorType = levoit_humidifier_ns.enum("TextSensorType", is_class=True)

TYPES = {
    "mcu_version": (
        TextSensorType.MCU_VERSION,
        {CONF_ICON: "mdi:chip", CONF_ENTITY_CATEGORY: ENTITY_CATEGORY_DIAGNOSTIC},
    ),
    # night light: "Color Cycle" or "Fixed Color"
    "night_light_mode": (
        TextSensorType.NIGHT_LIGHT_MODE,
        {CONF_ICON: "mdi:looks"},
    ),
    # night light: "Orange (#D25000)", "Cycling", or "Unknown (#rrggbb)"
    "night_light_color": (
        TextSensorType.NIGHT_LIGHT_COLOR,
        {CONF_ICON: "mdi:palette"},
    ),
}

CONFIG_SCHEMA = typed_schema(text_sensor.text_sensor_schema(), TYPES)


async def to_code(config):
    kind, _ = TYPES[config[CONF_TYPE]]
    hub = await cg.get_variable(config[CONF_LEVOIT_HUMIDIFIER_ID])
    var = await text_sensor.new_text_sensor(config)
    cg.add(hub.register_text_sensor(kind, var))
