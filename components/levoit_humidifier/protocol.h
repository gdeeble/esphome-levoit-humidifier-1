#pragma once

// The VeSync UART protocol between the ESP32 and the humidifier MCU, see docs/PROTOCOL.md.
// Kept free of ESPHome headers so it can be unit tested on the host (tests/host).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace levoit_humidifier {
namespace protocol {

//   A5 | ctrl | seq | len_lo | len_hi | checksum | payload[len]
//   payload: 01 | cmd_lo | cmd_hi | err | data...
// All bytes of a valid frame, checksum included, add up to 0xFF.
static const uint8_t FRAME_START = 0xA5;
static const size_t HEADER_LEN = 6;
static const uint16_t MAX_PAYLOAD = 0x5DC;
static const uint8_t PAYLOAD_VERSION = 0x01;
static const size_t PAYLOAD_HEADER_LEN = 4;

static const uint8_t CTRL_VERSION = 0x02;
static const uint8_t CTRL_ACK = 0x10;
static const uint8_t CTRL_WANT_ACK = 0x20;
static const uint8_t CTRL_ERROR = 0x40;

inline std::vector<uint8_t> encode_frame(uint8_t ctrl, uint8_t seq, uint16_t cmd, const std::vector<uint8_t> &data) {
  uint16_t len = PAYLOAD_HEADER_LEN + data.size();
  std::vector<uint8_t> frame = {FRAME_START, ctrl, seq, (uint8_t) (len & 0xFF), (uint8_t) (len >> 8), 0,
                                PAYLOAD_VERSION, (uint8_t) (cmd & 0xFF), (uint8_t) (cmd >> 8), 0};
  frame.insert(frame.end(), data.begin(), data.end());
  uint8_t sum = 0;
  for (uint8_t b : frame)
    sum += b;
  frame[5] = 0xFF - sum;
  return frame;
}

// A received frame with a valid checksum.
struct Frame {
  uint8_t ctrl{0};
  uint8_t seq{0};
  std::vector<uint8_t> payload;

  bool is_ack() const { return this->ctrl & CTRL_ACK; }
  bool wants_ack() const { return this->ctrl & CTRL_WANT_ACK; }
  bool is_error() const { return this->ctrl & CTRL_ERROR; }
  // version nibble 2 and a payload that starts with the payload header
  bool supported() const {
    return (this->ctrl & 0x0F) == CTRL_VERSION && this->payload.size() >= PAYLOAD_HEADER_LEN &&
           this->payload[0] == PAYLOAD_VERSION;
  }
  uint16_t cmd() const { return this->payload[1] | (this->payload[2] << 8); }
  uint8_t err() const { return this->payload[3]; }
  const uint8_t *data() const { return this->payload.data() + PAYLOAD_HEADER_LEN; }
  size_t data_len() const { return this->payload.size() - PAYLOAD_HEADER_LEN; }
};

// Byte-wise frame parser; bytes before a frame start are skipped.
class FrameParser {
 public:
  enum class Result { NONE, FRAME, BAD_CHECKSUM };

  Result feed(uint8_t b) {
    if (this->buf_.empty() && b != FRAME_START)
      return Result::NONE;
    this->buf_.push_back(b);
    if (this->buf_.size() < HEADER_LEN)
      return Result::NONE;
    uint16_t len = this->buf_[3] | (this->buf_[4] << 8);
    if (len > MAX_PAYLOAD) {
      this->buf_.clear();
      return Result::NONE;
    }
    if (this->buf_.size() < HEADER_LEN + len)
      return Result::NONE;

    uint8_t sum = 0;
    for (uint8_t c : this->buf_)
      sum += c;
    Result result = Result::BAD_CHECKSUM;
    if (sum == 0xFF) {
      this->frame_.ctrl = this->buf_[1];
      this->frame_.seq = this->buf_[2];
      this->frame_.payload.assign(this->buf_.begin() + HEADER_LEN, this->buf_.end());
      result = Result::FRAME;
    }
    this->raw_ = std::move(this->buf_);
    this->buf_.clear();
    return result;
  }

  // drop a partially received frame
  void reset() { this->buf_.clear(); }
  bool busy() const { return !this->buf_.empty(); }
  const Frame &frame() const { return this->frame_; }
  // all bytes of the last complete frame, for logging
  const std::vector<uint8_t> &raw() const { return this->raw_; }

 protected:
  std::vector<uint8_t> buf_;
  std::vector<uint8_t> raw_;
  Frame frame_;
};

// ---- status block: data of the 0x40C1 report and of the ACK to the 0x40C0 query ----

static const size_t STATUS_MIN_LEN = 15;
static const size_t STATUS_CLEAN_REMINDER = 20;  // only from MCUs that keep the reminder themselves
static const size_t STATUS_NIGHT_LIGHT = 15;     // on, mode, r, g, b (US model, MCU 2.0.02)
static const uint8_t NIGHT_LIGHT_CYCLE = 0;      // byte 16: colour cycle; 1 = fixed colour
static const uint8_t MODE_MANUAL = 1;            // 0 = auto; the stock firmware's internal mode 4
static const uint8_t HUMIDITY_HIGH_ABOVE = 80;   // the stock firmware's humidity_high

// Modes as the button on the device cycles them: Auto, then manual at mist level 1 / 2.
// Capitalised: Home Assistant shows custom humidifier modes exactly as sent.
static const char *const MODE_NAMES[] = {"Auto", "Low", "High"};
static const size_t MODE_COUNT = 3;

struct Status {
  uint8_t fw_major{0}, fw_minor{0}, fw_patch{0};
  bool power{false};
  bool tank_lifted{false};    // reported to the app as water_tank_lifted
  bool water_lacks{false};    // also set all the time while off (MCU 2.0.02)
  bool auto_stopped{false};   // target reached, paused by auto stop
  uint8_t display_config{0};  // brightness setting, 0 = off
  bool fog{false};            // "fog_status": 1 even while off, meaning unknown
  uint8_t target{0};
  uint8_t humidity{0};
  uint8_t temperature{0};
  uint8_t mode{0};
  uint8_t mist_level{0};
  uint8_t exception{0};       // non-zero: the stock firmware raises a humidifierError
  bool has_clean_reminder{false};
  bool clean_reminder{false};
  bool has_night_light{false};
  bool night_light_on{false};
  bool night_light_cycle{false};  // colour cycle instead of a fixed colour
  uint8_t night_light_r{0}, night_light_g{0}, night_light_b{0};  // fixed colour, kept while off

  bool manual() const { return this->mode == MODE_MANUAL; }
  // the app counts the display as on only while the humidifier is on
  bool display_on() const { return this->display_config > 0 && this->power; }
  // water_lacks is meaningless while off
  bool low_water() const { return this->water_lacks && this->power; }
  bool mist_active() const { return this->power && !this->tank_lifted && !this->water_lacks && !this->auto_stopped; }
  bool humidity_high() const { return this->humidity > HUMIDITY_HIGH_ABOVE; }
  // index into MODE_NAMES: 0 = auto, 1 = low, 2 = high
  size_t mode_index() const {
    if (!this->manual())
      return 0;
    return this->mist_level >= 2 ? 2 : 1;
  }
};

inline bool decode_status(const uint8_t *d, size_t len, Status &s) {
  if (len < STATUS_MIN_LEN)
    return false;
  s.fw_patch = d[0];
  s.fw_minor = d[1];
  s.fw_major = d[2];
  s.power = d[3];
  s.tank_lifted = d[4];
  s.water_lacks = d[5];
  s.auto_stopped = d[6];
  s.display_config = d[7];
  s.fog = d[8];
  s.target = d[9];
  s.humidity = d[10];
  s.temperature = d[11];
  s.mode = d[12];
  s.mist_level = d[13];
  s.exception = d[14];
  s.has_clean_reminder = len > STATUS_CLEAN_REMINDER;
  s.clean_reminder = s.has_clean_reminder && d[STATUS_CLEAN_REMINDER];
  s.has_night_light = len > STATUS_NIGHT_LIGHT + 4;
  if (s.has_night_light) {
    const uint8_t *n = d + STATUS_NIGHT_LIGHT;
    s.night_light_on = n[0];
    s.night_light_cycle = n[1] == NIGHT_LIGHT_CYCLE;
    s.night_light_r = n[2];
    s.night_light_g = n[3];
    s.night_light_b = n[4];
  }
  return true;
}

// The colours the night light button steps through, as the status reports them.
struct NightLightColor {
  const char *name;
  uint8_t r, g, b;
};
static const NightLightColor NIGHT_LIGHT_COLORS[] = {
    {"White", 0xFF, 0xFF, 0xFF}, {"Red", 0xD7, 0x1E, 0x78},  {"Purple", 0x28, 0x00, 0xFF},
    {"Light Purple", 0x14, 0x14, 0xBE}, {"Blue", 0x14, 0x28, 0x8C}, {"Aqua", 0x14, 0xE6, 0xB4},
    {"Orange", 0xD2, 0x50, 0x00},
};

// name of a button colour, nullptr for any other RGB value
inline const char *night_light_color_name(uint8_t r, uint8_t g, uint8_t b) {
  for (const auto &c : NIGHT_LIGHT_COLORS) {
    if (c.r == r && c.g == g && c.b == b)
      return c.name;
  }
  return nullptr;
}

// ---- raw commands, for finding unknown opcodes ----

// "A003 01 01 D2 50 00" -> cmd 0xA003, data {01, 01, D2, 50, 00}. The opcode is written as
// a number (it goes on the wire low byte first); spaces, ':' and '-' separate bytes.
inline bool parse_raw_command(const std::string &text, uint16_t &cmd, std::vector<uint8_t> &data) {
  std::vector<uint8_t> nibbles;
  for (char c : text) {
    if (c == ' ' || c == ':' || c == '-' || c == ',')
      continue;
    if (c >= '0' && c <= '9')
      nibbles.push_back(c - '0');
    else if (c >= 'a' && c <= 'f')
      nibbles.push_back(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      nibbles.push_back(c - 'A' + 10);
    else if (c == 'x' && nibbles.size() == 1 && nibbles[0] == 0)
      nibbles.clear();  // "0x" prefix
    else
      return false;
  }
  if (nibbles.size() < 4 || nibbles.size() % 2 != 0)
    return false;
  cmd = (nibbles[0] << 12) | (nibbles[1] << 8) | (nibbles[2] << 4) | nibbles[3];
  data.clear();
  for (size_t i = 4; i < nibbles.size(); i += 2)
    data.push_back((nibbles[i] << 4) | nibbles[i + 1]);
  return true;
}

// ---- command data ----

// auto mode: target with a +-5 % band, the layout the stock firmware sends
inline std::vector<uint8_t> auto_mode_data(uint8_t target) {
  return {target, (uint8_t) (target - 5), (uint8_t) (target + 5), 2, 1, 1};
}

// manual mode at a mist level (1-2)
inline std::vector<uint8_t> manual_level_data(uint8_t level) { return {0, 1, level}; }

// set_night_light (0x40C2) data, as confirmed on hardware: [0] = off, [1, 0] = colour
// cycle, [1, 1, r, g, b] = fixed colour (any RGB value, not only the button colours)
inline std::vector<uint8_t> night_light_data(bool on, bool cycle, uint8_t r, uint8_t g, uint8_t b) {
  if (!on)
    return {0};
  if (cycle)
    return {1, NIGHT_LIGHT_CYCLE};
  return {1, 1, r, g, b};
}

// true when the status already shows this night light state (RGB within +-1 for rounding)
inline bool night_light_matches(const Status &s, bool on, bool cycle, uint8_t r, uint8_t g, uint8_t b) {
  if (!s.has_night_light || s.night_light_on != on)
    return false;
  if (!on)
    return true;
  if (s.night_light_cycle != cycle)
    return false;
  if (cycle)
    return true;
  auto near = [](uint8_t a, uint8_t b) { return (a > b ? a - b : b - a) <= 1; };
  return near(s.night_light_r, r) && near(s.night_light_g, g) && near(s.night_light_b, b);
}

// set_wifi_led data: [mode, on_ms u16, off_ms u16, repeat], values from the stock LED table
inline std::vector<uint8_t> wifi_led_on_data() { return {1, 125, 0, 125, 0, 0}; }
inline std::vector<uint8_t> wifi_led_blink_data() { return {2, 0xF4, 0x01, 0xF4, 0x01, 0}; }  // 500 ms
inline std::vector<uint8_t> wifi_led_slow_blink_data() { return {2, 0xE8, 0x03, 0xE8, 0x03, 0}; }  // 1 s
inline std::vector<uint8_t> wifi_led_off_data() { return {0, 75, 0, 75, 0, 0}; }

}  // namespace protocol
}  // namespace levoit_humidifier
}  // namespace esphome
