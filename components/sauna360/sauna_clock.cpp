// Clock and the /sauna page - not in upstream f-io/esphome-tylo.
//
// The ESP has no internet at the sauna, so it has no time of its own. The
// heater does: it broadcasts a running clock in 0x4200 about once a minute,
// and keeps it across ESP reboots. On the test heater it read
// 2001-05-28 on 2026-09-17, i.e. never set - the basic panel has no clock.
//
// So the /sauna page has a button that sends the phone's local time here; it
// is written into the heater (0x4200, as a panel command) and also kept as a
// phone-synced ESP clock, which is used until the heater reports a plausible
// date (and is lost on reboot). Whether the heater accepts the write is
// visible on the page: the clock source changes to "heater".
//
// 0x4200 layout (decoded by upstream's process_datetime): minute bits 0-5,
// hour 6-10, day 12-16, month 17-20, year-2000 21-25. The other bits (bit 31
// is set on this heater) are preserved as last received.
//
// http://192.168.4.1/sauna        clock status, sync button, full history
// POST /sauna/clock?value=...     "YYYY-MM-DD HH:MM[:SS]", phone local time

#include "sauna360.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_WEBSERVER
#include "esphome/components/web_server_base/web_server_base.h"
#endif

#include <cstdio>
#include <cstring>

namespace esphome {
namespace sauna360 {

static const char *const CTAG = "sauna_clock";

namespace {
constexpr int MIN_PLAUSIBLE_YEAR = 2024;
constexpr uint32_t CLOCK_FIELDS_MASK = 0x03FFF7FF; // bits the date/time uses

// Days since 1970-01-01 for a civil date (H. Hinnant's algorithm).
int32_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned) (y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t) doe - 719468;
}

void civil_from_days(int32_t z, int &y, unsigned &m, unsigned &d) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned) (z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int) yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);
}

const int32_t EPOCH_2000_DAYS = days_from_civil(2000, 1, 1);
const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
} // namespace

uint32_t SAUNA360Component::local_from_fields_(int y, int mo, int d, int h,
                                               int mi, int s) {
  if (y < 2000 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 ||
      mi < 0 || mi > 59 || s < 0 || s > 59)
    return 0;
  const int32_t days = days_from_civil(y, (unsigned) mo, (unsigned) d) -
                       EPOCH_2000_DAYS;
  return (uint32_t) days * 86400u + (uint32_t) (h * 3600 + mi * 60 + s);
}

std::string SAUNA360Component::format_local(uint32_t t) {
  if (t == 0)
    return "clock not set";
  int y;
  unsigned m, d;
  civil_from_days((int32_t) (t / 86400u) + EPOCH_2000_DAYS, y, m, d);
  const unsigned secs = t % 86400u;
  char buf[32];
  snprintf(buf, sizeof(buf), "%u %s %d %02u:%02u", d, MONTHS[(m - 1) % 12], y,
           secs / 3600, (secs / 60) % 60);
  return buf;
}

uint32_t SAUNA360Component::heater_clock_local_s_() const {
  const uint32_t raw = this->heater_clock_raw_.load();
  if (raw == 0)
    return 0;
  const int minute = raw & 0x3F;
  const int hour = (raw >> 6) & 0x1F;
  const int day = (raw >> 12) & 0x1F;
  const int month = (raw >> 17) & 0x0F;
  const int year = ((raw >> 21) & 0x1F) + 2000;
  if (year < MIN_PLAUSIBLE_YEAR)
    return 0; // never set (2001 on the test heater)
  const uint32_t base = local_from_fields_(year, month, day, hour, minute, 0);
  if (base == 0)
    return 0;
  return base + (millis() - this->heater_clock_rx_ms_.load()) / 1000u;
}

uint32_t SAUNA360Component::now_local_s() const {
  const uint32_t heater = this->heater_clock_local_s_();
  if (heater != 0)
    return heater;
  if (this->phone_clock_base_s_ != 0)
    return this->phone_clock_base_s_ +
           (millis() - this->phone_clock_base_ms_) / 1000u;
  return 0;
}

std::string SAUNA360Component::get_clock_text() const {
  if (this->heater_clock_local_s_() != 0)
    return format_local(this->now_local_s()) + " (heater)";
  if (this->phone_clock_base_s_ != 0)
    return format_local(this->now_local_s()) + " (phone, until reboot)";
  return "not set - sync it at /sauna";
}

bool SAUNA360Component::set_clock_from_string(const std::string &value) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  const int n = sscanf(value.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s);
  if (n < 5)
    return false;
  const uint32_t local = local_from_fields_(y, mo, d, h, mi, s);
  if (local == 0 || y < MIN_PLAUSIBLE_YEAR || y > 2031)
    return false; // the heater's year field ends at 2031

  this->phone_clock_base_s_ = local;
  this->phone_clock_base_ms_ = millis();

  const uint32_t prev = this->heater_clock_raw_.load();
  uint32_t data = prev != 0 ? (prev & ~CLOCK_FIELDS_MASK) : 0x80000000u;
  data |= (uint32_t) (mi & 0x3F);
  data |= (uint32_t) (h & 0x1F) << 6;
  data |= (uint32_t) (d & 0x1F) << 12;
  data |= (uint32_t) (mo & 0x0F) << 17;
  data |= (uint32_t) ((y - 2000) & 0x1F) << 21;
  this->create_send_data_(0x07, 0x4200, data);
  ESP_LOGI(CTAG, "Clock set from phone: %04d-%02d-%02d %02d:%02d, written to "
                 "the heater (0x%08X)",
           y, mo, d, h, mi, (unsigned) data);
  return true;
}

std::string SAUNA360Component::get_recent_sessions_text() const {
  const StatsBlob &st = this->stats_;
  if (st.n == 0)
    return "none yet";
  std::string out;
  const uint32_t shown = st.n < 3 ? st.n : 3;
  for (uint32_t k = 0; k < shown; k++) {
    const uint32_t i = st.n - 1 - k;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%s, %u min", k ? " | " : "",
             format_local(st.starts[i]).c_str(),
             (unsigned) (st.lengths[i] / 60));
    out += buf;
  }
  return out;
}

#ifdef USE_WEBSERVER
namespace {
const char *end_text(uint8_t r) {
  switch (r) {
  case 1:
    return "time limit";
  case 2:
    return "not allowed";
  default:
    return "turned off";
  }
}

class SaunaPageHandler : public AsyncWebHandler {
public:
  explicit SaunaPageHandler(SAUNA360Component *parent) : parent_(parent) {}

  bool canHandle(AsyncWebServerRequest *request) const override {
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    const StringRef url = request->url_to(buf);
    return url == "/sauna" || url == "/sauna/" || url == "/sauna/clock";
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    const StringRef url = request->url_to(buf);
    if (url == "/sauna/clock") {
      if (request->method() != HTTP_POST) {
        request->send(405, "text/plain", "POST only");
        return;
      }
      const std::string value = request->arg("value");
      if (this->parent_->set_clock_from_string(value))
        request->send(200, "text/plain", "Clock set. Reloading ...");
      else
        request->send(400, "text/plain", "Could not read that date/time");
      return;
    }
    this->send_page_(request);
  }

protected:
  void send_page_(AsyncWebServerRequest *request) {
    std::string h;
    h.reserve(6000);
    h += "<!DOCTYPE html><html><head><meta charset=UTF-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>Sauna history</title><style>"
         ":root{color-scheme:light dark;--b:#8884}"
         "body{font-family:system-ui,sans-serif;margin:16px;max-width:640px}"
         "h1{font-size:1.3em;margin:0 0 12px}"
         "button{font-size:1em;padding:10px 16px;border-radius:8px;"
         "border:1px solid var(--b);cursor:pointer}"
         "table{border-collapse:collapse;width:100%;margin-top:12px}"
         "td,th{text-align:left;padding:6px 4px;border-bottom:1px solid var(--b)}"
         "td.n,th.n{text-align:right}.m{opacity:.7;font-size:.9em}"
         "</style></head><body><h1>Sauna history</h1>";

    h += "<p>Clock: <b>";
    h += this->parent_->get_clock_text();
    h += "</b></p><p><button onclick='syncClock()'>Sync clock from this "
         "phone</button> <span id=msg class=m></span></p>";

    char buf[160];
    snprintf(buf, sizeof(buf),
             "<p class=m>%u sessions, %.1f h in total, %u stopped by the time "
             "limit. Newest first; the last %u are kept.</p>",
             (unsigned) this->parent_->get_stat_sessions(),
             this->parent_->get_stat_total_hours(),
             (unsigned) this->parent_->get_stat_limit_stops(),
             (unsigned) SAUNA360Component::STATS_WINDOW_PUBLIC);
    h += buf;

    h += "<table><tr><th class=n>#</th><th>Started</th>"
         "<th class=n>Minutes</th><th>Ended by</th></tr>";
    const uint32_t n = this->parent_->stats_count_in_window();
    const uint32_t total = this->parent_->get_stat_sessions();
    for (uint32_t k = 0; k < n; k++) {
      uint32_t start, len;
      uint8_t reason;
      this->parent_->stats_entry(n - 1 - k, start, len, reason);
      snprintf(buf, sizeof(buf),
               "<tr><td class=n>%u</td><td>%s</td><td class=n>%u</td>"
               "<td>%s</td></tr>",
               (unsigned) (total - k),
               SAUNA360Component::format_local(start).c_str(),
               (unsigned) (len / 60), end_text(reason));
      h += buf;
    }
    if (n == 0)
      h += "<tr><td colspan=4 class=m>No sessions yet.</td></tr>";
    h += "</table><p class=m><a href='/'>Back to the controls</a></p>";

    h += "<script>function syncClock(){const d=new Date(),p=x=>String(x)"
         ".padStart(2,'0'),v=d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+"
         "p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes())+':'+"
         "p(d.getSeconds()),m=document.getElementById('msg');m.textContent="
         "'Sending ...';fetch('/sauna/clock?value='+encodeURIComponent(v),"
         "{method:'POST'}).then(r=>r.text()).then(t=>{m.textContent=t;"
         "setTimeout(()=>location.reload(),2000)}).catch(()=>m.textContent="
         "'Failed')}</script></body></html>";

    request->send(200, "text/html", h.c_str());
  }

  SAUNA360Component *parent_;
};
} // namespace
#endif // USE_WEBSERVER

void SAUNA360Component::clock_setup_() {
#ifdef USE_WEBSERVER
  if (web_server_base::global_web_server_base != nullptr) {
    web_server_base::global_web_server_base->add_handler(
        new SaunaPageHandler(this)); // NOLINT(cppcoreguidelines-owning-memory)
    ESP_LOGI(CTAG, "History page at /sauna");
  }
#endif
}

} // namespace sauna360
} // namespace esphome
