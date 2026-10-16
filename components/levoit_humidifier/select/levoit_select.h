#pragma once

#include <array>
#include <vector>

#include "esphome/components/select/select.h"
#include "esphome/core/helpers.h"
#include "../protocol.h"
#include "../types.h"

namespace esphome {
namespace levoit_humidifier {

class LevoitHumidifier;

class LevoitSelect : public select::Select, public Parented<LevoitHumidifier> {
 public:
  void set_type(SelectType type) { this->type_ = type; }
  // night_light_preset: options are [Off] [Color Cycle] one per preset (in order), Custom
  void set_extra_options(bool off, bool cycle) {
    this->has_off_ = off;
    this->has_cycle_ = cycle;
  }
  void add_preset(uint8_t r, uint8_t g, uint8_t b) { this->presets_.push_back({r, g, b}); }
  void follow_night_light();

 protected:
  void control(size_t index) override;
  void on_night_light_(const protocol::Status &s);

  SelectType type_{SelectType::MODE};
  std::vector<std::array<uint8_t, 3>> presets_;
  bool has_off_{true};
  bool has_cycle_{true};
  size_t first_preset_() const { return (size_t) this->has_off_ + (size_t) this->has_cycle_; }
};

}  // namespace levoit_humidifier
}  // namespace esphome
