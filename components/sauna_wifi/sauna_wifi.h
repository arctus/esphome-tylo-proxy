#pragma once
// Wi-Fi with a fallback access point that does not disturb its own clients.
//
// On an ESP32, ESPHome keeps the station searching for its network the whole
// time the fallback AP is up: a scan, a 500 ms pause, the next scan, for ever.
// Each scan takes the radio off the AP's channel, which is what a phone on
// the device's page notices. ESPHome only slows this down while a captive
// portal runs, and a captive portal takes over "/" - here the device's own
// page. So once the AP has been up for `search_time` without a connection,
// the network is taken out of ESPHome's list ("parked"): with no network to
// try, ESPHome's Wi-Fi state machine stands still and the AP runs undisturbed.
// Every `retry_interval` the network is put back and tried for another
// `search_time`, the way improv hands over new credentials (set_sta +
// start_connecting). A retry waits while a phone is joined to the AP, but
// never longer than `max_defer`.

#include <string>

#include "esphome/core/component.h"
#include "esphome/components/wifi/wifi_component.h"

namespace esphome {
namespace sauna_wifi {

class SaunaWifi : public Component {
 public:
  void set_search_time(uint32_t ms) { this->search_time_ = ms; }
  void set_retry_interval(uint32_t ms) { this->retry_interval_ = ms; }
  void set_max_defer(uint32_t ms) { this->max_defer_ = ms; }

  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  /// True while the network search is paused and only the AP runs.
  bool is_parked() const { return this->parked_; }
  /// Phones joined to the device's access point (0 when the AP is down).
  uint8_t ap_client_count() const;
  /// The station's IPv4 address on the network, or "" when not joined.
  std::string sta_ip() const;
  /// URL of `path` on the device as a visitor reaches it right now.
  std::string page_url(const char *path) const;
  /// One line for a diagnostics text sensor.
  std::string status_text() const;

 protected:
  uint32_t search_time_{120000};
  uint32_t retry_interval_{900000};
  uint32_t max_defer_{3600000};

  wifi::WiFiAP network_{};  // Copy of the configured network, kept while parked.
  bool have_network_{false};
  bool parked_{false};
  uint32_t ap_since_{0};      // When the AP was first seen up without a connection.
  uint32_t parked_since_{0};  // When the network was parked.
  uint32_t last_tick_{0};
};

}  // namespace sauna_wifi
}  // namespace esphome
