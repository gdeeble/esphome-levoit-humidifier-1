#include "levoit_select.h"
#include "../levoit_humidifier.h"

namespace esphome {
namespace levoit_humidifier {

void LevoitSelect::control(size_t index) {
  if (this->type_ == SelectType::NIGHT_LIGHT_PRESET) {
    size_t first = this->first_preset_();
    if (this->has_off_ && index == 0) {
      this->parent_->set_night_light(false, false, 0, 0, 0);
    } else if (this->has_cycle_ && index == (size_t) this->has_off_) {
      this->parent_->set_night_light(true, true, 0, 0, 0);
    } else if (index >= first && index < first + this->presets_.size()) {
      const auto &c = this->presets_[index - first];
      this->parent_->set_night_light(true, false, c[0], c[1], c[2]);  // also turns it on
    } else {
      // "Custom" is not a colour: snap back to what the light shows
      if (this->active_index().has_value())
        this->publish_state(*this->active_index());
      return;
    }
    this->publish_state(index);  // the status read-back corrects it if the MCU disagrees
    return;
  }
  if (this->type_ != SelectType::MODE)
    return;
  if (this->parent_->set_mode(index)) {
    this->publish_state(index);
  } else if (this->active_index().has_value()) {
    this->publish_state(*this->active_index());  // refused: snap Home Assistant back
  }
}

void LevoitSelect::follow_night_light() {
  this->parent_->add_night_light_listener([this](const protocol::Status &s) { this->on_night_light_(s); });
}

void LevoitSelect::on_night_light_(const protocol::Status &s) {
  size_t first = this->first_preset_();
  size_t index = first + this->presets_.size();  // Custom
  if (this->has_off_ && !s.night_light_on) {
    index = 0;
  } else if (s.night_light_cycle) {
    // without a Color Cycle option, a cycling light shows as Custom
    if (this->has_cycle_)
      index = (size_t) this->has_off_;
  } else {
    // without an Off option the colour stays shown while the light is off
    for (size_t i = 0; i < this->presets_.size(); i++) {
      const auto &c = this->presets_[i];
      if (c[0] == s.night_light_r && c[1] == s.night_light_g && c[2] == s.night_light_b) {
        index = first + i;
        break;
      }
    }
  }
  if (!this->active_index().has_value() || *this->active_index() != index)
    this->publish_state(index);
}

}  // namespace levoit_humidifier
}  // namespace esphome
