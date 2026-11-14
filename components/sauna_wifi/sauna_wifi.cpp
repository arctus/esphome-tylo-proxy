#include "sauna_wifi.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esp_wifi.h"

#include <cinttypes>

namespace esphome {
namespace sauna_wifi {

static const char *const TAG = "sauna_wifi";
static constexpr uint32_t TICK_MS = 5000;

void SaunaWifi::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sauna Wi-Fi:\n"
                "  Search time: %" PRIu32 " s\n"
                "  Retry interval: %" PRIu32 " s\n"
                "  Max defer: %" PRIu32 " s",
                this->search_time_ / 1000, this->retry_interval_ / 1000, this->max_defer_ / 1000);
}

uint8_t SaunaWifi::ap_client_count() const {
  wifi_sta_list_t list{};
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK)
    return 0;
  return (uint8_t) list.num;
}

std::string SaunaWifi::sta_ip() const {
  auto *w = wifi::global_wifi_component;
  if (w == nullptr || !w->is_connected())
    return "";
  char buf[network::IP_ADDRESS_BUFFER_SIZE];
  for (auto &ip : w->wifi_sta_ip_addresses()) {
    if (ip.is_set() && ip.is_ip4())
      return ip.str_to(buf);
  }
  return "";
}

std::string SaunaWifi::page_url(const char *path) const {
  std::string ip = this->sta_ip();
  return "http://" + (ip.empty() ? std::string("192.168.4.1") : ip) + path;
}

std::string SaunaWifi::status_text() const {
  auto *w = wifi::global_wifi_component;
  if (w == nullptr)
    return "Unknown";
  std::string ip = this->sta_ip();
  if (!ip.empty()) {
    char ssid[wifi::SSID_BUFFER_SIZE];
    return std::string(w->wifi_ssid_to(ssid)) + ", " + ip;
  }
  if (!w->is_ap_active())
    return "Connecting ...";
  if (!this->have_network_)
    return "Own access point";
  return this->parked_ ? "Own access point (network not found, retrying later)"
                       : "Own access point (looking for network)";
}

void SaunaWifi::loop() {
  const uint32_t now = millis();
  if (now - this->last_tick_ < TICK_MS)
    return;
  this->last_tick_ = now;

  auto *w = wifi::global_wifi_component;
  if (w == nullptr)
    return;

  // Keep a copy of the network while ESPHome still holds it.
  if (!this->parked_ && w->has_sta()) {
    wifi::WiFiAP sta = w->get_sta();
    if (!sta.get_ssid().empty()) {
      this->network_ = sta;
      this->have_network_ = true;
    }
  }

  if (w->is_connected()) {
    if (this->parked_) {
      // Joined anyway (a late association): hand the network back so ESPHome
      // looks after the connection again.
      w->set_sta(this->network_);
      this->parked_ = false;
    }
    this->ap_since_ = 0;
    return;
  }

  if (!this->parked_) {
    if (!w->is_ap_active()) {
      this->ap_since_ = 0;
      return;
    }
    if (this->ap_since_ == 0) {
      this->ap_since_ = now;
      return;
    }
    if (this->have_network_ && now - this->ap_since_ > this->search_time_) {
      ESP_LOGI(TAG, "Network not found; pausing the search, access point stays up");
      w->clear_sta();
      esp_wifi_scan_stop();
      esp_wifi_disconnect();
      this->parked_ = true;
      this->parked_since_ = now;
    }
    return;
  }

  // Parked: look again every retry_interval, preferably while nobody is on the AP.
  const uint32_t parked_for = now - this->parked_since_;
  if (parked_for < this->retry_interval_)
    return;
  const uint8_t clients = this->ap_client_count();
  if (clients > 0 && parked_for < this->max_defer_)
    return;
  ESP_LOGI(TAG, "Looking for the network again (%u client(s) on the access point)", clients);
  w->set_sta(this->network_);
  w->start_connecting(this->network_);
  this->parked_ = false;
  this->ap_since_ = now;  // A fresh search_time window before parking again.
}

}  // namespace sauna_wifi
}  // namespace esphome
