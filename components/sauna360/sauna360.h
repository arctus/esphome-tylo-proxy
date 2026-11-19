#pragma once

#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_DATETIME
#include "esphome/components/datetime/datetime_entity.h"
#endif

#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace esphome {
namespace sauna360 {

class SAUNA360Listener {
public:
  virtual void on_temperature(uint16_t) {};
  virtual void on_temperature_setting(uint16_t) {};
  virtual void on_remaining_time(uint16_t) {};
  virtual void on_bath_time_setting(uint16_t) {};
  virtual void on_total_uptime(uint32_t) {};
  virtual void on_max_bath_temperature(uint16_t) {};
  virtual void on_overheating_pcb_limit(uint16_t) {};
  virtual void on_heater_status(bool) {};
  virtual void on_heater_state(const std::string &state) {};
  virtual void on_light_status(bool) {};
  virtual void on_ready_status(bool) {};
  virtual void on_setting_humidity_step(uint16_t) {};
  virtual void on_setting_humidity_percent(uint16_t) {};
  virtual void on_water_tank_level(uint16_t) {};
  virtual void on_session_uptime(uint32_t) {};
  virtual void on_session_uptime_text(const std::string &) {};
  virtual void on_coils_active(uint8_t) {};
  int current_target_temperature = -1;
};

class SAUNA360Component : public uart::UARTDevice, public Component {

#ifdef USE_NUMBER
  SUB_NUMBER(bath_time)
  SUB_NUMBER(bath_temperature)
  SUB_NUMBER(max_bath_temperature)
  SUB_NUMBER(overheating_pcb_limit)
  SUB_NUMBER(humidity_step)
  SUB_NUMBER(humidity_percent)
#endif
#ifdef USE_SWITCH
  SUB_SWITCH(light_relay)
  SUB_SWITCH(heater_relay)
#endif

public:
  enum class Mode : uint8_t { PURE = 0, COMBI = 1, ELITE = 2, COMBI_ELITE = 3 };
  void set_mode(Mode m) { mode_ = m; }
  void set_flow_control_pin(GPIOPin *pin) { flow_control_pin_ = pin; }

  // --- RS485 proxy (not in upstream) ---------------------------------------------
  // With a panel UART configured the ESP sits between heater (this
  // component's own UART, bus A) and panel (bus B) and forwards frames
  // instead of sharing one bus. Without it, upstream behaviour is unchanged.
  enum class BusMode : uint8_t { LISTEN = 0, FORWARD = 1, CONTROL = 2 };
  void set_panel_uart(uart::UARTComponent *panel) { panel_uart_ = panel; }
  void set_panel_reply_timeout_ms(uint32_t ms) { panel_reply_timeout_us_ = ms * 1000; }
  void set_bus_mode(uint8_t mode);
  uint8_t get_bus_mode() const { return bus_mode_.load(); }
  bool is_proxy() const { return panel_uart_ != nullptr; }
  float get_heater_frames_per_s() const { return heater_fps_; }
  float get_panel_frames_per_s() const { return panel_fps_; }
  uint32_t get_panel_replies_missed() const { return replies_missed_.load(); }
  uint32_t get_panel_replies_late() const { return replies_late_.load(); }
  uint32_t get_panel_commands() const { return panel_commands_.load(); }
  uint32_t get_commands_sent() const { return commands_sent_.load(); }
  uint32_t get_commands_dropped() const { return commands_dropped_.load(); }
  float get_reply_latency_ms() const { return reply_latency_us_.load() / 1000.0f; }
  bool is_panel_responding() const;
  bool is_heater_talking() const;

  // --- Usage policy (sauna_policy.cpp) ------------------
  // Runs in proxy mode only, and only acts in Forward + Control.
  void set_policy_allowed(bool v) { pol_allowed_ = v; }
  void set_policy_temp_limit_enabled(bool v);
  void set_policy_temp_limit(float c) { pol_temp_limit_c_ = (int) std::lround(c); }
  void set_policy_heater_max_temp(float c) { pol_heater_max_c_ = (int) std::lround(c); }
  void set_policy_session_limit_min(int m) { pol_limit_min_ = m < 0 ? 0 : m; }
  void set_policy_cooldown_min(int m) { pol_cooldown_min_ = m < 0 ? 0 : m; }
  void set_policy_blink(bool v) { pol_blink_ = v; }
  // Opening hours. Minutes since midnight, local time; a window that wraps
  // midnight (open > close) is allowed. Enforced only while the clock is set
  // (sauna_clock.cpp), and skipped for the rest of the day by "relax today".
  void set_policy_hours_enabled(bool v) { pol_hours_enabled_ = v; }
  void set_policy_open_minutes(int m) { pol_open_min_ = wrap_minutes_(m); }
  void set_policy_close_minutes(int m) { pol_close_min_ = wrap_minutes_(m); }
  void set_relax_today(bool v);
  bool get_relax_today() const { return pol_relax_today_; }
  std::string get_opening_hours_state() const;
  // Heater bath time chosen in the web UI. Refused while a session time limit
  // is set, because the policy then holds it at 60 min + the limit.
  bool request_heater_bath_time(int minutes);
  // Display getters for the Temperature Level / Bath Time selects. Until the
  // heater reports, they fall back to the last-known values kept in flash
  // (sauna_cache.cpp). Nothing that writes to the heater uses them.
  int get_heater_target_c() const {
    uint32_t hex = setpoint_temperature_received_hex_;
    if (hex == 0)
      hex = cache_setpoint_hex_.load();
    if (hex == 0)
      return -1;
    return (int) std::lround((hex & 0x7FF) / 9.0f);
  }
  int get_heater_bath_time_min() const {
    uint32_t raw = bath_time_received_hex_;
    if (raw == 0)
      raw = cache_bath_raw_.load();
    if (raw == 0)
      return -1;
    return decode_bath_time_minutes_(raw & 0x0FFF);
  }
  std::string get_session_state() const;
  float get_session_minutes_left() const;
  float get_cooldown_minutes_left() const;

  // --- Session statistics (sauna_stats.cpp) -----------------------
  // Proxy mode only. Tracks the heater itself (0x3400 bit 4) in every bus
  // mode; saved to flash once per completed session.
  uint32_t get_stat_sessions() const { return stats_.count; }
  uint32_t get_stat_limit_stops() const { return stats_.limit_stops; }
  float get_stat_total_hours() const { return stats_.total_s / 3600.0f; }
  float get_stat_last_min() const;
  float get_stat_average_min() const;
  float get_stat_median_min() const;
  float get_current_session_min() const;
  void reset_statistics();
  std::string get_recent_sessions_text() const;
  static constexpr size_t STATS_WINDOW_PUBLIC = 64;
  uint32_t stats_count_in_window() const { return stats_.n; }
  void stats_entry(uint32_t i, uint32_t &start, uint32_t &len, uint8_t &reason) const {
    start = stats_.starts[i];
    len = stats_.lengths[i];
    reason = stats_.reasons[i];
  }

  // --- Clock (sauna_clock.cpp) -------------------------------------
  // The heater keeps a running clock (0x4200, broadcast about once a minute).
  // It is set from a phone on the /sauna page and read back after every
  // reboot; until the heater reports a plausible date, a phone-synced ESP
  // clock is used instead (lost on reboot). Times are LOCAL time, in seconds
  // since 2000-01-01 00:00.
  bool set_clock_from_string(const std::string &value); // "YYYY-MM-DD HH:MM[:SS]"
  uint32_t now_local_s() const;                         // 0 = unknown
  std::string get_clock_text() const;
  static std::string format_local(uint32_t t);

  void setup() override;
  void loop() override;
  void dump_config() override;

  void initialize_defaults();
  void register_listener(SAUNA360Listener *lst) { listeners_.push_back(lst); }

  void set_bath_time_number(float value);
  void set_bath_time_default_value(float v) { bath_time_default_ = v; }

  void set_bath_temperature_number(float value);
  void set_bath_temperature_default_value(float v) {
    bath_temperature_default_ = v;
  }

  void set_max_bath_temperature_number(float value);
  void set_max_bath_temperature_default_value(float v) {
    max_bath_temperature_default_ = v;
  }

  void set_humidity_step_number(float value);
  void set_humidity_step_default_value(float v) { humidity_step_default_ = v; }

  void set_humidity_percent_number(float value);
  void set_humidity_percent_default_value(float v) {
    humidity_percent_default_ = v;
  }

  void set_light_relay(bool enable);
  void set_heater_relay(bool enable);

  void process_heater_status(uint32_t data);
  void process_bath_time(uint32_t data);
  void process_pcb_limit(uint32_t data);
  void process_temperature(uint32_t data);
  void process_heater_error(uint32_t data);
  void process_relay_bitmap(uint32_t data);
  void process_total_uptime(uint32_t data);
  void process_remaining_time(uint32_t data);
  void process_door_error(uint32_t data);
  void process_sensor_error(uint32_t data);
  void process_tank_level(uint32_t data);
  void process_humidity_control(uint32_t data);
  void process_datetime(uint32_t data);
  void process_time_limit(uint32_t data);

protected:
  Mode mode_ = Mode::PURE;
  esphome::HighFrequencyLoopRequester high_freq_;
  int min_ifg_us_ = 520;
  uint32_t LIGHT_MASK_{0x00020000};
  uint32_t COILS_MASK_{0x0001C000};
  int COILS_SHIFT_{14};

  struct TxFrame {
    uint8_t len;
    uint8_t data[24];
  };

  std::vector<SAUNA360Listener *> listeners_{};
  std::vector<uint8_t> rx_message_;
  QueueHandle_t tx_queue_{nullptr};
  GPIOPin *flow_control_pin_{nullptr};
  int heater_port_{0};

  // --- RS485 proxy state (all touched by the proxy task on core 1; the
  // atomics are also read by loop() and the YAML diagnostics) ---
  static constexpr size_t RAW_MAX = 48;
  struct RawAssembler {
    uint8_t buf[RAW_MAX];
    size_t len{0};
    bool in_frame{false};
  };
  uart::UARTComponent *panel_uart_{nullptr};
  int panel_port_{-1};
  int64_t panel_reply_timeout_us_{60000};
  std::atomic<uint8_t> bus_mode_{0};
  RawAssembler asm_heater_{};
  RawAssembler asm_panel_{};
  bool pending_{false};
  uint8_t pending_type_{0};
  uint8_t pending_body_[16]{};
  size_t pending_body_len_{0};
  int64_t pending_since_us_{0};
  std::atomic<uint32_t> heater_frames_{0};
  std::atomic<uint32_t> panel_frames_{0};
  std::atomic<uint32_t> replies_missed_{0};
  std::atomic<uint32_t> replies_late_{0};
  std::atomic<uint32_t> panel_commands_{0};
  std::atomic<uint32_t> commands_sent_{0};
  std::atomic<uint32_t> commands_dropped_{0};
  std::atomic<uint32_t> reply_latency_us_{0};
  std::atomic<uint32_t> last_heater_ms_{0};
  std::atomic<uint32_t> last_panel_ms_{0};
  uint32_t fps_heater_prev_{0};
  uint32_t fps_panel_prev_{0};
  uint32_t fps_last_ms_{0};
  float heater_fps_{0};
  float panel_fps_{0};

  void proxy_task_();

  // --- Usage policy state ---
  enum class PolState : uint8_t { IDLE, WARMUP, COUNTING, STOPPING, BLOCKED };
  bool pol_allowed_{true};
  bool pol_temp_limit_enabled_{false};
  bool pol_temp_limit_restore_{false};
  int pol_temp_limit_c_{90};
  int pol_heater_max_c_{110};
  int pol_limit_min_{0};
  int pol_cooldown_min_{0};
  bool pol_blink_{false};
  bool pol_hours_enabled_{false};
  int pol_open_min_{7 * 60};
  int pol_close_min_{22 * 60};
  bool pol_relax_today_{false};
  uint32_t pol_relax_day_{0}; // local day the relax was switched on
  bool pol_hours_block_{false};
  bool pol_session_in_hours_{true};
  bool pol_clock_warned_{false};
  ESPPreferenceObject relax_pref_;
  static int wrap_minutes_(int m) { m %= 1440; return m < 0 ? m + 1440 : m; }
  bool within_opening_hours_(uint32_t now_local) const;
  std::atomic<bool> pol_block_start_{false};
  uint32_t pol_last_refused_ms_{0}; // proxy task only
  uint32_t pol_refused_count_{0};   // proxy task only
  PolState pol_state_{PolState::IDLE};
  bool pol_was_on_{false};
  uint32_t pol_session_start_ms_{0};
  bool pol_reached_{false};
  uint32_t pol_count_start_ms_{0};
  uint32_t pol_forbidden_since_ms_{0};
  uint32_t pol_last_stop_cmd_ms_{0};
  bool pol_stopped_by_limit_{false};
  uint32_t pol_cooldown_until_ms_{0};
  bool pol_cooldown_active_{false};
  uint32_t pol_last_4002_ms_{0};
  uint32_t pol_last_6000_ms_{0};
  uint32_t pol_last_tick_ms_{0};
  bool pol_blinked_{false};
  int pol_blink_step_{-1};
  uint32_t pol_blink_next_ms_{0};
  bool pol_blink_orig_{false};

  void policy_loop_();
  void relax_setup_();

  // --- Session statistics state ---
  static constexpr size_t STATS_WINDOW = STATS_WINDOW_PUBLIC;
  struct StatsBlob {
    uint32_t version;
    uint32_t count;
    uint32_t total_s;
    uint32_t limit_stops;
    uint32_t last_s;
    uint32_t n;
    uint32_t lengths[STATS_WINDOW]; // oldest first
    uint32_t starts[STATS_WINDOW];  // local start time, 0 = clock unknown
    uint8_t reasons[STATS_WINDOW];  // StatsEnd
  };
  enum StatsEnd : uint8_t { END_OFF = 0, END_LIMIT = 1, END_BLOCKED = 2 };
  StatsBlob stats_{};
  std::atomic<uint32_t> heater_clock_raw_{0};
  std::atomic<uint32_t> heater_clock_rx_ms_{0};
  uint32_t phone_clock_base_s_{0};
  uint32_t phone_clock_base_ms_{0};
  uint32_t stats_start_local_s_{0};
  static uint32_t local_from_fields_(int y, int mo, int d, int h, int mi, int s);
  uint32_t heater_clock_local_s_() const;
  void clock_setup_();
  // --- Last-known heater settings (sauna_cache.cpp) ---------------
  struct SettingsCache {
    uint32_t version;
    uint32_t setpoint_hex; // 0x6000 bits 11-21
    uint32_t bath_raw;     // 0x4002 bits 0-11
    uint32_t max_hex;      // 0x4002 bits 20-31
    uint32_t pcb_hex;      // 0x4003 bits 0-22
  };
  std::atomic<uint32_t> cache_setpoint_hex_{0};
  std::atomic<uint32_t> cache_bath_raw_{0};
  std::atomic<uint32_t> cache_max_hex_{0};
  std::atomic<uint32_t> cache_pcb_hex_{0};
  SettingsCache cache_saved_{};
  ESPPreferenceObject cache_pref_;
  void cache_setup_();
  void cache_publish_();
  void cache_note_(uint16_t code, uint32_t data);
  void cache_loop_();
  ESPPreferenceObject stats_pref_;
  bool stats_on_{false};
  uint32_t stats_start_ms_{0};
  void stats_setup_();
  void stats_loop_();
  void stats_save_();
  void policy_stop_heater_(const char *why);
  void policy_enforce_heater_settings_();
  void policy_blink_step_();
  void write_bath_settings_(int minutes, int max_temp_c);
  bool policy_rewrite_panel_command_(const uint8_t *body, size_t blen);
  bool assemble_(RawAssembler &a, uint8_t b);
  void on_heater_frame_(const uint8_t *raw, size_t len);
  void on_panel_frame_(const uint8_t *raw, size_t len);
  void answer_heater_ourselves_();
  void write_raw_(int port, const uint8_t *raw, size_t len);
  void write_body_(int port, const uint8_t *body, size_t len);
  static size_t unstuff_(const uint8_t *raw, size_t len, uint8_t *out, size_t out_max);
  void decode_raw_(const uint8_t *raw, size_t len);

  uint8_t decode_escape_sequence(uint8_t data);
  bool validate_packet(std::vector<uint8_t> &packet);
  void handle_byte_(uint8_t byte);
  void handle_packet_(std::vector<uint8_t> packet);
  void handle_frame_(const std::vector<uint8_t> &frame);
  void send_data_();
  void create_send_data_(uint8_t type, uint16_t code, uint32_t data);

  uint32_t temperature_received_hex_{0};
  uint32_t setpoint_temperature_received_hex_{0};
  uint32_t bath_time_received_hex_{0};
  uint32_t max_bath_temperature_received_hex_{0};
  uint32_t overheating_pcb_limit_received_hex_{0};
  uint32_t humidity_received_hex_{0};
  uint32_t bath_type_priority_received_hex_{0};
  uint32_t last_humidity_step_set_ms_{0};
  int last_humidity_step_target_{-1};
  bool humidity_step_published_{false};
  bool humidity_percent_published_{false};

  static constexpr int HUM_STEP_BASE = 40;
  static constexpr int HUM_STEP_SCALE = 8;

  bool frame_flag_{false};
  bool state_changed_{false};
  bool heating_status_{false};
  bool defaults_initialized_{false};

  bool relays_known_{false};
  bool last_light_on_{false};
  bool last_heater_on_{false};

  uint32_t last_light_cmd_ms_{0};
  uint32_t last_heater_cmd_ms_{0};

  float bath_time_default_{NAN};
  float bath_temperature_default_{NAN};
  float max_bath_temperature_default_{NAN};
  float humidity_step_default_{NAN};
  float humidity_percent_default_{NAN};

  int last_bath_time_target_{-1};
  uint32_t last_bath_time_set_ms_{0};

  int decode_bath_time_minutes_(uint16_t raw12) const;
  int encode_bath_time_raw_(int target_minutes) const;

  bool session_active_{false};
  uint32_t session_start_ms_{0};
  uint32_t session_frozen_s_{0};
  uint32_t last_session_pub_ms_{0};

  void publish_session_();
};

} // namespace sauna360
} // namespace esphome
