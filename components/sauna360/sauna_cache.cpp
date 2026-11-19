// Last-known heater settings - not in upstream.
//
// The heater sends a value only when it changes: 0x6000 (current and target
// temperature), 0x4002 (bath time and max temperature), 0x4003 (PCB overheat
// limit). After a controller-only restart (an OTA update) nothing arrives
// until one of them changes, so the target, max temperature, PCB limit and
// the Temperature Level / Bath Time selects read unknown - measured on a
// Pure heater: in 40 s at idle it sent only its 0x4200 clock. A heater
// power-on resends everything.
//
// So the last values decoded from live frames are kept in flash and, a few
// seconds after boot, published to those entities for DISPLAY ONLY. They
// never reach a write: the *_received_hex_ fields that the policy and the
// write paths read stay empty until a live frame fills them, exactly as
// before. The current temperature is not cached - it is a measurement.
//
// Everything passes through the proxy, so a cached setting can only be stale
// if it was changed while the ESP was off or in Listen mode with the panel
// wired straight. The first live frame replaces it.
//
// Flash writes: the masked setting bits only (not the current temperature in
// 0x6000), saved from loop() when one changed - a handful per settings change.

#include "sauna360.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace sauna360 {

static const char *const KTAG = "sauna_cache";

namespace {
constexpr uint32_t CACHE_VERSION = 1;
constexpr uint32_t CACHE_PREF_HASH = 0x5A57A752; // arbitrary, fixed
constexpr uint32_t CACHE_PUBLISH_DELAY_MS = 3000;
} // namespace

void SAUNA360Component::cache_setup_() {
  this->cache_pref_ =
      global_preferences->make_preference<SettingsCache>(CACHE_PREF_HASH, true);
  SettingsCache loaded{};
  if (!this->cache_pref_.load(&loaded) || loaded.version != CACHE_VERSION) {
    ESP_LOGI(KTAG, "No stored heater settings yet");
    return;
  }
  this->cache_setpoint_hex_ = loaded.setpoint_hex;
  this->cache_bath_raw_ = loaded.bath_raw;
  this->cache_max_hex_ = loaded.max_hex;
  this->cache_pcb_hex_ = loaded.pcb_hex;
  this->cache_saved_ = loaded;
  ESP_LOGI(KTAG,
           "Last-known heater settings: target %d C, bath time %d min, max %d C,"
           " PCB limit %d C - shown until the heater reports",
           (int) std::lround(loaded.setpoint_hex / 9.0f),
           loaded.bath_raw ? this->decode_bath_time_minutes_(loaded.bath_raw) : -1,
           (int) std::lround(loaded.max_hex / 18.0f),
           (int) ((loaded.pcb_hex >> 11) & 0x7FF) / 18);
  this->set_timeout("sauna_cache_publish", CACHE_PUBLISH_DELAY_MS,
                    [this]() { this->cache_publish_(); });
}

// Publish whatever has not been reported live since boot.
void SAUNA360Component::cache_publish_() {
  if (this->setpoint_temperature_received_hex_ == 0 && this->cache_setpoint_hex_ != 0) {
    const int t = (int) (this->cache_setpoint_hex_.load() / 9.0);
    for (auto &l : listeners_) {
      if (l->current_target_temperature != t) {
        l->on_temperature_setting(t);
        l->current_target_temperature = t;
      }
    }
  }
  if (this->bath_time_received_hex_ == 0 && this->cache_bath_raw_ != 0) {
    const int m = this->decode_bath_time_minutes_(this->cache_bath_raw_.load());
    for (auto &l : listeners_)
      l->on_bath_time_setting(m);
  }
  if (this->max_bath_temperature_received_hex_ == 0 && this->cache_max_hex_ != 0) {
    const int t = (int) (this->cache_max_hex_.load() / 18);
    for (auto &l : listeners_)
      l->on_max_bath_temperature(t);
  }
  if (this->overheating_pcb_limit_received_hex_ == 0 && this->cache_pcb_hex_ != 0) {
    const int t = (int) ((this->cache_pcb_hex_.load() >> 11) & 0x7FF) / 18;
    for (auto &l : listeners_)
      l->on_overheating_pcb_limit(t);
  }
  ESP_LOGI(KTAG, "Published last-known heater settings");
}

// Called from handle_packet_() (proxy task) for every decoded heater frame.
void SAUNA360Component::cache_note_(uint16_t code, uint32_t data) {
  switch (code) {
  case 0x6000: {
    const uint32_t sp = (data >> 11) & 0x7FF;
    if (sp != 0)
      this->cache_setpoint_hex_ = sp;
    break;
  }
  case 0x4002: {
    const uint32_t bath = data & 0x0FFF;
    const uint32_t max = (data >> 20) & 0xFFF;
    if (bath != 0)
      this->cache_bath_raw_ = bath;
    if (max != 0)
      this->cache_max_hex_ = max;
    break;
  }
  case 0x4003: {
    const uint32_t pcb = data & 0x007FFFFF;
    if (pcb != 0)
      this->cache_pcb_hex_ = pcb;
    break;
  }
  default:
    break;
  }
}

// Main loop: save when a setting changed (never from the proxy task).
void SAUNA360Component::cache_loop_() {
  SettingsCache now{};
  now.version = CACHE_VERSION;
  now.setpoint_hex = this->cache_setpoint_hex_.load();
  now.bath_raw = this->cache_bath_raw_.load();
  now.max_hex = this->cache_max_hex_.load();
  now.pcb_hex = this->cache_pcb_hex_.load();
  if (now.setpoint_hex == this->cache_saved_.setpoint_hex &&
      now.bath_raw == this->cache_saved_.bath_raw &&
      now.max_hex == this->cache_saved_.max_hex &&
      now.pcb_hex == this->cache_saved_.pcb_hex)
    return;
  if (this->cache_pref_.save(&now)) {
    this->cache_saved_ = now;
    ESP_LOGD(KTAG, "Saved heater settings");
  } else {
    ESP_LOGW(KTAG, "Saving heater settings failed");
  }
}

} // namespace sauna360
} // namespace esphome
