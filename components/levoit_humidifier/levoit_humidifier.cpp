#include "levoit_humidifier.h"

#include <algorithm>
#include <cmath>

#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif
#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/core/application.h"
#endif

namespace esphome {
namespace levoit_humidifier {

using namespace protocol;

static const char *const TAG = "levoit_humidifier";

static const uint32_t ACK_TIMEOUT_MS = 1000;
static const uint32_t SCAN_ACK_TIMEOUT_MS = 400;
static const uint8_t MAX_RETRIES = 3;
static const uint32_t RX_GAP_MS = 100;

void LevoitHumidifier::setup() {
  this->auto_stop_pref_ =
      global_preferences->make_preference<bool>(fnv1_hash(this->preference_key_ + "_auto_stop"));
  if (!this->auto_stop_pref_.load(&this->auto_stop_))
    this->auto_stop_ = false;
  // stock firmware clears any timer icon left over from before the reboot
  this->queue_command_(CMD_SET_TIMER_ICON, {0});
  this->queue_command_(CMD_QUERY_STATUS, {});
  this->publish_timer_();
  this->mqtt_setup_();
}

void LevoitHumidifier::update() {
  // only poll when idle, a status query is also queued after every ACKed command
  if (!this->awaiting_ack_ && this->queue_.empty())
    this->queue_command_(CMD_QUERY_STATUS, {});
}

void LevoitHumidifier::loop() {
  this->read_uart_();

  uint32_t timeout = this->in_flight_.scan ? SCAN_ACK_TIMEOUT_MS : ACK_TIMEOUT_MS;
  if (this->awaiting_ack_ && millis() - this->sent_at_ > timeout) {
    if (this->in_flight_.scan) {
      this->awaiting_ack_ = false;
      this->scan_done_one_(false, this->in_flight_.cmd, 0, 0);
    } else if (this->retries_ < MAX_RETRIES) {
      this->retries_++;
      ESP_LOGD(TAG, "No ACK for 0x%04X, resending (%u)", this->in_flight_.cmd, this->retries_);
      this->write_frame_(CTRL_VERSION | CTRL_WANT_ACK, this->tx_seq_, this->in_flight_.cmd, this->in_flight_.data);
      this->sent_at_ = millis();
    } else {
      ESP_LOGW(TAG, "MCU did not ACK command 0x%04X, dropping it", this->in_flight_.cmd);
      this->awaiting_ack_ = false;
    }
  }

  if (this->timer_active_) {
    uint32_t now = millis();
    if ((int32_t) (now - this->timer_end_) >= 0) {
      ESP_LOGI(TAG, "Timer expired, turning %s", this->timer_power_ ? "on" : "off");
      this->set_power(this->timer_power_);
      this->cancel_timer_();
    } else if (now - this->timer_published_at_ >= 60000) {
      this->publish_timer_();
    }
  }

  this->update_wifi_led_(false);
  this->send_next_();
  this->mqtt_loop_();
}

void LevoitHumidifier::dump_config() {
  ESP_LOGCONFIG(TAG, "Levoit humidifier:");
  LOG_UPDATE_INTERVAL(this);
  ESP_LOGCONFIG(TAG, "  Wi-Fi icon: %s",
                this->wifi_icon_source_ == WifiIconSource::HOME_ASSISTANT ? "Home Assistant connected"
                                                                          : "Wi-Fi connected");
  ESP_LOGCONFIG(TAG, "  MQTT humidifier entity: %s", YESNO(this->mqtt_humidifier_));
}

// ---- timer / clean reminder -----------------------------------------------

void LevoitHumidifier::set_timer(uint16_t minutes) {
  if (minutes == 0) {
    this->cancel_timer_();
    return;
  }
  minutes = std::min(minutes, MAX_TIMER_MINUTES);
  this->timer_active_ = true;
  this->timer_power_ = !this->status_.power;
  this->timer_end_ = millis() + minutes * 60000UL;
  ESP_LOGI(TAG, "Timer: turn %s in %u min", this->timer_power_ ? "on" : "off", minutes);
  this->queue_command_(CMD_SET_TIMER_ICON, {1});
  this->publish_timer_();
}

void LevoitHumidifier::cancel_timer_() {
  if (this->timer_active_)
    this->queue_command_(CMD_SET_TIMER_ICON, {0});
  this->timer_active_ = false;
  this->publish_timer_();
}

void LevoitHumidifier::publish_timer_() {
  this->timer_published_at_ = millis();
  auto *n = this->numbers_[(size_t) NumberType::TIMER];
  if (n == nullptr)
    return;
  float remaining = 0;
  if (this->timer_active_)
    remaining = std::ceil((this->timer_end_ - millis()) / 60000.0f);  // round up, 0 only when done
  if (n->state != remaining)
    n->publish_state(remaining);
}

void LevoitHumidifier::reset_clean_reminder() {
  this->queue_command_(CMD_CLEAN_RESET, {0});
  this->queue_command_(CMD_QUERY_STATUS, {});
}

void LevoitHumidifier::send_raw(uint16_t cmd, const std::vector<uint8_t> &data) {
  ESP_LOGI(TAG, "Raw command 0x%04X [%s]", cmd, format_hex_pretty(data).c_str());
  this->queue_command_(cmd, data);
  this->queue_.back().raw = true;
}

static bool is_known_opcode(uint16_t cmd) {
  switch (cmd) {
    case CMD_SET_AUTO_MODE:
    case CMD_QUERY_STATUS:
    case CMD_STATUS_REPORT:
    case CMD_CLEAN_RESET:
    case 0x40C3:  // set_container_clean
    case CMD_SET_POWER:
    case CMD_SET_DISPLAY:
    case CMD_SET_WIFI_LED:
    case CMD_SET_MANUAL_LEVEL:
    case CMD_SET_TIMER_ICON:
    case CMD_SET_AUTO_STOP:
      return true;
    default:
      return false;
  }
}

void LevoitHumidifier::scan_opcodes(uint16_t first, uint16_t last, const std::vector<uint8_t> &data) {
  if (this->scan_left_ > 0) {
    ESP_LOGW(TAG, "Scan already running, %u opcodes left", (unsigned) this->scan_left_);
    return;
  }
  this->scan_hits_.clear();
  for (uint32_t cmd = first; cmd <= last; cmd++) {
    uint8_t hi = cmd >> 8;
    if (is_known_opcode(cmd) || hi == 0xD0 || hi == 0xD1)
      continue;  // known, or factory test / reboot / pairing
    this->queue_.push_back(Command{(uint16_t) cmd, data, false, true});
    this->scan_left_++;
  }
  ESP_LOGI(TAG, "Scan 0x%04X-0x%04X with data [%s]: %u opcodes, about %u s", first, last,
           format_hex_pretty(data).c_str(), (unsigned) this->scan_left_,
           (unsigned) (this->scan_left_ * SCAN_ACK_TIMEOUT_MS / 1000));
}

void LevoitHumidifier::scan_done_one_(bool acked, uint16_t cmd, uint8_t ctrl, uint8_t err) {
  if (acked) {
    ESP_LOGI(TAG, "Scan: 0x%04X ACKed (ctrl=0x%02X err=0x%02X)", cmd, ctrl, err);
    this->scan_hits_.push_back(cmd);
  } else {
    ESP_LOGV(TAG, "Scan: 0x%04X no answer", cmd);
  }
  if (this->scan_left_ > 0 && --this->scan_left_ % 32 == 0 && this->scan_left_ > 0)
    ESP_LOGI(TAG, "Scan: %u opcodes left", (unsigned) this->scan_left_);
  if (this->scan_left_ == 0) {
    std::string hits;
    for (uint16_t h : this->scan_hits_)
      hits += str_sprintf(" %04X", h);
    ESP_LOGI(TAG, "Scan done, %u ACKed:%s", (unsigned) this->scan_hits_.size(),
             hits.empty() ? " none" : hits.c_str());
  }
}

bool LevoitHumidifier::send_raw(const std::string &text) {
  uint16_t cmd;
  std::vector<uint8_t> data;
  if (text.size() > 5 && (text.compare(0, 5, "SCAN ") == 0 || text.compare(0, 5, "scan ") == 0)) {
    // "SCAN A000 A0FF 01 02": parse "A000 01 02" for the start + data, then the end opcode
    std::string rest = text.substr(5);
    uint16_t last;
    std::vector<uint8_t> unused;
    size_t space = rest.find(' ');
    if (space == std::string::npos || !parse_raw_command(rest.substr(0, space), cmd, unused)) {
      ESP_LOGW(TAG, "Scan '%s' not understood, expected e.g. 'SCAN A000 A0FF 01'", text.c_str());
      return false;
    }
    std::string tail = rest.substr(space + 1);
    if (!parse_raw_command(tail, last, data) || last < cmd) {
      ESP_LOGW(TAG, "Scan '%s' not understood, expected e.g. 'SCAN A000 A0FF 01'", text.c_str());
      return false;
    }
    this->scan_opcodes(cmd, last, data);
    return true;
  }
  if (!parse_raw_command(text, cmd, data)) {
    ESP_LOGW(TAG, "Raw command '%s' not understood, expected e.g. 'A003 01 01 D2 50 00'", text.c_str());
    return false;
  }
  this->send_raw(cmd, data);
  return true;
}

void LevoitHumidifier::handle_button_(uint16_t cmd, uint8_t value) {
  // The stock firmware starts Wi-Fi pairing (2/3), a factory reset (4) or factory test
  // mode (0xD104) on these long-presses. None of that exists here, so they are only logged.
  ESP_LOGD(TAG, "Long press on the device (0x%04X value %u) ignored", cmd, value);
}

// ---- Wi-Fi icon ------------------------------------------------------------

bool LevoitHumidifier::connected_() const {
  if (this->wifi_icon_source_ == WifiIconSource::HOME_ASSISTANT) {
#ifdef USE_API
    return api::global_api_server != nullptr && api::global_api_server->is_connected_with_state_subscription();
#else
    return false;
#endif
  }
#ifdef USE_WIFI
  return wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
#else
  return false;
#endif
}

void LevoitHumidifier::update_wifi_led_(bool force) {
  if (!this->have_status_)
    return;  // the MCU has not answered yet

  WifiLed want = WifiLed::OFF;
  if (this->connected_()) {
    want = WifiLed::ON;
  }
#ifdef USE_WIFI
  else if (wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_ap_active()) {
    want = WifiLed::BLINK;  // fallback hotspot, the ESPHome counterpart of pairing mode
  }
#endif
  if (want == this->wifi_led_ && !force)
    return;
  // stock firmware holds back non-connected LED states while the humidifier is off,
  // and sends the current state again when it is switched on
  if (want != WifiLed::ON && !this->status_.power)
    return;

  switch (want) {
    case WifiLed::ON:
      this->queue_command_(CMD_SET_WIFI_LED, wifi_led_on_data());
      break;
    case WifiLed::BLINK:
      this->queue_command_(CMD_SET_WIFI_LED, wifi_led_blink_data());
      break;
    default:
      this->queue_command_(CMD_SET_WIFI_LED, wifi_led_off_data());
      break;
  }
  ESP_LOGD(TAG, "Wi-Fi icon: %s", want == WifiLed::ON ? "on" : want == WifiLed::BLINK ? "blinking" : "off");
  this->wifi_led_ = want;
}

// ---- commands --------------------------------------------------------------

void LevoitHumidifier::set_power(bool on) { this->queue_command_(CMD_SET_POWER, {(uint8_t) on}); }

bool LevoitHumidifier::set_display(bool on) {
  if (this->have_status_ && !this->status_.power) {
    ESP_LOGW(TAG, "Display can only be changed while the humidifier is on");
    return false;
  }
  this->queue_command_(CMD_SET_DISPLAY, {(uint8_t) (on ? 100 : 0)});
  return true;
}

void LevoitHumidifier::set_auto_stop(bool on) {
  this->auto_stop_ = on;
  this->auto_stop_pref_.save(&on);  // stock firmware keeps this setting in flash
  this->queue_command_(CMD_SET_AUTO_STOP, {(uint8_t) on});
}

void LevoitHumidifier::send_auto_mode_() {
  this->queue_command_(CMD_SET_AUTO_MODE, auto_mode_data(this->target_humidity_));
}

bool LevoitHumidifier::set_auto_mode(bool on) {
  if (this->have_status_ && !this->status_.power)
    this->set_power(true);  // choosing a mode switches the humidifier on
  if (!this->have_status_ || on == this->status_.manual()) {
    if (on) {
      this->send_auto_mode_();
    } else {
      this->queue_command_(CMD_SET_MANUAL_LEVEL, manual_level_data(this->mist_level_));
    }
  }
  if (this->status_.display_config > 0 && this->wifi_led_ == WifiLed::ON)
    this->update_wifi_led_(true);
  return true;
}

bool LevoitHumidifier::set_target_humidity(uint8_t target) {
  if (target < MIN_TARGET_HUMIDITY || target > MAX_TARGET_HUMIDITY) {
    ESP_LOGW(TAG, "Target humidity %u%% is out of range", target);
    return false;
  }
  if (this->have_status_ && !this->status_.power) {
    ESP_LOGW(TAG, "Cannot set target humidity: the humidifier is off");
    return false;
  }
  if (this->have_status_ && this->status_.manual()) {
    ESP_LOGW(TAG, "Manual mode cannot set target humidity, switch to Auto first");
    return false;
  }
  this->target_humidity_ = target;
  this->send_auto_mode_();
  return true;
}

bool LevoitHumidifier::set_mist_level(uint8_t level) {
  if (level < 1 || level > MAX_MIST_LEVEL) {
    ESP_LOGW(TAG, "Mist level %u is out of range", level);
    return false;
  }
  if (this->have_status_ && !this->status_.power)
    this->set_power(true);  // like the app, a mist level switches the humidifier on
  this->mist_level_ = level;
  this->queue_command_(CMD_SET_MANUAL_LEVEL, manual_level_data(level));  // also selects manual mode
  return true;
}

bool LevoitHumidifier::set_mode(size_t index) {
  if (index == 0)
    return this->set_auto_mode(true);
  return this->set_mist_level(index == 2 ? 2 : 1);  // Low / High = manual at that level
}

bool LevoitHumidifier::set_auto_target(uint8_t target) {
  if (target < MIN_TARGET_HUMIDITY || target > MAX_TARGET_HUMIDITY) {
    ESP_LOGW(TAG, "Target humidity %u%% is out of range", target);
    return false;
  }
  if (this->have_status_ && !this->status_.power) {
    ESP_LOGW(TAG, "Cannot set target humidity: the humidifier is off");
    return false;
  }
  this->target_humidity_ = target;
  if (this->have_status_ && this->status_.manual())
    return this->set_auto_mode(true);  // sends auto mode with the new target
  this->send_auto_mode_();
  return true;
}

// ---- MQTT humidifier entity ------------------------------------------------

#ifdef USE_MQTT
std::string LevoitHumidifier::mqtt_topic_(const char *suffix) const {
  return mqtt::global_mqtt_client->get_topic_prefix() + "/levoit_humidifier/" + suffix;
}

std::string LevoitHumidifier::mqtt_discovery_topic_() const {
  return this->mqtt_discovery_prefix_ + "/humidifier/" + App.get_name() + "/humidifier/config";
}

void LevoitHumidifier::mqtt_setup_() {
  auto *client = mqtt::global_mqtt_client;
  if (client == nullptr || !this->mqtt_humidifier_)
    return;
  client->subscribe(this->mqtt_topic_("power/set"), [this](const std::string &, const std::string &payload) {
    this->set_power(payload == "ON");
    this->mqtt_dirty_ = true;
  });
  client->subscribe(this->mqtt_topic_("mode/set"), [this](const std::string &, const std::string &payload) {
    for (size_t i = 0; i < MODE_COUNT; i++) {
      if (payload == MODE_NAMES[i])
        this->set_mode(i);
    }
    this->mqtt_dirty_ = true;  // a refused change snaps the entity back
  });
  client->subscribe(this->mqtt_topic_("target/set"), [this](const std::string &, const std::string &payload) {
    auto target = parse_number<float>(payload);
    if (target.has_value())
      this->set_auto_target((uint8_t) *target);
    this->mqtt_dirty_ = true;
  });
}

void LevoitHumidifier::mqtt_loop_() {
  auto *client = mqtt::global_mqtt_client;
  if (client == nullptr || !client->is_connected()) {
    this->mqtt_announced_ = false;  // announce again after a reconnect
    return;
  }

  if (!this->mqtt_humidifier_) {
    // remove an entity announced by an earlier build: an empty retained config deletes it
    if (!this->mqtt_announced_ && client->publish(this->mqtt_discovery_topic_(), std::string(), 0, true))
      this->mqtt_announced_ = true;
    return;
  }

  if (!this->mqtt_announced_) {
    std::string node = App.get_name();
    bool ok = client->publish_json(
        this->mqtt_discovery_topic_(),
        [this, &node](JsonObject root) {
          root["name"] = nullptr;  // takes the device name
          root["unique_id"] = node + "_levoit_humidifier";
          root["device_class"] = "humidifier";
          root["icon"] = "mdi:air-humidifier";
          root["command_topic"] = this->mqtt_topic_("power/set");
          root["state_topic"] = this->mqtt_topic_("power");
          root["target_humidity_command_topic"] = this->mqtt_topic_("target/set");
          root["target_humidity_state_topic"] = this->mqtt_topic_("target");
          root["current_humidity_topic"] = this->mqtt_topic_("current");
          root["mode_command_topic"] = this->mqtt_topic_("mode/set");
          root["mode_state_topic"] = this->mqtt_topic_("mode");
          JsonArray modes = root["modes"].to<JsonArray>();
          for (const char *name : MODE_NAMES)
            modes.add(name);
          root["action_topic"] = this->mqtt_topic_("action");
          root["min_humidity"] = MIN_TARGET_HUMIDITY;
          root["max_humidity"] = MAX_TARGET_HUMIDITY;
          const auto &availability = mqtt::global_mqtt_client->get_availability();
          if (!availability.topic.empty()) {
            root["availability_topic"] = availability.topic;
            root["payload_available"] = availability.payload_available;
            root["payload_not_available"] = availability.payload_not_available;
          }
          // same MAC as the ESPHome device, so Home Assistant puts this entity on it
          JsonObject device = root["device"].to<JsonObject>();
          JsonArray connections = device["connections"].to<JsonArray>();
          JsonArray mac = connections.add<JsonArray>();
          mac.add("mac");
          mac.add(get_mac_address_pretty());
        },
        0, true);
    if (!ok)
      return;
    ESP_LOGI(TAG, "Announced the humidifier entity over MQTT");
    this->mqtt_announced_ = true;
    this->mqtt_dirty_ = true;
  }

  if (!this->mqtt_dirty_ || !this->have_status_)
    return;
  this->mqtt_dirty_ = false;
  const Status &s = this->status_;
  // std::string payloads: a bare literal would pick the (char *, length) overload
  client->publish(this->mqtt_topic_("power"), std::string(s.power ? "ON" : "OFF"), 0, true);
  client->publish(this->mqtt_topic_("mode"), std::string(MODE_NAMES[s.mode_index()]), 0, true);
  client->publish(this->mqtt_topic_("target"), to_string(s.target), 0, true);
  client->publish(this->mqtt_topic_("current"), to_string(s.humidity), 0, true);
  client->publish(this->mqtt_topic_("action"),
                  std::string(!s.power ? "off" : s.mist_active() ? "humidifying" : "idle"), 0, true);
}
#else
void LevoitHumidifier::mqtt_setup_() {}
void LevoitHumidifier::mqtt_loop_() {}
#endif

// ---- send ------------------------------------------------------------------

void LevoitHumidifier::queue_command_(uint16_t cmd, std::vector<uint8_t> data) {
  // a newer value for the same setting replaces the queued one
  auto same = [cmd](const Command &c) { return c.cmd == cmd; };
  this->queue_.erase(std::remove_if(this->queue_.begin(), this->queue_.end(), same), this->queue_.end());
  this->queue_.push_back(Command{cmd, std::move(data)});
}

void LevoitHumidifier::send_next_() {
  if (this->awaiting_ack_ || this->queue_.empty())
    return;
  this->in_flight_ = std::move(this->queue_.front());
  this->queue_.pop_front();
  this->tx_seq_++;
  this->write_frame_(CTRL_VERSION | CTRL_WANT_ACK, this->tx_seq_, this->in_flight_.cmd, this->in_flight_.data);
  this->awaiting_ack_ = true;
  this->sent_at_ = millis();
  this->retries_ = 0;
}

void LevoitHumidifier::write_frame_(uint8_t ctrl, uint8_t seq, uint16_t cmd, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> frame = encode_frame(ctrl, seq, cmd, data);
  ESP_LOGV(TAG, ">>> %s", format_hex_pretty(frame).c_str());
  this->write_array(frame.data(), frame.size());
}

// ---- receive ---------------------------------------------------------------

void LevoitHumidifier::read_uart_() {
  uint32_t now = millis();
  if (this->parser_.busy() && now - this->last_rx_ > RX_GAP_MS)
    this->parser_.reset();  // drop a partial frame

  uint8_t b;
  while (this->available() && this->read_byte(&b)) {
    this->last_rx_ = now;
    switch (this->parser_.feed(b)) {
      case FrameParser::Result::FRAME:
        ESP_LOGV(TAG, "<<< %s", format_hex_pretty(this->parser_.raw()).c_str());
        this->handle_frame_(this->parser_.frame());
        break;
      case FrameParser::Result::BAD_CHECKSUM:
        ESP_LOGW(TAG, "Dropping frame with bad checksum: %s", format_hex_pretty(this->parser_.raw()).c_str());
        break;
      default:
        break;
    }
  }
}

void LevoitHumidifier::handle_frame_(const Frame &frame) {
  if (!frame.supported()) {
    ESP_LOGW(TAG, "Unsupported frame ctrl=0x%02X len=%u", frame.ctrl, (unsigned) frame.payload.size());
    return;
  }
  uint16_t cmd = frame.cmd();

  if (frame.is_ack()) {
    if (!this->awaiting_ack_ || frame.seq != this->tx_seq_) {
      // an ACK after its timeout, e.g. a slow answer during a scan: still worth knowing
      if (cmd != CMD_QUERY_STATUS)
        ESP_LOGI(TAG, "Late ACK for 0x%04X (seq %u, ctrl=0x%02X)", cmd, frame.seq, frame.ctrl);
      return;
    }
    this->awaiting_ack_ = false;
    if (this->in_flight_.scan) {
      this->scan_done_one_(true, cmd, frame.ctrl, frame.err());
    } else if (this->in_flight_.raw) {
      // for probing: the full answer, also when it is an error
      ESP_LOGI(TAG, "Raw 0x%04X answered: ctrl=0x%02X%s err=0x%02X data=[%s]", cmd, frame.ctrl,
               frame.is_error() ? " (ERROR)" : "", frame.err(),
               format_hex_pretty(frame.data(), frame.data_len()).c_str());
    } else if (frame.is_error()) {
      ESP_LOGW(TAG, "MCU rejected command 0x%04X, error 0x%02X", cmd, frame.err());
    }
    if (cmd == CMD_QUERY_STATUS) {
      this->handle_status_(frame.data(), frame.data_len());
    } else {
      this->queue_command_(CMD_QUERY_STATUS, {});  // read back the new state
    }
    return;
  }

  if (cmd == CMD_STATUS_REPORT) {
    this->handle_status_(frame.data(), frame.data_len());
  } else if ((cmd >= CMD_BUTTON_FIRST && cmd <= CMD_BUTTON_LAST) || cmd == CMD_TEST_MODE) {
    if (frame.wants_ack())
      this->write_frame_(CTRL_VERSION | CTRL_ACK, frame.seq, cmd, {});
    this->handle_button_(cmd, frame.data_len() > 0 ? frame.data()[0] : 0);
    this->queue_command_(CMD_QUERY_STATUS, {});
  } else if (cmd == CMD_CLEAN_RESET) {
    ESP_LOGD(TAG, "Tank-cleaning reminder reset on the device");
    this->queue_command_(CMD_QUERY_STATUS, {});
  } else {
    ESP_LOGD(TAG, "Unhandled MCU frame 0x%04X (%u bytes)", cmd, (unsigned) frame.payload.size());
  }
}

void LevoitHumidifier::handle_status_(const uint8_t *data, size_t len) {
  Status s;
  if (!decode_status(data, len, s)) {
    ESP_LOGW(TAG, "Status block too short (%u bytes)", (unsigned) len);
    return;
  }
  bool first = !this->have_status_;
  Status prev = this->status_;
  this->have_status_ = true;
  this->status_ = s;
  this->target_humidity_ = s.target;
  if (s.mist_level >= 1)
    this->mist_level_ = s.mist_level;

  std::vector<uint8_t> raw(data, data + len);
  if (raw != this->last_raw_status_) {
    ESP_LOGD(TAG,
             "Status: power=%u mode=%s target=%u%% humidity=%u%% tank=%s water=%s auto_stopped=%u "
             "display=%u error=%u night_light=%u/%s/%02X%02X%02X raw=%s",
             s.power, MODE_NAMES[s.mode_index()], s.target, s.humidity, s.tank_lifted ? "lifted" : "in",
             s.water_lacks ? "low" : "ok", s.auto_stopped, s.display_config, s.exception, s.night_light_on,
             s.night_light_cycle ? "cycle" : "fixed", s.night_light_r, s.night_light_g, s.night_light_b,
             format_hex_pretty(data, len).c_str());
    this->last_raw_status_ = std::move(raw);
    this->mqtt_dirty_ = true;
  }

  if (first) {
    // the auto-stop setting lives on the ESP in the stock firmware: re-apply the saved one
    this->queue_command_(CMD_SET_AUTO_STOP, {(uint8_t) this->auto_stop_});
    auto *sw = this->switches_[(size_t) SwitchType::AUTO_STOP];
    if (sw != nullptr)
      sw->publish_state(this->auto_stop_);
  }

  // a timer whose job got done another way (button, app, automation) is finished
  if (!first && this->timer_active_ && s.power != prev.power && s.power == this->timer_power_) {
    ESP_LOGI(TAG, "Timer cancelled: the humidifier was switched %s already", s.power ? "on" : "off");
    this->cancel_timer_();
  }

  if (first || (s.power && !prev.power)) {
    this->update_wifi_led_(true);
  } else if (s.display_config > 0 && prev.display_config == 0 && this->wifi_led_ == WifiLed::ON) {
    this->update_wifi_led_(true);  // stock firmware re-lights the icon when the display comes back
  }

  this->publish_status_(s, first);
  if (s.exception != 0) {
    ESP_LOGW(TAG, "MCU reports exception flags 0x%02X", s.exception);
  }
}

void LevoitHumidifier::publish_status_(const Status &s, bool first) {
  auto pub_switch = [&](SwitchType t, bool v) {
    auto *e = this->switches_[(size_t) t];
    if (e != nullptr && (first || e->state != v))
      e->publish_state(v);
  };
  auto pub_number = [&](NumberType t, float v) {
    auto *e = this->numbers_[(size_t) t];
    if (e != nullptr && (first || e->state != v))
      e->publish_state(v);
  };
  auto pub_sensor = [&](SensorType t, float v) {
    auto *e = this->sensors_[(size_t) t];
    if (e != nullptr && (first || e->get_raw_state() != v))
      e->publish_state(v);
  };
  auto pub_binary = [&](BinarySensorType t, bool v) {
    auto *e = this->binary_sensors_[(size_t) t];
    if (e != nullptr)
      e->publish_state(v);  // binary sensors already dedupe
  };

  pub_switch(SwitchType::POWER, s.power);
  pub_switch(SwitchType::DISPLAY, s.display_on());

  pub_number(NumberType::TARGET_HUMIDITY, s.target);
  if (s.mist_level >= 1)
    pub_number(NumberType::MIST_LEVEL, s.mist_level);

  auto *mode = this->selects_[(size_t) SelectType::MODE];
  size_t index = s.mode_index();
  if (mode != nullptr && (first || mode->active_index() != index))
    mode->publish_state(index);

  pub_sensor(SensorType::HUMIDITY, s.humidity);
  pub_sensor(SensorType::TEMPERATURE, s.temperature);

  pub_binary(BinarySensorType::WATER_TANK_LIFTED, s.tank_lifted);
  pub_binary(BinarySensorType::LOW_WATER, s.low_water());
  pub_binary(BinarySensorType::MIST_ACTIVE, s.mist_active());
  pub_binary(BinarySensorType::AUTO_STOPPED, s.auto_stopped);
  pub_binary(BinarySensorType::HUMIDITY_HIGH, s.humidity_high());
  pub_binary(BinarySensorType::FAULT, s.exception != 0);
  if (s.has_clean_reminder)
    pub_binary(BinarySensorType::CLEAN_REMINDER, s.clean_reminder);

  auto *ver = this->text_sensors_[(size_t) TextSensorType::MCU_VERSION];
  if (ver != nullptr && first)
    ver->publish_state(str_sprintf("%u.%u.%02u", s.fw_major, s.fw_minor, s.fw_patch));

  if (s.has_night_light) {
    auto pub_text = [&](TextSensorType t, const std::string &v) {
      auto *e = this->text_sensors_[(size_t) t];
      if (e != nullptr && (first || !e->has_state() || e->state != v))
        e->publish_state(v);
    };
    pub_binary(BinarySensorType::NIGHT_LIGHT, s.night_light_on);
    pub_text(TextSensorType::NIGHT_LIGHT_MODE, s.night_light_cycle ? "Color Cycle" : "Fixed Color");
    std::string color = "Cycling";
    if (!s.night_light_cycle) {
      const char *name = night_light_color_name(s.night_light_r, s.night_light_g, s.night_light_b);
      color = str_sprintf("%s (#%02X%02X%02X)", name != nullptr ? name : "Unknown", s.night_light_r,
                          s.night_light_g, s.night_light_b);
    }
    pub_text(TextSensorType::NIGHT_LIGHT_COLOR, color);
  }
}

}  // namespace levoit_humidifier
}  // namespace esphome
