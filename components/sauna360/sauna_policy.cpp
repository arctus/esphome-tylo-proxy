// Usage policy - not in upstream f-io/esphome-tylo.
//
// Runs only in proxy mode, and only acts while the bus mode is
// "Forward + Control" (commands are discarded in the other modes anyway).
//
//   Sauna Allowed      off: a heater start from the panel is refused at the
//                      proxy (passed on with the heater bit cleared, so the
//                      panel stops retrying it), and a heater that comes on
//                      anyway (external switch on the heater, or already on
//                      when switched off) is stopped after 10 s.
//   Temperature limit  the limit is written into the heater's own "max bath
//                      temperature" (0x4002 high bits), and a target above it
//                      is set back to the limit (0x6000). Switching the
//                      feature off restores the heater maximum once.
//   Session time limit counting starts when the room is within 10 C of the
//                      target, or after 60 min of warm-up, whichever is
//                      first; at the limit the heater is switched off.
//                      Starting again begins a new session.
//                      The heater's own bath time is held at 60 min + the
//                      limit: it is the only thing that stops a running
//                      heater if this device dies mid-session.
//   Opening hours      outside them a start is refused the same way, but a
//                      session that began inside them runs to its end - the
//                      sauna closes to new users, it does not throw anyone
//                      out. Needs the clock (sauna_clock.cpp); with no clock
//                      nothing is enforced. "Relax today" skips the window
//                      until midnight, and clears itself then.
//   Restart cooldown   after a stop by the time limit, starts are refused
//                      for N minutes.
//   Blink light        5 min before a limit stop the light is blinked twice.
//
// Heater on/off comes from 0x3400 bit 4 (heating_status_), confirmed on a
// Pure heater. The heater command 0x7000/1 is a TOGGLE, so a stop is only
// sent while the heater reports on, and at most every 10 s.

#include "sauna360.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace sauna360 {

static const char *const PTAG = "sauna_policy";

namespace {
constexpr uint32_t MIN_MS = 60000u;
constexpr uint32_t WARMUP_MAX_MS = 60u * MIN_MS;
constexpr int REACHED_MARGIN_C = 10;
constexpr uint32_t FORBIDDEN_GRACE_MS = 10000u;
constexpr uint32_t STOP_RETRY_MS = 10000u;
constexpr uint32_t BLINK_LEAD_MS = 5u * MIN_MS;
constexpr uint32_t BLINK_STEP_MS = 1500u;
constexpr int BLINK_STEPS = 4; // off/on/off/on = two blinks, ends as it began
constexpr uint32_t SETTINGS_RETRY_MS = 15000u;
constexpr uint32_t SETTINGS_BACKOFF_MS = 300000u;
constexpr int BATH_TIME_MAX_MIN = 575;
// How long after boot to wait for the heater's own 0x4002 before writing the
// rules' values to make it report (see policy_enforce_heater_settings_()).
constexpr uint32_t SETTINGS_BOOT_WAIT_MS = 60000u;
} // namespace

void SAUNA360Component::set_policy_temp_limit_enabled(bool v) {
  if (this->pol_temp_limit_enabled_ && !v)
    this->pol_temp_limit_restore_ = true;
  if (v)
    this->pol_temp_limit_restore_ = false;
  this->pol_temp_limit_enabled_ = v;
}

std::string SAUNA360Component::get_session_state() const {
  switch (this->pol_state_) {
  case PolState::WARMUP:
    return "Warming up";
  case PolState::COUNTING:
    return this->pol_limit_min_ > 0 ? "Counting" : "Hot (no time limit)";
  case PolState::STOPPING:
    return "Stopping";
  case PolState::BLOCKED:
    if (!this->pol_allowed_)
      return "Not allowed";
    if (this->pol_cooldown_active_)
      return "Cooldown";
    return "Closed now";
  case PolState::IDLE:
  default:
    return "Idle";
  }
}

float SAUNA360Component::get_session_minutes_left() const {
  if (this->pol_state_ != PolState::COUNTING || this->pol_limit_min_ == 0)
    return NAN;
  const int32_t left = (int32_t) (this->pol_limit_min_ * MIN_MS) -
                       (int32_t) (millis() - this->pol_count_start_ms_);
  return left > 0 ? left / (float) MIN_MS : 0.0f;
}

float SAUNA360Component::get_cooldown_minutes_left() const {
  if (!this->pol_cooldown_active_)
    return 0.0f;
  const int32_t left = (int32_t) (this->pol_cooldown_until_ms_ - millis());
  return left > 0 ? left / (float) MIN_MS : 0.0f;
}

// Called on the proxy task for every non-keep-alive panel reply in CONTROL
// mode. Returns true when it has answered the heater itself.
//
// A refused heater start is NOT swallowed: the panel resends a command until
// the heater confirms it (0x7000 back to 0 in its data frames), so a command
// that simply vanished was retried by the panel indefinitely - and the first
// retry after Sauna Allowed was switched back on started the heater. Instead
// the command goes on with the heater bit cleared (07 7000 00 when nothing
// else was asked), which the heater handles like any command - clearing its
// command register, which is what ends the panel's retries.
//
// Belt and braces, in case a panel keeps retrying anyway: a heater start that
// arrives within RETRY_WINDOW_MS of a refused one is treated as the same
// press and refused as well, whatever the rules say by then. A stale press can
// therefore never start the heater; a new press works once the retries stop.
bool SAUNA360Component::policy_rewrite_panel_command_(const uint8_t *body,
                                                      size_t blen) {
  constexpr uint32_t RETRY_WINDOW_MS = 3000u;
  if (blen < 10)
    return false;
  const uint16_t code = encode_uint16(body[2], body[3]);
  if (code != 0x7000)
    return false;
  const uint32_t data = encode_uint32(body[4], body[5], body[6], body[7]);
  if ((data & 0x1) == 0)
    return false; // not a heater toggle (light is bit 1)
  if (this->heating_status_)
    return false; // switching OFF is always allowed

  const uint32_t now = millis();
  const bool retry = this->pol_last_refused_ms_ != 0 &&
                     now - this->pol_last_refused_ms_ < RETRY_WINDOW_MS;
  if (!this->pol_block_start_.load() && !retry)
    return false;

  if (!retry) {
    this->pol_refused_count_ = 0;
    ESP_LOGW(PTAG, "Heater start from the panel refused (%s)",
             !this->pol_allowed_       ? "sauna not allowed"
             : this->pol_cooldown_active_ ? "restart cooldown"
             : this->pol_hours_block_     ? "outside the opening hours"
                                          : "repeat of a refused press");
  } else {
    this->pol_refused_count_++;
    // The panel is still retrying. If the heater answer does not stop it,
    // this line keeps coming - worth knowing when testing a new panel.
    if (this->pol_refused_count_ == 1 || this->pol_refused_count_ % 20 == 0)
      ESP_LOGW(PTAG, "Panel is repeating the refused heater start (%u so far)",
               (unsigned) this->pol_refused_count_);
  }
  this->pol_last_refused_ms_ = now;

  const uint32_t rest = data & ~0x1u;
  const uint8_t cmd[8] = {0x40,
                          0x07,
                          0x70,
                          0x00,
                          (uint8_t) (rest >> 24),
                          (uint8_t) (rest >> 16),
                          (uint8_t) (rest >> 8),
                          (uint8_t) rest};
  this->write_body_(this->heater_port_, cmd, sizeof(cmd));
  return true;
}

bool SAUNA360Component::request_heater_bath_time(int minutes) {
  if (this->pol_limit_min_ > 0) {
    ESP_LOGW(PTAG,
             "Bath time not changed: the session time limit (%d min) holds it "
             "at %d min. Set the limit to Off to choose it freely.",
             this->pol_limit_min_, 60 + this->pol_limit_min_);
    return false;
  }
  const int max_c =
      this->max_bath_temperature_received_hex_ != 0
          ? (int) std::lround(this->max_bath_temperature_received_hex_ / 18.0f)
          : this->pol_heater_max_c_;
  this->write_bath_settings_(minutes, max_c);
  return true;
}

void SAUNA360Component::write_bath_settings_(int minutes, int max_temp_c) {
  if (minutes < 0)
    minutes = 0;
  if (minutes > BATH_TIME_MAX_MIN)
    minutes = BATH_TIME_MAX_MIN;
  const uint32_t raw =
      static_cast<uint32_t>(this->encode_bath_time_raw_(minutes)) & 0x0FFF;
  const uint32_t hi = static_cast<uint32_t>(max_temp_c * 18) & 0x0FFF;
  // Keep upstream's bath-time display logic in step with what we wrote.
  this->last_bath_time_target_ = minutes;
  this->last_bath_time_set_ms_ = millis();
  this->create_send_data_(0x07, 0x4002, raw | (hi << 20));
  ESP_LOGI(PTAG, "Heater settings written: bath time %d min, max %d C",
           minutes, max_temp_c);
}

void SAUNA360Component::policy_enforce_heater_settings_() {
  const uint32_t now = millis();
  static int attempts_4002 = 0;
  static uint32_t last_6000_try = 0;

  // 0x4002: bath time (low 12 bits) + max bath temperature (high 12 bits).
  const bool have_4002 = this->bath_time_received_hex_ != 0 ||
                         this->max_bath_temperature_received_hex_ != 0;
  if (have_4002) {
    const int cur_min =
        this->decode_bath_time_minutes_(this->bath_time_received_hex_ & 0x0FFF);
    const int cur_max =
        (int) std::lround(this->max_bath_temperature_received_hex_ / 18.0f);

    int want_min = cur_min;
    if (this->pol_limit_min_ > 0) {
      want_min = 60 + this->pol_limit_min_;
      if (want_min > BATH_TIME_MAX_MIN)
        want_min = BATH_TIME_MAX_MIN;
    }
    int want_max = cur_max;
    if (this->pol_temp_limit_enabled_)
      want_max = this->pol_temp_limit_c_;
    else if (this->pol_temp_limit_restore_)
      want_max = this->pol_heater_max_c_;

    const bool differs = std::abs(cur_min - want_min) > 2 || cur_max != want_max;
    if (!differs) {
      attempts_4002 = 0;
      if (this->pol_temp_limit_restore_ && cur_max == this->pol_heater_max_c_)
        this->pol_temp_limit_restore_ = false;
    } else {
      const uint32_t wait =
          attempts_4002 >= 3 ? SETTINGS_BACKOFF_MS : SETTINGS_RETRY_MS;
      if (this->pol_last_4002_ms_ == 0 || now - this->pol_last_4002_ms_ >= wait) {
        ESP_LOGI(PTAG,
                 "Heater has bath time %d min / max %d C, want %d min / %d C",
                 cur_min, cur_max, want_min, want_max);
        this->write_bath_settings_(want_min, want_max);
        this->pol_last_4002_ms_ = now;
        attempts_4002++;
      }
    }
  } else if (now >= SETTINGS_BOOT_WAIT_MS) {
    // The heater sends 0x4002 only when it changes, so after a restart of the
    // ESP alone (an OTA update) it never arrives by itself, and the checks
    // above never run. Write the values the rules prescribe; the heater
    // answers with its own 0x4002, which fills have_4002 and the display.
    // Only values the rules would write anyway: with no session limit the
    // bath time must come from the last-known settings (sauna_cache.cpp), and
    // with no temperature limit so must the maximum - otherwise nothing is
    // written rather than inventing a value.
    int want_min = -1;
    if (this->pol_limit_min_ > 0) {
      want_min = 60 + this->pol_limit_min_;
      if (want_min > BATH_TIME_MAX_MIN)
        want_min = BATH_TIME_MAX_MIN;
    } else if (this->cache_bath_raw_.load() != 0) {
      want_min = this->decode_bath_time_minutes_(this->cache_bath_raw_.load());
    }
    int want_max = -1;
    if (this->pol_temp_limit_enabled_)
      want_max = this->pol_temp_limit_c_;
    else if (this->cache_max_hex_.load() != 0)
      want_max = (int) std::lround(this->cache_max_hex_.load() / 18.0f);

    const uint32_t wait =
        attempts_4002 >= 3 ? SETTINGS_BACKOFF_MS : SETTINGS_RETRY_MS;
    if (want_min > 0 && want_max > 0 &&
        (this->pol_last_4002_ms_ == 0 || now - this->pol_last_4002_ms_ >= wait)) {
      ESP_LOGI(PTAG,
               "Heater has not reported its bath time / max since boot - "
               "writing %d min / %d C so it does",
               want_min, want_max);
      this->write_bath_settings_(want_min, want_max);
      this->pol_last_4002_ms_ = now;
      attempts_4002++;
    }
  }

  // 0x6000: target temperature above the limit -> back to the limit.
  if (this->pol_temp_limit_enabled_ &&
      this->setpoint_temperature_received_hex_ != 0) {
    const int target =
        (int) std::lround(this->setpoint_temperature_received_hex_ / 9.0f);
    if (target > this->pol_temp_limit_c_ &&
        (last_6000_try == 0 || now - last_6000_try >= SETTINGS_RETRY_MS)) {
      ESP_LOGW(PTAG, "Target %d C is above the %d C limit - setting it back",
               target, this->pol_temp_limit_c_);
      this->set_bath_temperature_number((float) this->pol_temp_limit_c_);
      last_6000_try = now;
    }
  }
}

namespace {
constexpr uint32_t RELAX_PREF_HASH = 0x5A57A752;
std::string hhmm(int minutes) {
  char b[8];
  snprintf(b, sizeof(b), "%02d:%02d", (minutes / 60) % 24, minutes % 60);
  return b;
}
} // namespace

void SAUNA360Component::relax_setup_() {
  this->relax_pref_ = global_preferences->make_preference<uint32_t>(
      RELAX_PREF_HASH, true);
  uint32_t day = 0;
  if (this->relax_pref_.load(&day) && day != 0) {
    // Survives a reboot within the same day, so a restart in the middle of a
    // relaxed evening does not close the sauna again.
    this->pol_relax_day_ = day;
    this->pol_relax_today_ = true;
    ESP_LOGI(PTAG, "Relax for today restored (day %u)", (unsigned) day);
  }
}

void SAUNA360Component::set_relax_today(bool v) {
  if (v == this->pol_relax_today_)
    return;
  this->pol_relax_today_ = v;
  const uint32_t now_local = this->now_local_s();
  this->pol_relax_day_ = (v && now_local != 0) ? now_local / 86400u : 0;
  uint32_t stored = v ? (this->pol_relax_day_ != 0 ? this->pol_relax_day_ : 1)
                      : 0;
  this->relax_pref_.save(&stored);
  global_preferences->sync();
  if (v && now_local == 0) {
    ESP_LOGW(PTAG, "Relax for today ON, but the clock is not set - it will "
                   "stay on until it is switched off or the device reboots");
  } else {
    ESP_LOGI(PTAG, "Relax for today %s", v ? "ON until midnight" : "OFF");
  }
}

bool SAUNA360Component::within_opening_hours_(uint32_t now_local) const {
  if (this->pol_open_min_ == this->pol_close_min_)
    return true; // open == close means all day
  const int m = (int) ((now_local % 86400u) / 60u);
  if (this->pol_open_min_ < this->pol_close_min_)
    return m >= this->pol_open_min_ && m < this->pol_close_min_;
  return m >= this->pol_open_min_ || m < this->pol_close_min_; // over midnight
}

std::string SAUNA360Component::get_opening_hours_state() const {
  if (!this->pol_hours_enabled_)
    return "Not enforced";
  if (this->pol_relax_today_)
    return "Relaxed for today (" + hhmm(this->pol_open_min_) + " - " +
           hhmm(this->pol_close_min_) + " skipped)";
  const uint32_t now_local = this->now_local_s();
  if (now_local == 0)
    return "Clock not set - not enforced";
  return this->within_opening_hours_(now_local)
             ? "Open until " + hhmm(this->pol_close_min_)
             : "Closed until " + hhmm(this->pol_open_min_);
}

void SAUNA360Component::policy_stop_heater_(const char *why) {
  const uint32_t now = millis();
  if (!this->heating_status_)
    return;
  if (this->pol_last_stop_cmd_ms_ != 0 &&
      now - this->pol_last_stop_cmd_ms_ < STOP_RETRY_MS)
    return;
  this->pol_last_stop_cmd_ms_ = now;
  ESP_LOGW(PTAG, "Stopping the heater: %s", why);
  this->create_send_data_(0x07, 0x7000, 0x1); // heater toggle
}

void SAUNA360Component::policy_blink_step_() {
  if (this->pol_blink_step_ < 0)
    return;
  const uint32_t now = millis();
  if ((int32_t) (now - this->pol_blink_next_ms_) < 0)
    return;
  const bool target = (this->pol_blink_step_ % 2 == 0) ? !this->pol_blink_orig_
                                                       : this->pol_blink_orig_;
  this->set_light_relay(target);
  this->pol_blink_step_++;
  this->pol_blink_next_ms_ = now + BLINK_STEP_MS;
  if (this->pol_blink_step_ >= BLINK_STEPS)
    this->pol_blink_step_ = -1;
}

void SAUNA360Component::policy_loop_() {
  const uint32_t now = millis();

  if (this->bus_mode_.load() != static_cast<uint8_t>(BusMode::CONTROL)) {
    // Nothing can be sent in the other modes; do not refuse panel starts.
    this->pol_block_start_ = false;
    this->pol_state_ = PolState::IDLE;
    this->pol_blink_step_ = -1;
    return;
  }

  this->policy_blink_step_();
  if (this->pol_last_tick_ms_ != 0 && now - this->pol_last_tick_ms_ < 1000u)
    return;
  this->pol_last_tick_ms_ = now;

  if (this->pol_cooldown_active_ &&
      (int32_t) (this->pol_cooldown_until_ms_ - now) <= 0) {
    this->pol_cooldown_active_ = false;
    ESP_LOGI(PTAG, "Restart cooldown over");
  }
  // Opening hours, and the relax that skips them until midnight.
  const uint32_t now_local = this->now_local_s();
  const bool clock_ok = now_local != 0;
  if (this->pol_relax_today_ && clock_ok && this->pol_relax_day_ != 0 &&
      now_local / 86400u != this->pol_relax_day_) {
    this->pol_relax_today_ = false;
    uint32_t zero = 0;
    this->relax_pref_.save(&zero);
    global_preferences->sync();
    ESP_LOGI(PTAG, "Relax for today has expired at midnight");
  }
  bool hours_block = false;
  if (this->pol_hours_enabled_ && !this->pol_relax_today_) {
    if (!clock_ok) {
      if (!this->pol_clock_warned_) {
        ESP_LOGW(PTAG, "Opening hours are set but the clock is not - nothing "
                       "is enforced. Sync it on the /sauna page.");
        this->pol_clock_warned_ = true;
      }
    } else {
      this->pol_clock_warned_ = false;
      hours_block = !this->within_opening_hours_(now_local);
    }
  }
  this->pol_hours_block_ = hours_block;
  this->pol_block_start_ =
      !this->pol_allowed_ || this->pol_cooldown_active_ || hours_block;

  this->policy_enforce_heater_settings_();

  const bool on = this->heating_status_;

  if (!on) {
    if (this->pol_was_on_) {
      ESP_LOGI(PTAG, "Session ended after %u min%s",
               (unsigned) ((now - this->pol_session_start_ms_) / MIN_MS),
               this->pol_stopped_by_limit_ ? " (time limit)" : "");
      if (this->pol_stopped_by_limit_ && this->pol_cooldown_min_ > 0) {
        this->pol_cooldown_until_ms_ = now + this->pol_cooldown_min_ * MIN_MS;
        this->pol_cooldown_active_ = true;
        this->pol_block_start_ = true;
        ESP_LOGI(PTAG, "Restart cooldown: %d min", this->pol_cooldown_min_);
      }
      if (this->pol_blink_step_ >= 0) {
        this->pol_blink_step_ = -1;
        this->set_light_relay(this->pol_blink_orig_);
      }
    }
    this->pol_was_on_ = false;
    this->pol_stopped_by_limit_ = false;
    this->pol_forbidden_since_ms_ = 0;
    this->pol_state_ =
        this->pol_block_start_ ? PolState::BLOCKED : PolState::IDLE;
    return;
  }

  if (!this->pol_was_on_) {
    this->pol_was_on_ = true;
    this->pol_session_start_ms_ = now;
    this->pol_reached_ = false;
    this->pol_blinked_ = false;
    this->pol_stopped_by_limit_ = false;
    this->pol_session_in_hours_ = !hours_block;
    ESP_LOGI(PTAG, "Session started%s",
             hours_block ? " outside the opening hours" : "");
  }

  // Closing time does not end a session that started while the sauna was
  // open; it only stops one that began outside the hours.
  const bool must_stop = !this->pol_allowed_ || this->pol_cooldown_active_ ||
                         (hours_block && !this->pol_session_in_hours_);
  if (must_stop) {
    if (this->pol_forbidden_since_ms_ == 0)
      this->pol_forbidden_since_ms_ = now;
    this->pol_state_ = PolState::STOPPING;
    if (now - this->pol_forbidden_since_ms_ >= FORBIDDEN_GRACE_MS)
      this->policy_stop_heater_(!this->pol_allowed_ ? "sauna not allowed"
                                : this->pol_cooldown_active_ ? "restart cooldown"
                                                             : "outside the opening hours");
    return;
  }
  this->pol_forbidden_since_ms_ = 0;

  if (!this->pol_reached_) {
    const int cur = (int) (this->temperature_received_hex_ / 9);
    const int target = (int) (this->setpoint_temperature_received_hex_ / 9);
    const bool hot = target > 0 && cur >= target - REACHED_MARGIN_C;
    const bool warmup_over = now - this->pol_session_start_ms_ >= WARMUP_MAX_MS;
    if (hot || warmup_over) {
      this->pol_reached_ = true;
      this->pol_count_start_ms_ = now;
      if (hot)
        ESP_LOGI(PTAG, "Temperature reached (%d C, target %d C) - counting",
                 cur, target);
      else
        ESP_LOGI(PTAG, "60 min warm-up used up (%d C, target %d C) - counting",
                 cur, target);
    }
  }
  if (!this->pol_reached_) {
    this->pol_state_ = PolState::WARMUP;
    return;
  }

  this->pol_state_ = PolState::COUNTING;
  if (this->pol_limit_min_ == 0)
    return;

  const int32_t left = (int32_t) (this->pol_limit_min_ * MIN_MS) -
                       (int32_t) (now - this->pol_count_start_ms_);
  if (left <= 0) {
    this->pol_state_ = PolState::STOPPING;
    this->pol_stopped_by_limit_ = true;
    this->policy_stop_heater_("session time limit reached");
    return;
  }

  if (this->pol_blink_ && !this->pol_blinked_ &&
      left <= (int32_t) BLINK_LEAD_MS) {
    this->pol_blinked_ = true;
    this->pol_blink_orig_ = this->last_light_on_;
    this->pol_blink_step_ = 0;
    this->pol_blink_next_ms_ = now;
    ESP_LOGI(PTAG, "%d min left - blinking the light",
             (int) ((left + MIN_MS - 1) / MIN_MS));
  }
}

} // namespace sauna360
} // namespace esphome
