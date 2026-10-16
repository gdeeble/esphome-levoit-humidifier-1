#pragma once

#include "esphome/components/light/light_effect.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/core/helpers.h"

#include "../protocol.h"

namespace esphome {
namespace levoit_humidifier {

class LevoitHumidifier;
class LevoitColorCycleEffect;

// The night light (US model) as an RGB light. Brightness scales the RGB value sent to the
// MCU, which has no brightness of its own. The colour cycle is the "Color Cycle" effect.
// What the light shows always comes from the MCU status: commands go out, the status
// that comes back is published.
class LevoitLight : public light::LightOutput, public Parented<LevoitHumidifier> {
 public:
  light::LightTraits get_traits() override;
  void setup_state(light::LightState *state) override;
  void write_state(light::LightState *state) override;

  void set_cycle_effect(LevoitColorCycleEffect *effect) { this->cycle_effect_ = effect; }
  // from the effect
  void start_cycle();
  void stop_cycle();

 protected:
  void on_status_(const protocol::Status &s);
  void send_(bool on);

  light::LightState *state_{nullptr};
  LevoitColorCycleEffect *cycle_effect_{nullptr};
  bool cycle_{false};
  bool synced_{false};
  // colour (without brightness) when the cycle started: picking another colour ends it
  float cycle_red_{1}, cycle_green_{1}, cycle_blue_{1};
};

class LevoitColorCycleEffect : public light::LightEffect {
 public:
  explicit LevoitColorCycleEffect(const char *name) : LightEffect(name) {}
  void init() override;
  void start() override;
  void stop() override;
  void apply() override {}

 protected:
  LevoitLight *light_() const;
};

}  // namespace levoit_humidifier
}  // namespace esphome
