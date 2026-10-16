#pragma once

#include <cstdint>

namespace esphome {
namespace levoit_humidifier {

enum class SwitchType : uint8_t { POWER, DISPLAY, AUTO_STOP, COUNT };
enum class NumberType : uint8_t { TARGET_HUMIDITY, MIST_LEVEL, TIMER, COUNT };
enum class SelectType : uint8_t { MODE, NIGHT_LIGHT_PRESET, COUNT };
enum class SensorType : uint8_t { HUMIDITY, TEMPERATURE, COUNT };
enum class BinarySensorType : uint8_t { WATER_TANK_LIFTED, LOW_WATER, MIST_ACTIVE, AUTO_STOPPED, CLEAN_REMINDER, HUMIDITY_HIGH, FAULT, NIGHT_LIGHT, COUNT };
enum class TextSensorType : uint8_t { MCU_VERSION, NIGHT_LIGHT_MODE, NIGHT_LIGHT_COLOR, COUNT };
enum class ButtonType : uint8_t { RESET_CLEAN_REMINDER, COUNT };

// Opcodes, see docs/PROTOCOL.md
static const uint16_t CMD_SET_AUTO_MODE = 0x4080;
static const uint16_t CMD_QUERY_STATUS = 0x40C0;
static const uint16_t CMD_STATUS_REPORT = 0x40C1;
// ESP -> MCU: reset the tank-cleaning reminder; MCU -> ESP: it was reset on the device
static const uint16_t CMD_CLEAN_RESET = 0x40C4;
// ESP -> MCU: night light (US model), found by scanning; [1] = on, rest of the layout TBD
static const uint16_t CMD_SET_NIGHT_LIGHT = 0x40C2;
static const uint16_t CMD_SET_POWER = 0xA000;
static const uint16_t CMD_SET_DISPLAY = 0xA105;
static const uint16_t CMD_SET_WIFI_LED = 0xA129;
static const uint16_t CMD_SET_MANUAL_LEVEL = 0xA260;
static const uint16_t CMD_SET_TIMER_ICON = 0xA26A;
static const uint16_t CMD_SET_AUTO_STOP = 0xA5E5;
// MCU -> ESP button events, data[0]: 2/3 = Wi-Fi pairing, 4 = factory reset
static const uint16_t CMD_BUTTON_FIRST = 0xD102;
static const uint16_t CMD_BUTTON_LAST = 0xD103;
static const uint16_t CMD_TEST_MODE = 0xD104;

static const uint8_t MIN_TARGET_HUMIDITY = 30;
static const uint8_t MAX_TARGET_HUMIDITY = 80;
static const uint8_t MAX_MIST_LEVEL = 2;
static const uint16_t MAX_TIMER_MINUTES = 1440;

}  // namespace levoit_humidifier
}  // namespace esphome
