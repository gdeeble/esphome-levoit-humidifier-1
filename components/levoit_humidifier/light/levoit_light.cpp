#include "levoit_light.h"
#include "../levoit_humidifier.h"

#include <algorithm>
#include <cmath>

#include "esphome/core/log.h"

namespace esphome {
namespace levoit_humidifier {

static const char *const TAG = "levoit_humidifier.light";

static uint8_t to_byte(float v) { return (uint8_t) std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); }

static const uint32_t NO_EFFECT = 0;

light::LightTraits LevoitLight::get_traits() {
  light::LightTraits traits;
  traits.set_supported_color_modes({light::ColorMode::RGB});
  return traits;
}

void LevoitLight::setup_state(light::LightState *state) {
  this->state_ = state;
  this->parent_->add_night_light_listener([this](const protocol::Status &s) { this->on_status_(s); });
}

void LevoitLight::send_(bool on) {
  float r, g, b;
  this->state_->current_values_as_rgb(&r, &g, &b);  // includes brightness, gamma 1 by default
  this->parent_->set_night_light(on, this->cycle_, to_byte(r), to_byte(g), to_byte(b));
}

void LevoitLight::write_state(light::LightState *state) {
  // The restored state at boot must not overwrite what the humidifier shows: only send
  // once the light has been synced to the first status.
  if (!this->synced_)
    return;
  bool on = state->current_values.is_on();
  if (this->cycle_ && on) {
    const auto &v = state->remote_values;
    if (std::fabs(v.get_red() - this->cycle_red_) > 0.01f || std::fabs(v.get_green() - this->cycle_green_) > 0.01f ||
        std::fabs(v.get_blue() - this->cycle_blue_) > 0.01f) {
      // a colour was picked while cycling: that ends the cycle
      this->cycle_ = false;
      if (this->cycle_effect_ != nullptr && this->cycle_effect_->is_active())
        state->make_call().set_effect(NO_EFFECT).perform();
    }
  }
  this->send_(on);
}

void LevoitLight::start_cycle() {
  const auto &v = this->state_->remote_values;
  this->cycle_red_ = v.get_red();
  this->cycle_green_ = v.get_green();
  this->cycle_blue_ = v.get_blue();
  if (this->cycle_)
    return;
  this->cycle_ = true;
  if (this->synced_)
    this->send_(true);
}

void LevoitLight::stop_cycle() {
  if (!this->cycle_)
    return;
  this->cycle_ = false;
  // effect switched off with the light still on: back to the fixed colour. When the light
  // is being turned off, write_state sends the off.
  if (this->synced_ && this->state_->remote_values.is_on())
    this->send_(true);
}

void LevoitLight::on_status_(const protocol::Status &s) {
  if (this->state_ == nullptr)
    return;
  // RGB as brightness (largest channel) and a full-scale colour, so it round-trips through
  // current_values_as_rgb()
  uint8_t mx = std::max({s.night_light_r, s.night_light_g, s.night_light_b});
  float bright = mx == 0 ? 0.0f : mx / 255.0f;
  float r = mx == 0 ? 1.0f : s.night_light_r / (float) mx;
  float g = mx == 0 ? 1.0f : s.night_light_g / (float) mx;
  float b = mx == 0 ? 1.0f : s.night_light_b / (float) mx;

  if (this->synced_) {
    float cr, cg, cb;
    this->state_->current_values_as_rgb(&cr, &cg, &cb);
    bool on = this->state_->remote_values.is_on();
    if (protocol::night_light_matches(s, on, this->cycle_, to_byte(cr), to_byte(cg), to_byte(cb)))
      return;  // already showing this
  }
  ESP_LOGD(TAG, "Following the humidifier: %s", !s.night_light_on ? "off"
                                                : s.night_light_cycle ? "colour cycle"
                                                                      : "fixed colour");

  // set before the call, so the effect start/stop and write_state see the new state and
  // find nothing to send
  this->cycle_ = s.night_light_on && s.night_light_cycle;
  bool first = !this->synced_;
  auto call = this->state_->make_call();
  call.set_state(s.night_light_on);
  call.set_transition_length(0);
  if (!s.night_light_cycle) {
    call.set_rgb(r, g, b);
    if (bright > 0)
      call.set_brightness(bright);
  }
  if (s.night_light_on && this->cycle_effect_ != nullptr) {
    uint32_t index = this->cycle_ ? this->cycle_effect_->get_index() : NO_EFFECT;
    if (index != this->state_->get_current_effect_index())
      call.set_effect(index);
  }
  this->synced_ = true;
  if (first)
    call.set_save(false);
  call.perform();
}

LevoitLight *LevoitColorCycleEffect::light_() const {
  // this effect is only registered for the levoit_humidifier light (see light/__init__.py)
  return this->state_ == nullptr ? nullptr : static_cast<LevoitLight *>(this->state_->get_output());
}

void LevoitColorCycleEffect::init() {
  auto *l = this->light_();
  if (l != nullptr)
    l->set_cycle_effect(this);
}

void LevoitColorCycleEffect::start() {
  auto *l = this->light_();
  if (l != nullptr)
    l->start_cycle();
}

void LevoitColorCycleEffect::stop() {
  auto *l = this->light_();
  if (l != nullptr)
    l->stop_cycle();
}

}  // namespace levoit_humidifier
}  // namespace esphome
