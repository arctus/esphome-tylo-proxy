// Session statistics - not in upstream f-io/esphome-tylo.
//
// Proxy mode only. A session is the heater's own on-state (0x3400 bit 4) from
// on to off, whoever started or stopped it, in every bus mode. Sessions under
// a minute are ignored - that is what a start refused by the usage policy
// looks like (on, then off again within ~10 s).
//
// Kept across reboots: the totals and the last 64 sessions (start time from
// the heater clock - see sauna_clock.cpp - length and how it ended) are
// written to flash once per completed session and on a reset, so a busy day
// costs a handful of writes. The session in progress is not saved; a reboot
// mid-session loses it. The full list is on the /sauna page.

#include "sauna360.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace esphome {
namespace sauna360 {

static const char *const STAG = "sauna_stats";

namespace {
constexpr uint32_t STATS_VERSION = 2; // 2: + start times and end reasons
constexpr uint32_t STATS_PREF_HASH = 0x5A57A751; // arbitrary, fixed
constexpr uint32_t MIN_SESSION_S = 60;
} // namespace

void SAUNA360Component::stats_setup_() {
  this->stats_pref_ =
      global_preferences->make_preference<StatsBlob>(STATS_PREF_HASH, true);
  StatsBlob loaded{};
  if (this->stats_pref_.load(&loaded) && loaded.version == STATS_VERSION &&
      loaded.n <= STATS_WINDOW) {
    this->stats_ = loaded;
    ESP_LOGI(STAG, "Loaded: %u sessions, %.1f h, %u stopped by the limit",
             (unsigned) this->stats_.count, this->stats_.total_s / 3600.0f,
             (unsigned) this->stats_.limit_stops);
  } else {
    this->stats_ = StatsBlob{};
    this->stats_.version = STATS_VERSION;
    ESP_LOGI(STAG, "No stored statistics - starting from zero");
  }
}

void SAUNA360Component::stats_save_() {
  if (!this->stats_pref_.save(&this->stats_))
    ESP_LOGW(STAG, "Saving statistics failed");
  global_preferences->sync();
}

void SAUNA360Component::stats_loop_() {
  const bool on = this->heating_status_;
  const uint32_t now = millis();

  if (on && !this->stats_on_) {
    this->stats_on_ = true;
    this->stats_start_ms_ = now;
    this->stats_start_local_s_ = this->now_local_s();
    return;
  }
  if (on || !this->stats_on_)
    return;

  // Session over.
  this->stats_on_ = false;
  const uint32_t len_s = (now - this->stats_start_ms_) / 1000u;
  if (len_s < MIN_SESSION_S) {
    ESP_LOGD(STAG, "Ignored a %u s session", (unsigned) len_s);
    return;
  }

  StatsBlob &st = this->stats_;
  st.count++;
  st.total_s += len_s;
  st.last_s = len_s;
  if (this->pol_stopped_by_limit_)
    st.limit_stops++;
  const uint8_t reason = this->pol_stopped_by_limit_ ? END_LIMIT
                         : this->pol_forbidden_since_ms_ != 0 ? END_BLOCKED
                                                              : END_OFF;
  if (st.n == STATS_WINDOW) {
    memmove(&st.lengths[0], &st.lengths[1],
            (STATS_WINDOW - 1) * sizeof(st.lengths[0]));
    memmove(&st.starts[0], &st.starts[1],
            (STATS_WINDOW - 1) * sizeof(st.starts[0]));
    memmove(&st.reasons[0], &st.reasons[1],
            (STATS_WINDOW - 1) * sizeof(st.reasons[0]));
    st.n--;
  }
  st.lengths[st.n] = len_s;
  st.starts[st.n] = this->stats_start_local_s_;
  st.reasons[st.n] = reason;
  st.n++;
  this->stats_save_();
  ESP_LOGI(STAG, "Session %u: %u min%s (total %.1f h)", (unsigned) st.count,
           (unsigned) (len_s / 60),
           this->pol_stopped_by_limit_ ? ", stopped by the time limit" : "",
           st.total_s / 3600.0f);
}

void SAUNA360Component::reset_statistics() {
  ESP_LOGW(STAG, "Statistics cleared (%u sessions, %.1f h)",
           (unsigned) this->stats_.count, this->stats_.total_s / 3600.0f);
  this->stats_ = StatsBlob{};
  this->stats_.version = STATS_VERSION;
  this->stats_save_();
  // A session in progress keeps running and is counted when it ends; the
  // policy's own session timer is not touched, so a reset can never extend
  // a time limit.
}

float SAUNA360Component::get_stat_last_min() const {
  return this->stats_.count == 0 ? NAN : this->stats_.last_s / 60.0f;
}

float SAUNA360Component::get_stat_average_min() const {
  const uint32_t n = this->stats_.n;
  if (n == 0)
    return NAN;
  uint64_t sum = 0;
  for (uint32_t i = 0; i < n; i++)
    sum += this->stats_.lengths[i];
  return (float) sum / (float) n / 60.0f;
}

float SAUNA360Component::get_stat_median_min() const {
  const uint32_t n = this->stats_.n;
  if (n == 0)
    return NAN;
  uint32_t buf[STATS_WINDOW];
  memcpy(buf, this->stats_.lengths, n * sizeof(buf[0]));
  std::sort(buf, buf + n);
  const float mid = (n % 2 == 1)
                        ? (float) buf[n / 2]
                        : ((float) buf[n / 2 - 1] + (float) buf[n / 2]) / 2.0f;
  return mid / 60.0f;
}

float SAUNA360Component::get_current_session_min() const {
  if (!this->stats_on_)
    return 0.0f;
  return (millis() - this->stats_start_ms_) / 60000.0f;
}

} // namespace sauna360
} // namespace esphome
