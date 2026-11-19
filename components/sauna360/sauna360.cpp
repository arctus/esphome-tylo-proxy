#include "sauna360.h"
#include "esphome/components/number/number.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/components/uart/uart_component_esp_idf.h"
#include "esphome/core/application.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/core/time.h"

#include "driver/uart.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <algorithm>
#include <cstring>

namespace esphome {
namespace sauna360 {

static const char *TAG = "sauna360";

void SAUNA360Component::setup() {
  // Cross-core safe TX queue (producers: main task + RX task, consumer: RX task)
  this->tx_queue_ = xQueueCreate(8, sizeof(TxFrame));
  if (this->tx_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create TX queue");
    this->mark_failed();
    return;
  }

  // Optional DE/RE pin for transceivers without auto-direction (e.g. MAX485)
  if (this->flow_control_pin_ != nullptr) {
    this->flow_control_pin_->setup();
    this->flow_control_pin_->digital_write(false); // listen
  }

  this->min_ifg_us_ = (this->mode_ == Mode::PURE) ? 520 : 7000;

  const char *mode_str = nullptr;
  switch (this->mode_) {
  case Mode::PURE:
    mode_str = "PURE";
    break;
  case Mode::COMBI:
    mode_str = "COMBI";
    break;
  case Mode::ELITE:
    mode_str = "ELITE";
    break;
  case Mode::COMBI_ELITE:
    mode_str = "COMBI_ELITE";
    break;
  default:
    mode_str = "PURE";
    break;
  }

  ESP_LOGI(TAG, "IFG selected: %d us (%s)", this->min_ifg_us_, mode_str);

  // Model-specific bitmasks
  // PURE
  if (this->mode_ == Mode::PURE) {
    this->LIGHT_MASK_ = 0x00020000; // bit 17
    this->COILS_MASK_ = 0x0001C000; // bits 16..14
    this->COILS_SHIFT_ = 14;
    // COMBI, ELITE, COMBI_ELITE
  } else {
    this->LIGHT_MASK_ = 0x00000020; // bit 5
    this->COILS_MASK_ = 0x00000007; // bits 2..0
    this->COILS_SHIFT_ = 0;
  }
  ESP_LOGI(TAG, "Relay bitmasks: LIGHT=0x%08X COILS=0x%08X (shift=%d)",
           (unsigned)this->LIGHT_MASK_, (unsigned)this->COILS_MASK_,
           this->COILS_SHIFT_);

  // Initial UI state
  if (this->light_relay_switch_ != nullptr)
    this->light_relay_switch_->publish_state(false);
  if (this->heater_relay_switch_ != nullptr)
    this->heater_relay_switch_->publish_state(false);
  if (this->bath_temperature_number_ != nullptr)
    this->bath_temperature_number_->publish_state(0.0f);
  for (auto &l : listeners_)
    l->on_ready_status(true);

  // Keep UART hot
  this->high_freq_.start();

  // UART low-latency. Upstream hardcodes UART_NUM_0, which is only right while
  // this is the first hardware UART ESPHome hands out; read the real port.
  this->heater_port_ = static_cast<uart::IDFUARTComponent *>(this->parent_)
                           ->get_hw_serial_number();
  const uart_port_t PORT = static_cast<uart_port_t>(this->heater_port_);
  uart_set_rx_full_threshold(PORT, 1);
  uart_set_rx_timeout(PORT, 1);

  TaskHandle_t rx_task = nullptr;
#if CONFIG_FREERTOS_UNICORE
  const BaseType_t core_id = 0;
#else
  const BaseType_t core_id = 1;
#endif

  if (this->panel_uart_ != nullptr) {
    this->panel_port_ =
        static_cast<uart::IDFUARTComponent *>(this->panel_uart_)
            ->get_hw_serial_number();
    if (this->panel_port_ == this->heater_port_) {
      ESP_LOGE(TAG, "Panel UART and heater UART are the same port (%d)",
               this->panel_port_);
      this->mark_failed();
      return;
    }
    const uart_port_t PPORT = static_cast<uart_port_t>(this->panel_port_);
    uart_set_rx_full_threshold(PPORT, 1);
    uart_set_rx_timeout(PPORT, 1);
    ESP_LOGI(TAG,
             "PROXY mode: heater bus = UART%d, panel bus = UART%d, "
             "panel reply timeout %u ms",
             this->heater_port_, this->panel_port_,
             (unsigned)(this->panel_reply_timeout_us_ / 1000));
    xTaskCreatePinnedToCore(
        [](void *ctx) { static_cast<SAUNA360Component *>(ctx)->proxy_task_(); },
        "sauna_proxy", 6144, this, configMAX_PRIORITIES - 3, &rx_task,
        core_id);
    // No upstream boot defaults in proxy mode: they would re-program the
    // heater's bath time and temperatures on every reboot, and the usage
    // policy (sauna_policy.cpp) owns those settings instead.
    this->defaults_initialized_ = true;
    this->stats_setup_();
    this->cache_setup_();
    this->clock_setup_();
    this->relax_setup_();
    return;
  }

  // RX/flow-control task
  xTaskCreatePinnedToCore(
      [](void *ctx) {
        auto *self = static_cast<SAUNA360Component *>(ctx);
        const uart_port_t PORT = static_cast<uart_port_t>(self->heater_port_);

        uint8_t b = 0, w0 = 0, w1 = 0, w2 = 0, w3 = 0, w4 = 0, w5 = 0;

        for (;;) {
          int n = uart_read_bytes(PORT, &b, 1, portMAX_DELAY);
          if (n != 1)
            continue;

          // Detect panel EOF: 98 40 07 FD E3 9C
          w0 = w1;
          w1 = w2;
          w2 = w3;
          w3 = w4;
          w4 = w5;
          w5 = b;
          const bool panel_eof = (w0 == 0x98 && w1 == 0x40 && w2 == 0x07 &&
                                  w3 == 0xFD && w4 == 0xE3 && w5 == 0x9C);

          if (panel_eof && uxQueueMessagesWaiting(self->tx_queue_) > 0) {
            esp_rom_delay_us(self->min_ifg_us_);
            size_t rx_avail = 0;
            (void)uart_get_buffered_data_len(PORT, &rx_avail);
            if (rx_avail == 0)
              self->send_data_();
          }

          self->handle_byte_(b);
        }
      },
      "sauna_rx_fast", 4096, this, configMAX_PRIORITIES - 3, &rx_task, core_id);

  if (!this->defaults_initialized_) {
    this->initialize_defaults();
    this->defaults_initialized_ = true;
  }
}

void SAUNA360Component::loop() {
  const uint32_t now = millis();
  if (this->panel_uart_ != nullptr && (now - this->fps_last_ms_) >= 2000u) {
    const float dt = (now - this->fps_last_ms_) / 1000.0f;
    const uint32_t h = this->heater_frames_.load();
    const uint32_t p = this->panel_frames_.load();
    if (this->fps_last_ms_ != 0) {
      this->heater_fps_ = (h - this->fps_heater_prev_) / dt;
      this->panel_fps_ = (p - this->fps_panel_prev_) / dt;
    }
    this->fps_heater_prev_ = h;
    this->fps_panel_prev_ = p;
    this->fps_last_ms_ = now;
  }
  if (this->panel_uart_ != nullptr) {
    this->stats_loop_(); // before the policy: it reads pol_stopped_by_limit_
    this->policy_loop_();
    this->cache_loop_();
  }
  if ((now - this->last_session_pub_ms_) >= 1000u) {
    if (this->session_active_) {
      this->publish_session_();
    }
    this->last_session_pub_ms_ = now;
  }
}

void SAUNA360Component::handle_byte_(uint8_t c) {
  if (c == 0x98) {
    this->frame_flag_ = true;
  }
  if (c == 0x98 && this->frame_flag_ == true) {
    this->rx_message_.clear();
  }
  if (c == 0x9C) {
    this->rx_message_.push_back(c);
    std::vector<uint8_t> frame(this->rx_message_.begin(),
                               this->rx_message_.end());
    size_t len = frame.size();
    if (len > 6) {
      this->handle_frame_(frame);
    }
    this->rx_message_.clear();
    this->frame_flag_ = false;
    return;
  }
  if (this->frame_flag_ == true) {
    this->rx_message_.push_back(c);
  }
}

void SAUNA360Component::handle_frame_(const std::vector<uint8_t> &frame) {
  std::vector<uint8_t> packet;
  bool is_escaped = false;

  for (size_t i = 1; i < frame.size() - 1; ++i) {
    uint8_t data = frame[i];

    if (data == 0x91) {
      is_escaped = true;
      continue;
    }

    if (is_escaped) {
      data = decode_escape_sequence(data);
      is_escaped = false;
    }

    packet.push_back(data);
  }

  if (!validate_packet(packet)) {
    ESP_LOGI(TAG, "Invalid packet size or CRC error");
    return;
  }

  this->handle_packet_(packet);
}

uint8_t SAUNA360Component::decode_escape_sequence(uint8_t data) {
  switch (data) {
  case 0x63:
    return 0x9C; // EOF
  case 0x67:
    return 0x98; // SOF
  case 0x6E:
    return 0x91; // ESC
  default:
    ESP_LOGI(TAG, "Unknown escape sequence: %02X", data);
    return 0x91; // Return original escape character for unknown sequences
  }
}

bool SAUNA360Component::validate_packet(std::vector<uint8_t> &packet) {
  if (packet.size() < 2) {
    return false;
  }

  uint16_t crc = (packet.end()[-2] << 8) | packet.end()[-1];
  packet.resize(packet.size() - 2);

  uint16_t calculated_crc =
      crc16be(packet.data(), packet.size(), 0xFFFF, 0x90D9, false, false);
  if (crc != calculated_crc) {
    std::string packet_str;
    for (size_t i = 0; i < packet.size(); ++i) {
      char buffer[4];
      snprintf(buffer, sizeof(buffer), " %02X", packet[i]);
      packet_str += buffer;
    }
    ESP_LOGI(TAG, "CRC ERROR: Expected %04X, got %04X. Full packet:[%s]", crc,
             calculated_crc, packet_str.c_str());
    return false;
  }
  return true;
}

void SAUNA360Component::handle_packet_(std::vector<uint8_t> packet) {
  if (packet.size() < 8) {
    ESP_LOGW(TAG, "Packet too short: %u", (unsigned)packet.size());
    return;
  }
  uint8_t packet_type = packet[1];
  uint16_t code = encode_uint16(packet[2], packet[3]);
  uint32_t data = encode_uint32(packet[4], packet[5], packet[6], packet[7]);

  const bool from_panel = (packet_type == 0x07) || (packet_type == 0x09);

  // ESPHome 2026.11 removed the hex-dump helper upstream used here;
  // format locally so the component builds on old and new ESPHome alike.
  char hex[3 * 16 + 1] = {0};
  {
    size_t pos = 0;
    for (size_t i = 0; i < packet.size() && i < 16; i++)
      pos += snprintf(hex + pos, sizeof(hex) - pos, i ? ".%02X" : "%02X", packet[i]);
  }

  if (from_panel) {
    ESP_LOGD(TAG, "%s [ HEATER <-- PANEL ] CODE %04X DATA 0x%08X",
             hex, code, (unsigned)data);
    packet.clear();
    return;
  }

  ESP_LOGD(TAG, "%s [ HEATER --> PANEL ] CODE %04X DATA 0x%08X",
           hex, code, (unsigned)data);

  switch (code) {
  case 0x3400:
    this->process_heater_status(data);
    break;
  case 0x3801: /* combi sensors */
    break;
  case 0x4002:
    this->process_bath_time(data);
    break;
  case 0x4003:
    this->process_pcb_limit(data);
    break;
  case 0x4200:
    this->process_datetime(data);
    break;
  case 0x5200: /* relay */
    break;
  case 0x5201: /* relay */
    break;
  case 0x5202: /* relay */
    break;
  case 0x6000:
    this->process_temperature(data);
    break;
  case 0x6001:
    this->process_humidity_control(data);
    break;
  case 0x7000:
    this->process_heater_error(data);
    break;
  case 0x7180:
    this->process_relay_bitmap(data);
    break;
  case 0x7280:
    this->process_tank_level(data);
    break;
  case 0x9000:
    this->process_time_limit(data);
    break;
  case 0x9400:
    this->process_total_uptime(data);
    break;
  case 0x9401:
    this->process_remaining_time(data);
    break;
  case 0xB000:
    this->process_door_error(data);
    break;
  default:
    if ((code & 0xFF00) == 0xB600) {
      this->process_sensor_error(data);
    } else {
      ESP_LOGD(TAG, "Unhandled packet code: %04X", code);
    }
    break;
  }
  // Remember the heater's settings for the next boot (sauna_cache.cpp).
  if (this->panel_uart_ != nullptr)
    this->cache_note_(code, data);
}

void SAUNA360Component::process_heater_status(uint32_t data) {
  this->heating_status_ = ((data >> 4) & 1); // "enabled"
  this->state_changed_ = true;
  if (this->heating_status_) {
    for (auto &l : listeners_)
      l->on_ready_status(false);
  }
}

// Raw 12-bit -> minutes (bucketed)
int SAUNA360Component::decode_bath_time_minutes_(uint16_t raw12) const {
  int raw = static_cast<int>(raw12);
  if (raw < 0)
    raw = 0;
  if (raw > 4095)
    raw = 4095;

  if (raw < 64)
    return raw;

  int k = (raw / 64) - 1; // 0..7 for raw >= 64
  if (k < 0)
    k = 0;
  if (k > 7)
    k = 7;
  int offset = 4 * (k + 1); // 4,8,12,16,20,24,28,32

  int minutes = raw - offset;
  if (minutes < 0)
    minutes = 0;
  if (minutes > 575)
    minutes = 575;
  return minutes;
}

// minutes -> Raw 12-bit (inverse of above)
int SAUNA360Component::encode_bath_time_raw_(int target_minutes) const {
  int t = target_minutes;
  if (t < 0)
    t = 0;
  if (t > 575)
    t = 575;

  if (t <= 63)
    return t;

  int k = (t / 64) - 1; // 0..7 for t >= 64
  if (k < 0)
    k = 0;
  if (k > 7)
    k = 7;
  int offset = 4 * (k + 1);

  int raw = t + offset;
  // Raw field is 12-bit in low bits of 0x4002
  if (raw < 0)
    raw = 0;
  if (raw > 4095)
    raw = 4095;
  return raw;
}

void SAUNA360Component::process_bath_time(uint32_t data) {
  const uint16_t raw = static_cast<uint16_t>(data & 0x0FFF);
  this->bath_time_received_hex_ = raw;

  const int decoded = this->decode_bath_time_minutes_(raw);

  // Snap logic
  constexpr uint32_t SNAP_WINDOW_MS = 1000; // 1s
  constexpr int SNAP_TOL_MIN = 8;           // ±8 min

  int to_publish = decoded;

  const bool have_intent = (this->last_bath_time_target_ >= 0);
  const uint32_t dt = millis() - this->last_bath_time_set_ms_;
  const bool within_window = have_intent && (dt <= SNAP_WINDOW_MS);
  const bool close_to_target =
      have_intent &&
      (std::abs(decoded - this->last_bath_time_target_) <= SNAP_TOL_MIN);

  const bool heater_on = this->heating_status_ || this->last_heater_on_;

  if (!heater_on && !(within_window || close_to_target)) {
    if (have_intent) {
      to_publish = this->last_bath_time_target_;
    } else {
    }
  } else if (within_window || close_to_target) {
    to_publish = this->last_bath_time_target_;
  }

  // Publish
  for (auto &listener : listeners_)
    listener->on_bath_time_setting(to_publish);

  if (this->bath_time_number_ != nullptr) {
    if (this->bath_time_number_->state != static_cast<float>(to_publish)) {
      this->bath_time_number_->publish_state(static_cast<float>(to_publish));
    }
  }

  // High bits: max bath temperature (12 Bit)
  const uint32_t hi = (data >> 20) & 0xFFF;
  const int max_bath_temperature = static_cast<int>(hi / 18);
  this->max_bath_temperature_received_hex_ = hi;

  for (auto &listener : listeners_)
    listener->on_max_bath_temperature(max_bath_temperature);

  if (this->max_bath_temperature_number_ != nullptr)
    this->max_bath_temperature_number_->publish_state(
        static_cast<float>(max_bath_temperature));

  ESP_LOGI(TAG, "Bath time RX: raw=%u -> decoded=%d, published=%d%s%s",
           static_cast<unsigned>(raw), decoded, to_publish,
           (to_publish != decoded) ? " (snap)" : "",
           (!heater_on && !(within_window || close_to_target))
               ? " (held while OFF)"
               : "");
}

void SAUNA360Component::process_pcb_limit(uint32_t data) {
  int overheating_pcb_limit = ((data >> 11) & 0x00007FF) / 18;
  this->overheating_pcb_limit_received_hex_ = ((data) & 0x007FFFFF);
  for (auto &listener : listeners_) {
    listener->on_overheating_pcb_limit(overheating_pcb_limit);
  }
}

void SAUNA360Component::process_datetime(uint32_t data) {
  // Keep the heater clock for session timestamps (sauna_clock.cpp).
  this->heater_clock_raw_ = data;
  this->heater_clock_rx_ms_ = millis();
  int minute = (data & 0x3F);
  int hour = ((data >> 6) & 0x1F);
  int day = ((data >> 12) & 0x1F);
  int month = ((data >> 17) & 0x0F);
  int year = ((data >> 21) & 0x1F) + 2000;

  ESP_LOGI(TAG, "Datetime from heater: %04d-%02d-%02d %02d:%02d", year, month,
           day, hour, minute);
}

void SAUNA360Component::process_temperature(uint32_t data) {
  int actual_temp = (data & 0x00007FF) / 9.0;
  this->temperature_received_hex_ = (data & 0x00007FF);

  for (auto &listener : listeners_) {
    listener->on_temperature(actual_temp);
  }

  int setpoint_temp = ((data >> 11) & 0x00007FF) / 9.0;
  this->setpoint_temperature_received_hex_ = ((data >> 11) & 0x00007FF);

  if (this->bath_temperature_number_ != nullptr) {
    if (this->bath_temperature_number_->state != setpoint_temp) {
      this->bath_temperature_number_->publish_state(setpoint_temp);
    }
  }

  for (auto &listener : listeners_) {
    if (listener->current_target_temperature != setpoint_temp) {
      listener->on_temperature_setting(setpoint_temp);
      listener->current_target_temperature = setpoint_temp;
    }
  }

  ESP_LOGI(TAG, "Temperature = %d°C, Target Temperature = %d°C", actual_temp,
           setpoint_temp);
}

void SAUNA360Component::process_humidity_control(uint32_t data) {
  this->humidity_received_hex_ = data;
  if ((data & 0xF0000000) == 0) {
    // Step mode: keep priority bits
    this->bath_type_priority_received_hex_ = (data & 0x0000F000);
  } else {
    // Percent mode: keep high nibble + priority + flags
    this->bath_type_priority_received_hex_ = (data & 0xF000C000);
  }

  if ((data & 0xF0000000) == 0) {
    // STEP MODE (0..10) ---
    int raw = static_cast<int>((data >> 4) & 0xFF);
    int step = (raw - HUM_STEP_BASE) / HUM_STEP_SCALE;
    if (step < 0)
      step = 0;
    if (step > 10)
      step = 10;

    for (auto &listener : listeners_)
      listener->on_setting_humidity_step(static_cast<uint16_t>(step));

#ifdef USE_NUMBER
    if (this->humidity_step_number_ != nullptr) {
      const float fstep = static_cast<float>(step);
      if (!this->humidity_step_published_ ||
          this->humidity_step_number_->state != fstep) {
        this->humidity_step_number_->publish_state(fstep);
        this->humidity_step_published_ = true;
      }
    }
#endif
    ESP_LOGI(TAG, "Humidity step: %d", step);
  } else {
    // % MODE (0..63 encoded)
    int target_pct = static_cast<int>((data >> 7) & 0x3F);
    int current_pct = static_cast<int>(data & 0x7F);
    if (target_pct < 0)
      target_pct = 0;
    if (target_pct > 100)
      target_pct = 100;

    for (auto &listener : listeners_)
      listener->on_setting_humidity_percent(static_cast<uint16_t>(target_pct));

#ifdef USE_NUMBER
    if (this->humidity_percent_number_ != nullptr) {
      const float fpct = static_cast<float>(target_pct);
      if (!this->humidity_percent_published_ ||
          this->humidity_percent_number_->state != fpct) {
        this->humidity_percent_number_->publish_state(fpct);
        this->humidity_percent_published_ = true;
      }
    }
#endif
    ESP_LOGI(TAG, "Humidity: target %d%%, current %d%%", target_pct,
             current_pct);
  }

  int pr = static_cast<int>((data >> 14) & 0x3);
  const char *priority = (pr == 0)   ? "Automatic"
                         : (pr == 1) ? "Temperature"
                         : (pr == 2) ? "Humidity"
                                     : "Unknown";
  ESP_LOGI(TAG, "Priority: %s", priority);
}

void SAUNA360Component::process_heater_error(uint32_t data) {
  if (!this->state_changed_ && !this->heating_status_) {
    std::string value = "Operation blocked by not allowed start";
    for (auto &listener : listeners_) {
      listener->on_heater_state(value);
    }
  }
  this->state_changed_ = false;
}

void SAUNA360Component::process_relay_bitmap(uint32_t data) {
  // Remember previous heater state
  const bool prev_heater_on = this->last_heater_on_;

  // Decode light using model-specific mask
  const bool light_on = (data & this->LIGHT_MASK_) != 0;

  // Decode coils using model-specific mask/shift
  const uint8_t coilmap =
      static_cast<uint8_t>((data & this->COILS_MASK_) >> this->COILS_SHIFT_);
  const bool c1 = (coilmap & 0x01) != 0;
  const bool c2 = (coilmap & 0x02) != 0;
  const bool c3 = (coilmap & 0x04) != 0;
  const int active_coils = (c1 ? 1 : 0) + (c2 ? 1 : 0) + (c3 ? 1 : 0);
  const bool any_coil = (active_coils > 0);

  for (auto &l : listeners_)
    l->on_coils_active(static_cast<uint8_t>(active_coils));

  // Consider heater enabled if status flag or any coil energized
  const bool heater_enabled = this->heating_status_ || any_coil;

  // Optional AUX (we only log if present for COMBI/ELITE)
  const uint32_t AUX1_MASK = 0x00040000;
  const uint32_t AUX2_MASK = 0x00080000;
  const uint32_t AUX3_MASK = 0x00100000;
  const uint32_t AUX_MASK = (AUX1_MASK | AUX2_MASK | AUX3_MASK);
  const bool aux1_on = (data & AUX1_MASK) != 0;
  const bool aux2_on = (data & AUX2_MASK) != 0;
  const bool aux3_on = (data & AUX3_MASK) != 0;

  // Raw frame log
  ESP_LOGI(TAG, "0x7180 raw: 0x%08X (bits:%02X.%02X.%02X.%02X)",
           (unsigned)data, (unsigned)((data >> 24) & 0xFF),
           (unsigned)((data >> 16) & 0xFF), (unsigned)((data >> 8) & 0xFF),
           (unsigned)(data & 0xFF));

  // Pretty coil tuple and binary map (C3 C2 C1)
  char coil_tuple[32];
  snprintf(coil_tuple, sizeof(coil_tuple), "(%s,%s,%s)", c1 ? "ON" : "OFF",
           c2 ? "ON" : "OFF", c3 ? "ON" : "OFF");

  char coilmap_bin[4];
  coilmap_bin[0] = (coilmap & 0x04) ? '1' : '0';
  coilmap_bin[1] = (coilmap & 0x02) ? '1' : '0';
  coilmap_bin[2] = (coilmap & 0x01) ? '1' : '0';
  coilmap_bin[3] = '\0';

  const char *derived_state =
      !heater_enabled ? "OFF" : (any_coil ? "Heating" : "Standby");

  ESP_LOGI(
      TAG, "Light: %s, Heater: %s, Coils: %s (active: %d, map: 0b%s [C3C2C1])",
      ONOFF(light_on), derived_state, coil_tuple, active_coils, coilmap_bin);

  if (this->mode_ == Mode::COMBI || this->mode_ == Mode::ELITE ||
      this->mode_ == Mode::COMBI_ELITE) {
    if ((data & AUX_MASK) != 0) {
      ESP_LOGI(TAG, "AUX relays: [%s,%s,%s]", ONOFF(aux1_on), ONOFF(aux2_on),
               ONOFF(aux3_on));
    } else {
      ESP_LOGI(TAG, "AUX relays: [-]");
    }
  }

  // Session timer: track OFF -> ON and ON -> OFF
  if (!prev_heater_on && heater_enabled) {
    this->session_active_ = true;
    this->session_start_ms_ = millis();
    this->session_frozen_s_ = 0;
    this->last_session_pub_ms_ = millis();
    this->publish_session_();
  } else if (prev_heater_on && !heater_enabled) {
    if (this->session_active_) {
      const uint32_t now = millis();
      this->session_frozen_s_ = (now - this->session_start_ms_) / 1000u;
      this->session_active_ = false;
      this->publish_session_();
    }
  }

  // Publish to HA switches
  if (this->light_relay_switch_ != nullptr)
    this->light_relay_switch_->publish_state(light_on);
  if (this->heater_relay_switch_ != nullptr)
    this->heater_relay_switch_->publish_state(heater_enabled);

  // Cache the truth from 0x7180
  this->last_light_on_ = light_on;
  this->last_heater_on_ = heater_enabled;
  if (!this->relays_known_) {
    ESP_LOGI(
        TAG,
        "Relay map learned from 0x7180 - relay control is now idempotent.");
  }
  this->relays_known_ = true;

  // resync bath time to last user intent
  if (!prev_heater_on && heater_enabled) {
    if (this->last_bath_time_target_ >= 0) {
      const int current_minutes = this->decode_bath_time_minutes_(
          this->bath_time_received_hex_ & 0x0FFF);
      const int target_minutes = this->last_bath_time_target_;
      if (std::abs(current_minutes - target_minutes) >= 2) {
        this->set_bath_time_number(static_cast<float>(target_minutes));
        ESP_LOGI(TAG, "Bath time resynced after ON: target=%d (was %d)",
                 target_minutes, current_minutes);
      }
    }
  }

  // Notify listeners
  std::string state_str(derived_state);
  for (auto &listener : listeners_) {
    listener->on_light_status(light_on);
    listener->on_heater_status(heater_enabled);
    listener->on_heater_state(state_str);
  }
}

void SAUNA360Component::process_tank_level(uint32_t data) {
  const uint32_t lvl = (data & 0x00001800);

  int level_pct = -1;
  switch (lvl) {
  case 0x0000:
    level_pct = 0;
    break;
  case 0x0800:
    level_pct = 50;
    break;
  case 0x1000:
    level_pct = 100;
    break;
  case 0x1800:
    ESP_LOGI(TAG, "Tank level error: (0x%08X)", (unsigned)data);
    return;
  default:
    ESP_LOGI(TAG, "Tank level unknown: 0x%08X", (unsigned)data);
    return;
  }
  ESP_LOGI(TAG, "Tank level: %d%%", level_pct);

  for (auto &listener : listeners_) {
    listener->on_water_tank_level(static_cast<uint16_t>(level_pct));
  }
}

void SAUNA360Component::process_time_limit(uint32_t data) {
  int from_min = (data & 0x3F);
  int from_hour = ((data >> 6) & 0x1F);
  int until_min = ((data >> 11) & 0x3F);
  int until_hour = ((data >> 17) & 0x1F);
  bool active = ((data >> 22) & 0x01);

  ESP_LOGI(TAG, "Not-allowed start window: %02d:%02d -> %02d:%02d (active: %s)",
           from_hour, from_min, until_hour, until_min, active ? "yes" : "no");
}

void SAUNA360Component::process_total_uptime(uint32_t data) {
  for (auto &listener : listeners_) {
    listener->on_total_uptime(data);
  }
  ESP_LOGI(TAG, "Total uptime: %u minutes", (unsigned)data);
}

void SAUNA360Component::process_remaining_time(uint32_t data) {
  const uint16_t raw = static_cast<uint16_t>(data & 0xFFFF);

  // Normalize sentinels to 0 minutes
  const bool is_sentinel =
      (raw == 0xFFFC) || (raw == 0xFFFD) || (raw == 0xFFFE) || (raw == 0xFFFF);
  const uint16_t minutes = is_sentinel ? 0 : raw;

  // Publish to listeners / sensors
  for (auto &listener : listeners_) {
    listener->on_remaining_time(minutes);
  }

  // Log as unsigned to avoid negative prints
  ESP_LOGI(TAG, "Remaining time: %u minutes (raw=0x%04X%s)",
           static_cast<unsigned>(minutes), static_cast<unsigned>(raw),
           is_sentinel ? ", normalized from sentinel" : "");
}

void SAUNA360Component::process_door_error(uint32_t data) {
  for (auto &listener : listeners_) {
    listener->on_ready_status((~data) & 1);
  }
  if (data == 0x00060001) {
    std::string value = "Operation blocked by not allowed start";
    for (auto &listener : listeners_) {
      listener->on_heater_state(value);
    }
    this->create_send_data_(0x07, 0xB000, 0x00060101);
  }
  if (data == 0x00130001) {
    std::string value = "Operation blocked by not allowed start";
    for (auto &listener : listeners_) {
      listener->on_heater_state(value);
    }
    this->create_send_data_(0x07, 0xB000, 0x00130101);
  }
  if (data == 0x00130003) {
    std::string value = "Door opened too long, bath cancelled";
    for (auto &listener : listeners_) {
      listener->on_heater_state(value);
    }
    this->create_send_data_(0x07, 0xB000, 0x00130103);
  }
  if (data == 0x00140003) {
    std::string value = "Door has been open, check sauna";
    for (auto &listener : listeners_) {
      listener->on_heater_state(value);
    }
    this->create_send_data_(0x07, 0xB000, 0x00140000);
  }
}

void SAUNA360Component::process_sensor_error(uint32_t data) {
  std::string error_message;
  if ((data & 0xF0000000) == 0x30000000) {
    error_message = "Room temperature sensor not connected or malfunctioning";
  } else if ((data & 0xF0000000) == 0x10000000) {
    error_message = "High temperature limit control tripped, must be reset";
  }

  if (!error_message.empty()) {
    for (auto &listener : listeners_) {
      listener->on_heater_state(error_message);
    }
    ESP_LOGI(TAG, "Sensor error: %s", error_message.c_str());
  }
}

void SAUNA360Component::set_max_bath_temperature_number(float value) {
  value = std::round(value);

  uint32_t minutes_low12 = 0;
  if (this->last_bath_time_target_ >= 0) {
    minutes_low12 =
        static_cast<uint32_t>(this->last_bath_time_target_) & 0x0FFF;
  } else {
    minutes_low12 = this->bath_time_received_hex_ & 0x0FFF;
  }

  uint32_t data = minutes_low12 & 0x0FFF;
  data |= (((static_cast<uint32_t>(value) * 18) & 0x0FFF) << 20);

  this->create_send_data_(0x07, 0x4002, data);
  ESP_LOGI(TAG, "SENT: Max bath temperature: %.0f°C (preserved minutes=%u)",
           value, (unsigned)minutes_low12);
}

void SAUNA360Component::set_bath_time_number(float value) {
  int target = static_cast<int>(std::lround(value));
  if (target < 0)
    target = 0;
  if (target > 575)
    target = 575;

  this->last_bath_time_target_ = target;
  this->last_bath_time_set_ms_ = millis();

  const int encoded = this->encode_bath_time_raw_(target);
  uint32_t payload = static_cast<uint32_t>(encoded) & 0x0FFF;

  // High bits: max bath temperature (12 Bit) keep/set
  if (this->max_bath_temperature_received_hex_) {
    payload |= ((this->max_bath_temperature_received_hex_ & 0xFFF) << 20);
  } else {
    const uint32_t hi =
        static_cast<uint32_t>(this->max_bath_temperature_default_ * 18) & 0xFFF;
    payload |= (hi << 20);
  }

  this->create_send_data_(0x07, 0x4002, payload);
  ESP_LOGI(TAG, "SENT: Bath time target=%d min -> raw=%d (0x%03X)", target,
           encoded, encoded);
}

void SAUNA360Component::set_bath_temperature_number(float value) {
  float current_set_temp =
      (this->setpoint_temperature_received_hex_ & 0x00007FF) / 9.0f;
  value = std::round(value);
  if (static_cast<int>(value) == static_cast<int>(current_set_temp)) {
    return; // nothing to do
  }

  uint32_t data = 0;
  data |= (static_cast<uint32_t>(value) * 9) << 11;       // encode target
  data |= (this->temperature_received_hex_ & 0x000007FF); // keep current temp

  this->create_send_data_(0x07, 0x6000, data);
  ESP_LOGI(TAG, "SENT: Bath temperature: %.0f°C", value);
}

void SAUNA360Component::set_light_relay(bool enable) {
  const uint32_t now = millis();

  if (this->relays_known_) {
    if (this->last_light_on_ == enable) {
      ESP_LOGD(TAG, "Light already %s (0x7180 truth) - no toggle.",
               enable ? "ON" : "OFF");
      return;
    }
  } else {
    if ((now - this->last_light_cmd_ms_) < 250) {
      ESP_LOGD(TAG, "Light toggle suppressed (no relay truth yet; debounce).");
      return;
    }
  }

  this->create_send_data_(0x07, 0x7000, 0x2);
  this->last_light_cmd_ms_ = now;
  ESP_LOGI(TAG, "LIGHT toggle sent (target=%s)", enable ? "ON" : "OFF");
}

void SAUNA360Component::set_heater_relay(bool enable) {
  const uint32_t now = millis();

  if (this->relays_known_) {
    if (this->last_heater_on_ == enable) {
      ESP_LOGD(TAG, "Heater already %s (0x7180 truth) - no toggle.",
               enable ? "ON" : "OFF");
      return;
    }
  } else {
    if ((now - this->last_heater_cmd_ms_) < 250) {
      ESP_LOGD(TAG, "Heater toggle suppressed (no relay truth yet; debounce).");
      return;
    }
  }

  this->create_send_data_(0x07, 0x7000, 0x1);
  this->last_heater_cmd_ms_ = now;
  ESP_LOGI(TAG, "HEATER toggle sent (target=%s)", enable ? "ON" : "OFF");
}

void SAUNA360Component::set_humidity_step_number(float value) {
  int v = static_cast<int>(std::lround(value));
  if (v < 0)
    v = 0;
  if (v > 10)
    v = 10;

  // Step-Mode: upper Nibble 0, keep Priority-Bits
  uint32_t payload = (this->bath_type_priority_received_hex_ & 0x0FFFFFFF);

  uint32_t raw =
      static_cast<uint32_t>(v * HUM_STEP_SCALE + HUM_STEP_BASE) & 0xFF;
  payload &= ~(0xFFu << 4);
  payload |= (raw << 4);

  this->create_send_data_(0x07, 0x6001, payload);
  ESP_LOGI(TAG, "SENT: Humidity step: %d (payload=0x%08X)", v,
           (unsigned)payload);

#ifdef USE_NUMBER
  if (this->humidity_step_number_ != nullptr) {
    this->humidity_step_number_->publish_state(static_cast<float>(v));
    this->humidity_step_published_ = true;
  }
#endif
}

void SAUNA360Component::set_humidity_percent_number(float value) {
  int v = static_cast<int>(std::lround(value));
  if (v < 0)
    v = 0;
  if (v > 63)
    v = 63;

  uint32_t payload = this->bath_type_priority_received_hex_;
  payload |= 0x10000000u; // Percent-Mode

  payload &= ~(0x3Fu << 7);
  payload |= (static_cast<uint32_t>(v) & 0x3F) << 7;

  this->create_send_data_(0x07, 0x6001, payload);
  ESP_LOGI(TAG, "SENT: Humidity target (%%): %d (payload=0x%08X)", v,
           (unsigned)payload);

#ifdef USE_NUMBER
  if (this->humidity_percent_number_ != nullptr) {
    this->humidity_percent_number_->publish_state(static_cast<float>(v));
    this->humidity_percent_published_ = true;
  }
#endif
}

void SAUNA360Component::create_send_data_(uint8_t type, uint16_t code,
                                          uint32_t data) {
  ESP_LOGD(TAG, "CREATING SEND DATA TYPE:%02X CODE:%04X DATA:%08X", type, code,
           (unsigned) data);

  std::vector<uint8_t> packet = {0x40, type};
  std::array<uint8_t, 2> code_array = decode_value(code);
  std::array<uint8_t, 4> data_array = decode_value(data);

  packet.insert(packet.end(), code_array.begin(), code_array.end());
  packet.insert(packet.end(), data_array.begin(), data_array.end());

  uint16_t crc_calculated =
      crc16be(packet.data(), packet.size(), 0xffff, 0x90d9, false, false);
  std::array<uint8_t, 2> crc_array = decode_value(crc_calculated);
  packet.insert(packet.end(), crc_array.begin(), crc_array.end());

  packet.insert(packet.begin(), 0x98); // SOF
  packet.push_back(0x9C);              // EOF

  if (this->tx_queue_ == nullptr || packet.size() > sizeof(TxFrame::data)) {
    ESP_LOGE(TAG, "TX frame dropped (size=%u)", (unsigned)packet.size());
    return;
  }

  TxFrame frame;
  frame.len = packet.size();
  memcpy(frame.data, packet.data(), packet.size());

  if (xQueueSend(this->tx_queue_, &frame, 0) != pdTRUE) {
    ESP_LOGW(TAG, "TX queue full, frame dropped");
  }
}

void SAUNA360Component::send_data_() {
  TxFrame frame;
  if (xQueueReceive(this->tx_queue_, &frame, 0) != pdTRUE) {
    return;
  }

  const uart_port_t PORT = static_cast<uart_port_t>(this->heater_port_);

  if (this->flow_control_pin_ != nullptr) {
    this->flow_control_pin_->digital_write(true); // drive bus
    esp_rom_delay_us(10);
  }

  uart_write_bytes(PORT, reinterpret_cast<const char *>(frame.data), frame.len);

  if (this->flow_control_pin_ != nullptr) {
    // Release only after the last stop bit left the shift register
    uart_wait_tx_done(PORT, pdMS_TO_TICKS(20));
    this->flow_control_pin_->digital_write(false);
  }
}

// ===========================================================================
// RS485 proxy (not in upstream)
//
//   heater ==bus A== [ESP] ==bus B== panel
//
// Measured on a Tylo Pure heater from a PC on the bus (2026-09-17):
//   - the heater registers exactly one panel (search 98 7F 00 .., the panel
//     announces with its ID, the heater assigns address 0x40) and ignores a
//     second one, so two devices on one bus can only talk over each other;
//   - the panel answers a poll the moment its stop bit ends, so there is no
//     free slot to share with it;
//   - the heater waits for replies: held back 2..150 ms it just polls slower
//     (gap ~30 ms + delay) and never drops the panel;
//   - a command sent in place of the panel's keep-alive reply is accepted
//     (light toggle 98 40 07 70 00 00 00 00 02 A3 B8 9C, light on in 40 ms).
// So every frame is forwarded whole, registration included, and the ESP only
// steps in when the panel is late (answers the heater itself) or, in CONTROL
// mode, when it has a queued command (sends it instead of the panel's
// keep-alive).
// ===========================================================================

namespace {
// Panel replies to the heater's configuration polls (98 40 06 <code> ..),
// captured from the real panel at registration. Only used when the panel is
// late or absent; otherwise its own answer is forwarded.
struct ConfigReply {
  uint16_t code;
  uint32_t data;
};
const ConfigReply CONFIG_REPLIES[] = {
    {0x1800, 0x01040100}, {0x1801, 0x00000000}, {0x1802, 0x01001782},
    {0x1803, 0x01001782}, {0x1A00, 0x00014801}, {0x1A01, 0x00000000},
    {0x3800, 0x00000000}, {0x3801, 0x00000000},
};
const uint8_t PANEL_KEEPALIVE[] = {0x98, 0x40, 0x07, 0xFD, 0xE3, 0x9C};
const char *const BUS_MODE_NAMES[] = {"Listen", "Forward", "Forward + Control"};
} // namespace

void SAUNA360Component::set_bus_mode(uint8_t mode) {
  if (mode > static_cast<uint8_t>(BusMode::CONTROL))
    mode = static_cast<uint8_t>(BusMode::LISTEN);
  const uint8_t old = this->bus_mode_.exchange(mode);
  if (old != mode) {
    ESP_LOGI(TAG, "Bus mode: %s -> %s", BUS_MODE_NAMES[old],
             BUS_MODE_NAMES[mode]);
  }
}

bool SAUNA360Component::is_panel_responding() const {
  const uint32_t t = this->last_panel_ms_.load();
  return t != 0 && (millis() - t) < 3000u;
}

bool SAUNA360Component::is_heater_talking() const {
  const uint32_t t = this->last_heater_ms_.load();
  return t != 0 && (millis() - t) < 3000u;
}

bool SAUNA360Component::assemble_(RawAssembler &a, uint8_t b) {
  if (b == 0x98) {
    a.len = 0;
    a.buf[a.len++] = b;
    a.in_frame = true;
    return false;
  }
  if (!a.in_frame)
    return false;
  if (a.len >= RAW_MAX) {
    a.in_frame = false; // runaway frame, resync on the next SOF
    return false;
  }
  a.buf[a.len++] = b;
  if (b == 0x9C) {
    a.in_frame = false;
    return true;
  }
  return false;
}

size_t SAUNA360Component::unstuff_(const uint8_t *raw, size_t len,
                                   uint8_t *out, size_t out_max) {
  size_t n = 0;
  if (len < 2)
    return 0;
  for (size_t i = 1; i + 1 < len && n < out_max; ++i) {
    uint8_t c = raw[i];
    if (c == 0x91 && i + 2 < len) {
      const uint8_t e = raw[++i];
      c = (e == 0x67) ? 0x98 : (e == 0x63) ? 0x9C : (e == 0x6E) ? 0x91 : e;
    }
    out[n++] = c;
  }
  return n; // addr, type, [code, data], crc_hi, crc_lo
}

void SAUNA360Component::decode_raw_(const uint8_t *raw, size_t len) {
  if (len <= 6)
    return; // keep-alives carry nothing to decode
  const std::vector<uint8_t> frame(raw, raw + len);
  this->handle_frame_(frame);
}

void SAUNA360Component::write_raw_(int port, const uint8_t *raw, size_t len) {
  if (port < 0)
    return;
  uart_write_bytes(static_cast<uart_port_t>(port),
                   reinterpret_cast<const char *>(raw), len);
}

void SAUNA360Component::write_body_(int port, const uint8_t *body,
                                    size_t len) {
  uint8_t out[RAW_MAX];
  size_t n = 0;
  const uint16_t crc = crc16be(body, len, 0xFFFF, 0x90D9, false, false);
  out[n++] = 0x98;
  auto put = [&](uint8_t c) {
    if (n + 3 > sizeof(out))
      return;
    if (c == 0x98) {
      out[n++] = 0x91;
      out[n++] = 0x67;
    } else if (c == 0x9C) {
      out[n++] = 0x91;
      out[n++] = 0x63;
    } else if (c == 0x91) {
      out[n++] = 0x91;
      out[n++] = 0x6E;
    } else {
      out[n++] = c;
    }
  };
  for (size_t i = 0; i < len; ++i)
    put(body[i]);
  put(static_cast<uint8_t>(crc >> 8));
  put(static_cast<uint8_t>(crc & 0xFF));
  out[n++] = 0x9C;
  this->write_raw_(port, out, n);
}

void SAUNA360Component::on_heater_frame_(const uint8_t *raw, size_t len) {
  this->heater_frames_++;
  this->last_heater_ms_ = millis();
  const uint8_t mode = this->bus_mode_.load();

  uint8_t body[20];
  const size_t blen = unstuff_(raw, len, body, sizeof(body));
  if (blen < 4)
    return;
  const uint8_t addr = body[0];
  const uint8_t type = body[1];

  if (mode == static_cast<uint8_t>(BusMode::LISTEN)) {
    // Panel still wired straight to the heater: this bus carries both sides.
    this->decode_raw_(raw, len);
    return;
  }

  // A new frame while the previous poll is still open means the panel never
  // answered it and the timeout has not fired yet - answer that one first.
  if (this->pending_)
    this->answer_heater_ourselves_();

  this->write_raw_(this->panel_port_, raw, len);

  if (addr == 0x40 && (type == 0x06 || type == 0x08)) {
    this->pending_ = true;
    this->pending_type_ = type;
    this->pending_body_len_ = std::min(blen, sizeof(this->pending_body_));
    memcpy(this->pending_body_, body, this->pending_body_len_);
    this->pending_since_us_ = esp_timer_get_time();
  }
  if (type == 0x08)
    this->decode_raw_(raw, len);
}

void SAUNA360Component::on_panel_frame_(const uint8_t *raw, size_t len) {
  this->panel_frames_++;
  this->last_panel_ms_ = millis();
  const uint8_t mode = this->bus_mode_.load();
  if (mode == static_cast<uint8_t>(BusMode::LISTEN))
    return; // bus B is not wired in this mode; never transmit

  uint8_t body[20];
  const size_t blen = unstuff_(raw, len, body, sizeof(body));
  if (blen < 4)
    return;
  const uint8_t addr = body[0];
  const uint8_t type = body[1];

  // Registration (announce 7F 01, 7F 03, confirm 40 05) is not a reply to a
  // poll - pass it straight through.
  if (addr != 0x40 || (type != 0x07 && type != 0x09)) {
    this->write_raw_(this->heater_port_, raw, len);
    return;
  }

  if (!this->pending_) {
    // Arrived after we already answered for the panel. Forwarding it now
    // would put two replies on bus A for one poll.
    this->replies_late_++;
    return;
  }
  const int64_t lat = esp_timer_get_time() - this->pending_since_us_;
  const uint32_t prev = this->reply_latency_us_.load();
  this->reply_latency_us_ =
      prev == 0 ? (uint32_t) lat
                : (uint32_t) ((prev * 7ull + (uint64_t) lat) / 8);
  const uint8_t polled = this->pending_type_;
  this->pending_ = false;

  const bool keepalive = (len == sizeof(PANEL_KEEPALIVE)) &&
                         memcmp(raw, PANEL_KEEPALIVE, len) == 0;
  // Usage policy: a heater start from the panel may be refused here, in
  // which case the heater gets a stand-in reply instead of the command.
  if (!keepalive && type == 0x07 && mode == static_cast<uint8_t>(BusMode::CONTROL) &&
      this->policy_rewrite_panel_command_(body, blen)) {
    this->panel_commands_++;
    return;
  }
  if (keepalive && polled == 0x06 &&
      mode == static_cast<uint8_t>(BusMode::CONTROL) &&
      uxQueueMessagesWaiting(this->tx_queue_) > 0) {
    this->send_data_(); // our command goes in the panel's reply slot
    this->commands_sent_++;
    return;
  }

  this->write_raw_(this->heater_port_, raw, len);
  if (type == 0x07 && !keepalive) {
    this->panel_commands_++;
    this->decode_raw_(raw, len); // logs as [ HEATER <-- PANEL ]
  }
}

void SAUNA360Component::answer_heater_ourselves_() {
  if (!this->pending_)
    return;
  this->pending_ = false;
  this->replies_missed_++;
  const uint8_t mode = this->bus_mode_.load();

  if (this->pending_type_ == 0x08) {
    // Data frame: acknowledge with the same code + data as type 09.
    if (this->pending_body_len_ < 10)
      return;
    uint8_t ack[8];
    memcpy(ack, this->pending_body_, sizeof(ack));
    ack[1] = 0x09;
    this->write_body_(this->heater_port_, ack, sizeof(ack));
    return;
  }

  // Plain poll (98 40 06 crc crc).
  if (this->pending_body_len_ <= 4) {
    if (mode == static_cast<uint8_t>(BusMode::CONTROL) &&
        uxQueueMessagesWaiting(this->tx_queue_) > 0) {
      this->send_data_();
      this->commands_sent_++;
    } else {
      this->write_raw_(this->heater_port_, PANEL_KEEPALIVE,
                       sizeof(PANEL_KEEPALIVE));
    }
    return;
  }

  // Configuration poll (98 40 06 <code> crc crc).
  const uint16_t code =
      encode_uint16(this->pending_body_[2], this->pending_body_[3]);
  uint32_t data = 0;
  for (const auto &r : CONFIG_REPLIES) {
    if (r.code == code) {
      data = r.data;
      break;
    }
  }
  const uint8_t reply[8] = {0x40,
                            0x07,
                            this->pending_body_[2],
                            this->pending_body_[3],
                            (uint8_t) (data >> 24),
                            (uint8_t) (data >> 16),
                            (uint8_t) (data >> 8),
                            (uint8_t) data};
  this->write_body_(this->heater_port_, reply, sizeof(reply));
}

void SAUNA360Component::proxy_task_() {
  const uart_port_t A = static_cast<uart_port_t>(this->heater_port_);
  const uart_port_t B = static_cast<uart_port_t>(this->panel_port_);
  uint8_t buf[64];
  uint32_t last_drop_log_ms = 0;

  for (;;) {
    bool idle = true;

    int n = uart_read_bytes(A, buf, sizeof(buf), 0);
    if (n > 0) {
      idle = false;
      for (int i = 0; i < n; ++i) {
        if (this->assemble_(this->asm_heater_, buf[i]))
          this->on_heater_frame_(this->asm_heater_.buf, this->asm_heater_.len);
      }
    }

    n = uart_read_bytes(B, buf, sizeof(buf), 0);
    if (n > 0) {
      idle = false;
      for (int i = 0; i < n; ++i) {
        if (this->assemble_(this->asm_panel_, buf[i]))
          this->on_panel_frame_(this->asm_panel_.buf, this->asm_panel_.len);
      }
    }

    const uint8_t mode = this->bus_mode_.load();
    if (this->pending_) {
      if (mode == static_cast<uint8_t>(BusMode::LISTEN)) {
        this->pending_ = false;
      } else if (esp_timer_get_time() - this->pending_since_us_ >
                 this->panel_reply_timeout_us_) {
        this->answer_heater_ourselves_();
      }
    }

    // Commands only leave the ESP in CONTROL mode. Anything queued in the
    // other modes (UI toggles, the boot-time defaults) is discarded, so
    // switching to CONTROL never releases a backlog of stale commands.
    if (mode != static_cast<uint8_t>(BusMode::CONTROL)) {
      TxFrame dropped;
      uint32_t count = 0;
      while (xQueueReceive(this->tx_queue_, &dropped, 0) == pdTRUE)
        count++;
      if (count > 0) {
        this->commands_dropped_ += count;
        const uint32_t now = millis();
        if (now - last_drop_log_ms > 1000u) {
          ESP_LOGW(TAG, "%u command(s) not sent: bus mode is %s",
                   (unsigned) count, BUS_MODE_NAMES[mode]);
          last_drop_log_ms = now;
        }
      }
    }

    if (idle)
      vTaskDelay(1); // CONFIG_FREERTOS_HZ is 1000 on ESPHome IDF builds
  }
}

void SAUNA360Component::publish_session_() {
  const uint32_t s = this->session_active_
                         ? ((millis() - this->session_start_ms_) / 1000u)
                         : this->session_frozen_s_;
  const uint32_t m = s / 60u;

  static uint32_t last_sent_min = 0xFFFFFFFFu;
  if (this->session_active_) {
    if (m == last_sent_min)
      return;
  }
  last_sent_min = m;

  for (auto &l : this->listeners_) {
    l->on_session_uptime(m);
  }
}

void SAUNA360Component::initialize_defaults() {
  ESP_LOGI(TAG, "=========== Queueing default values ===========");
  if (!std::isnan(this->max_bath_temperature_default_))
    ESP_LOGI(TAG, "Max bath temperature: %.0f°C",
             this->max_bath_temperature_default_);
  if (!std::isnan(this->bath_time_default_))
    ESP_LOGI(TAG, "Bath time: %.0f minutes", this->bath_time_default_);
  if (!std::isnan(this->bath_temperature_default_))
    ESP_LOGI(TAG, "Bath temperature: %.0f°C", this->bath_temperature_default_);
  if (!std::isnan(this->humidity_step_default_))
    ESP_LOGI(TAG, "Humidity step: %.0f", this->humidity_step_default_);
#ifdef USE_NUMBER
  if (!std::isnan(this->humidity_percent_default_))
    ESP_LOGI(TAG, "Humidity percent: %.0f%%", this->humidity_percent_default_);
#endif
  ESP_LOGI(TAG, "================================================");

  if (!std::isnan(this->bath_time_default_))
    this->set_bath_time_number(this->bath_time_default_);

  if (!std::isnan(this->max_bath_temperature_default_))
    this->set_max_bath_temperature_number(this->max_bath_temperature_default_);

  if (!std::isnan(this->bath_temperature_default_))
    this->set_bath_temperature_number(this->bath_temperature_default_);

  if (this->humidity_step_number_ != nullptr &&
      !std::isnan(this->humidity_step_default_))
    this->set_humidity_step_number(this->humidity_step_default_);

#ifdef USE_NUMBER
  if (this->humidity_percent_number_ != nullptr &&
      !std::isnan(this->humidity_percent_default_))
    this->set_humidity_percent_number(this->humidity_percent_default_);
#endif
}

void SAUNA360Component::dump_config() {
  ESP_LOGCONFIG(TAG, "UART component");
  ESP_LOGCONFIG(TAG, "Session timer: enabled");
  LOG_PIN("  Flow control pin: ", this->flow_control_pin_);
  ESP_LOGCONFIG(TAG, "  Heater UART: %d", this->heater_port_);
  if (this->panel_uart_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Proxy: panel UART %d, reply timeout %u ms",
                  this->panel_port_,
                  (unsigned)(this->panel_reply_timeout_us_ / 1000));
  }
}

} // namespace sauna360
} // namespace esphome
