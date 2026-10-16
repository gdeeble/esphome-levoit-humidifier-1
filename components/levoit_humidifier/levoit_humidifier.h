#pragma once

#include <array>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/number/number.h"
#include "esphome/components/select/select.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "protocol.h"
#include "types.h"

namespace esphome {
namespace levoit_humidifier {

// What lights the Wi-Fi icon on the display: Wi-Fi alone, or a Home Assistant connection
// (the closest match to the stock firmware's "cloud connected").
enum class WifiIconSource : uint8_t { WIFI, HOME_ASSISTANT };

// Talks to the humidifier MCU over the VeSync UART protocol (protocol.h).
// One command is in flight at a time; it is resent until the MCU ACKs it.
class LevoitHumidifier : public PollingComponent, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void register_switch(SwitchType t, switch_::Switch *s) { this->switches_[(size_t) t] = s; }
  void register_number(NumberType t, number::Number *n) { this->numbers_[(size_t) t] = n; }
  void register_select(SelectType t, select::Select *s) { this->selects_[(size_t) t] = s; }
  void register_sensor(SensorType t, sensor::Sensor *s) { this->sensors_[(size_t) t] = s; }
  void register_binary_sensor(BinarySensorType t, binary_sensor::BinarySensor *s) {
    this->binary_sensors_[(size_t) t] = s;
  }
  void register_text_sensor(TextSensorType t, text_sensor::TextSensor *s) { this->text_sensors_[(size_t) t] = s; }

  // keys this hub's saved settings, so several humidifiers do not share them
  void set_preference_key(const std::string &key) { this->preference_key_ = key; }
  void set_wifi_icon_source(WifiIconSource source) { this->wifi_icon_source_ = source; }
  void set_mqtt_humidifier(bool enabled) { this->mqtt_humidifier_ = enabled; }
  void set_mqtt_discovery_prefix(const std::string &prefix) { this->mqtt_discovery_prefix_ = prefix; }

  void set_power(bool on);
  // like the stock firmware, refused while the humidifier is off; returns false then
  bool set_display(bool on);
  void set_auto_stop(bool on);
  // Return false when the change is refused (target outside 30-80 %, or while off / in
  // manual). Mode and mist level are always sent, also without water: the display follows
  // and the MCU decides whether it can mist. They switch the humidifier on first.
  bool set_auto_mode(bool on);
  bool set_target_humidity(uint8_t target);
  bool set_mist_level(uint8_t level);
  // the device's own modes: 0 = Auto, 1 = Low, 2 = High (manual at mist level 1 / 2)
  bool set_mode(size_t index);
  // target from the humidifier entity: like turning the dial in the app, it moves to Auto
  bool set_auto_target(uint8_t target);

  // Timer like the VeSync app: after `minutes` the power flips to the opposite of what it
  // is now; 0 cancels. Lights the timer icon while running. It is cancelled when the power
  // reaches that state some other way, so it never switches back on after a manual off.
  void set_timer(uint16_t minutes);
  void reset_clean_reminder();

  // Night light (US model): off, colour cycle, or a fixed colour (any RGB). Skipped when the
  // status already shows that state and no night light command is pending.
  void set_night_light(bool on, bool cycle, uint8_t r, uint8_t g, uint8_t b);
  // called with every status block that carries the night light (the light / preset select)
  void add_night_light_listener(std::function<void(const protocol::Status &)> &&f) {
    this->night_light_listeners_.push_back(std::move(f));
  }
  bool has_status() const { return this->have_status_; }
  const protocol::Status &status() const { return this->status_; }

  // Send any opcode, for finding unknown commands (the night light). The MCU's answer is
  // logged at INFO, followed by a status read-back. Careful: some opcodes are factory
  // functions (0xD0xx / 0xD1xx). Returns false when the text does not parse.
  void send_raw(uint16_t cmd, const std::vector<uint8_t> &data);
  // Text form: "A003 01 01 D2 50 00", or "SCAN A000 A0FF [data]" to send every opcode in
  // the range once (known ones and 0xD0xx/0xD1xx skipped) and log which ones the MCU ACKs.
  // The MCU ignores opcodes it does not know, so an ACK means it has a handler.
  bool send_raw(const std::string &text);
  void scan_opcodes(uint16_t first, uint16_t last, const std::vector<uint8_t> &data);

 protected:
  struct Command {
    uint16_t cmd;
    std::vector<uint8_t> data;
    bool raw{false};   // from send_raw: log the MCU's whole answer
    bool scan{false};  // from an opcode scan: no resend, short timeout
  };

  // Wi-Fi icon on the humidifier display, driven like the stock firmware does
  enum class WifiLed : uint8_t { UNKNOWN, OFF, ON, BLINK };
  void update_wifi_led_(bool force);
  bool connected_() const;

  void queue_command_(uint16_t cmd, std::vector<uint8_t> data);
  void send_next_();
  void write_frame_(uint8_t ctrl, uint8_t seq, uint16_t cmd, const std::vector<uint8_t> &data);
  void read_uart_();
  void handle_frame_(const protocol::Frame &frame);
  void handle_status_(const uint8_t *data, size_t len);
  void publish_status_(const protocol::Status &s, bool first);
  void handle_button_(uint16_t cmd, uint8_t value);

  // Optional Home Assistant humidifier entity over MQTT discovery, on the same device as
  // the ESPHome entities (matched by MAC address). With it disabled, a previously
  // announced entity is removed again.
  void mqtt_setup_();
  void mqtt_loop_();
  std::string mqtt_topic_(const char *suffix) const;
  std::string mqtt_discovery_topic_() const;
  bool mqtt_humidifier_{false};
  std::string mqtt_discovery_prefix_{"homeassistant"};
  bool mqtt_announced_{false};
  bool mqtt_dirty_{true};

  void send_auto_mode_();
  void cancel_timer_();
  void publish_timer_();

  bool pending_(uint16_t cmd) const;
  std::vector<std::function<void(const protocol::Status &)>> night_light_listeners_;

  std::deque<Command> queue_;
  Command in_flight_{};
  bool awaiting_ack_{false};
  uint8_t tx_seq_{0};
  uint32_t sent_at_{0};
  uint8_t retries_{0};
  size_t scan_left_{0};
  std::vector<uint16_t> scan_hits_;
  bool scan_control_ok_{false};
  void scan_done_one_(bool acked, uint16_t cmd, uint8_t ctrl, uint8_t err);

  protocol::FrameParser parser_;
  uint32_t last_rx_{0};

  bool have_status_{false};
  protocol::Status status_{};
  std::vector<uint8_t> last_raw_status_;
  bool auto_stop_{false};
  std::string preference_key_{"levoit_humidifier"};
  ESPPreferenceObject auto_stop_pref_;
  WifiIconSource wifi_icon_source_{WifiIconSource::WIFI};
  WifiLed wifi_led_{WifiLed::UNKNOWN};
  uint8_t target_humidity_{45};
  uint8_t mist_level_{1};

  bool timer_active_{false};
  bool timer_power_{false};
  uint32_t timer_end_{0};
  uint32_t timer_published_at_{0};

  std::array<switch_::Switch *, (size_t) SwitchType::COUNT> switches_{};
  std::array<number::Number *, (size_t) NumberType::COUNT> numbers_{};
  std::array<select::Select *, (size_t) SelectType::COUNT> selects_{};
  std::array<sensor::Sensor *, (size_t) SensorType::COUNT> sensors_{};
  std::array<binary_sensor::BinarySensor *, (size_t) BinarySensorType::COUNT> binary_sensors_{};
  std::array<text_sensor::TextSensor *, (size_t) TextSensorType::COUNT> text_sensors_{};
};

}  // namespace levoit_humidifier
}  // namespace esphome
