import esphome.codegen as cg
from esphome.components import light
from esphome.components.light.effects import register_rgb_effect
from esphome.components.light.types import LightEffect
import esphome.config_validation as cv
from esphome.const import (
    CONF_DEFAULT_TRANSITION_LENGTH,
    CONF_EFFECTS,
    CONF_GAMMA_CORRECT,
    CONF_NAME,
)

from .. import CONF_LEVOIT_HUMIDIFIER_ID, HUB_SCHEMA, levoit_humidifier_ns

DEPENDENCIES = ["levoit_humidifier"]

LevoitLight = levoit_humidifier_ns.class_("LevoitLight", light.LightOutput)
LevoitColorCycleEffect = levoit_humidifier_ns.class_("LevoitColorCycleEffect", LightEffect)

CONF_COLOR_CYCLE_EFFECT = "levoit_color_cycle"


# The night light's own colour cycle, run by the humidifier (0x40C2 [1, 0]). Only works on
# the levoit_humidifier light.
@register_rgb_effect(CONF_COLOR_CYCLE_EFFECT, LevoitColorCycleEffect, "Color Cycle", {})
async def color_cycle_effect_to_code(config, effect_id):
    return cg.new_Pvariable(effect_id, config[CONF_NAME])


_SCHEMA = (
    light.light_schema(LevoitLight, light.LightType.RGB, icon="mdi:lightbulb-night")
    .extend(HUB_SCHEMA)
    .extend(
        {
            # the MCU takes the RGB value as is: no gamma, and no fades (each step would be
            # a UART command)
            cv.Optional(CONF_GAMMA_CORRECT, default=1.0): cv.positive_float,
            cv.Optional(
                CONF_DEFAULT_TRANSITION_LENGTH, default="0s"
            ): cv.positive_time_period_milliseconds,
        }
    )
)


def CONFIG_SCHEMA(config):
    # the colour cycle effect is there unless the effects are set explicitly
    if isinstance(config, dict) and CONF_EFFECTS not in config:
        config = {**config, CONF_EFFECTS: [{CONF_COLOR_CYCLE_EFFECT: {}}]}
    return _SCHEMA(config)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_LEVOIT_HUMIDIFIER_ID])
    var = await light.new_light(config)
    cg.add(var.set_parent(hub))
