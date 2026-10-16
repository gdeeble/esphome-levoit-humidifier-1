import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import CONF_COLOR, CONF_ICON, CONF_NAME, CONF_TYPE

from .. import CONF_LEVOIT_HUMIDIFIER_ID, levoit_humidifier_ns, typed_schema

LevoitSelect = levoit_humidifier_ns.class_("LevoitSelect", select.Select)
SelectType = levoit_humidifier_ns.enum("SelectType", is_class=True)

CONF_PRESETS = "presets"
CONF_FRONT_PANEL_PRESETS = "front_panel_presets"

OPTION_COLOR_CYCLE = "Color Cycle"
OPTION_CUSTOM = "Custom"

# the colours the night light button steps through, as the MCU reports them
FRONT_PANEL_PRESETS = [
    ("White", "FFFFFF"),
    ("Red", "D71E78"),
    ("Purple", "2800FF"),
    ("Light Purple", "1414BE"),
    ("Blue", "14288C"),
    ("Aqua", "14E6B4"),
    ("Orange", "D25000"),
]

# option order matches the C++ indexes: Auto, then manual at mist level 1 / 2,
# the same cycle as the button on the device
TYPES = {
    "mode": (SelectType.MODE, ["Auto", "Low", "High"], {CONF_ICON: "mdi:air-humidifier"}),
    # night light: Color Cycle, the front-panel colours, your own presets, then Custom
    # (shown for any other colour, e.g. one picked on the light entity)
    "night_light_preset": (SelectType.NIGHT_LIGHT_PRESET, None, {CONF_ICON: "mdi:palette"}),
}


def hex_color(value):
    value = cv.string_strict(value).strip().lstrip("#")
    if len(value) != 6:
        raise cv.Invalid("Expected a colour like '#FF8000'")
    try:
        int(value, 16)
    except ValueError as err:
        raise cv.Invalid("Expected a colour like '#FF8000'") from err
    return value.upper()


PRESET_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string_strict,
        cv.Required(CONF_COLOR): hex_color,
    }
)

_typed = typed_schema(
    select.select_schema(LevoitSelect).extend(
        {
            cv.Optional(CONF_PRESETS): cv.ensure_list(PRESET_SCHEMA),
            cv.Optional(CONF_FRONT_PANEL_PRESETS): cv.boolean,
        }
    ),
    TYPES,
)


def _presets(config):
    presets = []
    if config.get(CONF_FRONT_PANEL_PRESETS, True):
        presets.extend(FRONT_PANEL_PRESETS)
    presets.extend((p[CONF_NAME], p[CONF_COLOR]) for p in config.get(CONF_PRESETS, []))
    return presets


def CONFIG_SCHEMA(config):
    config = _typed(config)
    if config[CONF_TYPE] != "night_light_preset":
        for key in (CONF_PRESETS, CONF_FRONT_PANEL_PRESETS):
            if key in config:
                raise cv.Invalid(f"{key} is only for type: night_light_preset", path=[key])
        return config
    names = [OPTION_COLOR_CYCLE] + [n for n, _ in _presets(config)] + [OPTION_CUSTOM]
    seen = set()
    for n in names:
        if n in seen:
            raise cv.Invalid(f"Preset name '{n}' is used twice (Color Cycle and Custom are taken)", path=[CONF_PRESETS])
        seen.add(n)
    return config


async def to_code(config):
    kind, options, _ = TYPES[config[CONF_TYPE]]
    hub = await cg.get_variable(config[CONF_LEVOIT_HUMIDIFIER_ID])
    presets = []
    if config[CONF_TYPE] == "night_light_preset":
        presets = _presets(config)
        options = [OPTION_COLOR_CYCLE] + [n for n, _ in presets] + [OPTION_CUSTOM]
    var = await select.new_select(config, options=options)
    cg.add(var.set_parent(hub))
    cg.add(var.set_type(kind))
    for _, color in presets:
        v = int(color, 16)
        cg.add(var.add_preset((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF))
    if config[CONF_TYPE] == "night_light_preset":
        cg.add(var.follow_night_light())
    else:
        cg.add(hub.register_select(kind, var))
