#include "fakegps.h"

#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <sys/time.h>

namespace esphome {
namespace fakegps {

static const char *const TAG = "fakegps";

// Epoch sanity floor for "system clock has been set": 2021-01-01 UTC.
static const time_t SYNC_EPOCH_FLOOR = 1609459200;
// How far before the emission instant we switch the main loop to high
// frequency, and how close we get before busy-spinning to the exact mark.
static const int64_t HF_WINDOW_US = 60000;
static const int64_t SPIN_WINDOW_US = 2000;

void FakeGPS::setup() {
  this->config_tx_pin_();
  this->config_motion_pin_();
  this->config_pir_pin_();
}

void FakeGPS::config_tx_pin_() {
  if (this->tx_gpio_ < 0)
    return;
  gpio_num_t pin = (gpio_num_t) this->tx_gpio_;
  gpio_reset_pin(pin);
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  gpio_set_level(pin, !this->invert_);  // idle level
}

void FakeGPS::config_motion_pin_() {
  if (this->motion_gpio_ < 0)
    return;
  gpio_num_t pin = (gpio_num_t) this->motion_gpio_;
  gpio_reset_pin(pin);
  gpio_set_direction(pin, GPIO_MODE_OUTPUT);
  gpio_set_level(pin, 0);
}

void FakeGPS::config_pir_pin_() {
  if (this->pir_gpio_ < 0)
    return;
  gpio_num_t pin = (gpio_num_t) this->pir_gpio_;
  gpio_reset_pin(pin);
  gpio_set_direction(pin, GPIO_MODE_INPUT);
  gpio_pullup_dis(pin);
  gpio_pulldown_en(pin);  // defined low when no PIR is wired
  this->pir_last_ = gpio_get_level(pin);
}

void FakeGPS::set_tx_gpio(int8_t pin) {
  if (pin == this->tx_gpio_)
    return;
  if (this->tx_gpio_ >= 0)
    gpio_reset_pin((gpio_num_t) this->tx_gpio_);
  this->tx_gpio_ = pin;
  this->config_tx_pin_();
  ESP_LOGI(TAG, "TX pin -> GPIO%d", pin);
}

void FakeGPS::set_pir_gpio(int8_t pin) {
  if (pin == this->pir_gpio_)
    return;
  if (this->pir_gpio_ >= 0)
    gpio_reset_pin((gpio_num_t) this->pir_gpio_);
  this->pir_gpio_ = pin;
  this->pir_last_ = false;
  this->config_pir_pin_();
  if (pin < 0) {
    ESP_LOGI(TAG, "PIR input -> none");
  } else {
    ESP_LOGI(TAG, "PIR input -> GPIO%d", pin);
  }
}

void FakeGPS::set_motion_gpio(int8_t pin) {
  if (pin == this->motion_gpio_)
    return;
  if (this->motion_gpio_ >= 0)
    gpio_reset_pin((gpio_num_t) this->motion_gpio_);
  this->motion_gpio_ = pin;
  this->strobe_active_ = false;
  this->config_motion_pin_();
  ESP_LOGI(TAG, "Motion output -> GPIO%d", pin);
}

void FakeGPS::dump_config() {
  ESP_LOGCONFIG(TAG, "FakeGPS:");
  ESP_LOGCONFIG(TAG, "  TX: GPIO%d, PIR in: GPIO%d, Motion out: GPIO%d", this->tx_gpio_, this->pir_gpio_,
                this->motion_gpio_);
  ESP_LOGCONFIG(TAG, "  Serial: %" PRIu32 " baud, %uN%u, %s", this->baud_, this->data_bits_, this->stop_bits_,
                this->invert_ ? "inverted (idle low)" : "TTL (idle high)");
  ESP_LOGCONFIG(TAG, "  Sentences: %s%s%s%s", this->en_zda_ ? "ZDA " : "", this->en_rmc_ ? "RMC " : "",
                this->en_gga_ ? "GGA " : "", this->en_gsa_ ? "GSA" : "");
  ESP_LOGCONFIG(TAG, "  Time basis: %s, offset %+" PRId32 " ms, every %" PRIu32 " s (silent until synced)",
                this->time_basis_ == BASIS_LOCAL ? "local" : "utc", this->time_offset_ms_, this->interval_s_);
  ESP_LOGCONFIG(TAG, "  Motion: off delay %" PRIu32 " ms, strobe %" PRIu32 " ms every %" PRIu32 " ms",
                this->off_delay_ms_, this->strobe_pulse_ms_, this->restrobe_period_ms_);
}

void FakeGPS::set_invert(bool v) {
  this->invert_ = v;
  if (this->tx_gpio_ >= 0)
    gpio_set_level((gpio_num_t) this->tx_gpio_, !v);  // re-assert idle level with new polarity
}

void FakeGPS::set_time_offset_ms(int32_t v) {
  this->time_offset_ms_ = v;
  this->next_emit_us_ = 0;  // reschedule with the new offset
}

void FakeGPS::set_sentence_interval_s(uint32_t v) {
  this->interval_s_ = std::max<uint32_t>(1, v);
  this->next_emit_us_ = 0;  // reschedule with the new interval
}

void FakeGPS::set_motion_mode(MotionMode m) {
  if (m != this->mode_) {
    this->mode_ = m;
    this->force_strobe_ = true;  // apply the new mode immediately
    ESP_LOGI(TAG, "Motion mode -> %s", m == MODE_AUTO ? "Auto" : (m == MODE_ON ? "Force On" : "Force Off"));
  }
}

void FakeGPS::tickle() {
  if (this->mode_ == MODE_OFF) {
    ESP_LOGD(TAG, "Tickle ignored (Force Off)");
    return;
  }
  this->motion_event_("tickle");
}

int FakeGPS::wifi_rssi_now() const {
  wifi_ap_record_t info;
  if (esp_wifi_sta_get_ap_info(&info) != ESP_OK)
    return -127;
  return info.rssi;
}

int FakeGPS::wifi_channel() const {
  wifi_ap_record_t info;
  if (esp_wifi_sta_get_ap_info(&info) != ESP_OK)
    return 0;
  return info.primary;
}

std::string FakeGPS::bssid() const {
  wifi_ap_record_t info;
  if (esp_wifi_sta_get_ap_info(&info) != ESP_OK)
    return "no link";
  return str_sprintf("%02X:%02X:%02X:%02X:%02X:%02X", info.bssid[0], info.bssid[1], info.bssid[2], info.bssid[3],
                     info.bssid[4], info.bssid[5]);
}

float FakeGPS::seconds_since_motion() const {
  if (!this->has_motion_)
    return NAN;
  return (millis() - this->last_motion_ms_) / 1000.0f;
}

float FakeGPS::countdown_remaining_s() const {
  if (this->mode_ != MODE_AUTO || !this->has_motion_)
    return 0.0f;
  uint32_t elapsed = millis() - this->last_motion_ms_;
  if (elapsed >= this->off_delay_ms_)
    return 0.0f;
  return (this->off_delay_ms_ - elapsed) / 1000.0f;
}

void FakeGPS::loop() {
  this->motion_loop_();

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  int64_t now_us = (int64_t) tv.tv_sec * 1000000LL + tv.tv_usec;
  this->synced_ = tv.tv_sec > SYNC_EPOCH_FLOOR;

  // The line stays completely silent until real time exists. Pre-sync output
  // would stamp boot-time garbage (1970); clocks that latch it typically
  // then reject the correct time as an implausible jump for a long lockout
  // period. There is deliberately no option to emit earlier.
  if (this->synced_ && !this->was_synced_)
    ESP_LOGI(TAG, "Time synced; NMEA stream starting");
  this->was_synced_ = this->synced_;

  if (!this->stream_enabled_ || this->tx_gpio_ < 0 || !this->synced_) {
    if (this->hf_active_) {
      this->hf_.stop();
      this->hf_active_ = false;
    }
    this->next_emit_us_ = 0;
    return;
  }

  const int64_t offset_us = (int64_t) this->time_offset_ms_ * 1000LL;

  // (Re)compute the next emission if unscheduled or hopelessly late (>0.5 s,
  // e.g. after a time jump or a long stall).
  if (this->next_emit_us_ == 0 || now_us > this->next_emit_us_ + 500000LL) {
    this->stamp_second_ = (now_us + offset_us) / 1000000LL + 1;
    this->next_emit_us_ = this->stamp_second_ * 1000000LL - offset_us;
  }

  int64_t due_in = this->next_emit_us_ - now_us;
  if (due_in > HF_WINDOW_US) {
    if (this->hf_active_) {
      this->hf_.stop();
      this->hf_active_ = false;
    }
    return;
  }
  if (due_in > SPIN_WINDOW_US) {
    if (!this->hf_active_) {
      this->hf_.start();  // tighten loop cadence as the mark approaches
      this->hf_active_ = true;
    }
    return;
  }

  // Within the spin window (or slightly late): hold until the exact mark.
  do {
    gettimeofday(&tv, nullptr);
    now_us = (int64_t) tv.tv_sec * 1000000LL + tv.tv_usec;
  } while (now_us < this->next_emit_us_);

  this->emit_burst_((time_t) this->stamp_second_);

  this->stamp_second_ += this->interval_s_;
  this->next_emit_us_ += (int64_t) this->interval_s_ * 1000000LL;
  if (this->hf_active_) {
    this->hf_.stop();
    this->hf_active_ = false;
  }
}

void FakeGPS::emit_burst_(time_t stamp) {
  std::vector<std::string> sentences;
  this->build_sentences_(stamp, sentences);
  for (auto &s : sentences) {
    this->tx_string_(s);
    App.feed_wdt();  // bursts are ~10s of ms; keep the watchdog happy
    // Device log only (web page / builder log) — deliberately NOT an HA
    // entity: state churn every burst floods the HA logbook.
    ESP_LOGD(TAG, "TX %s", s.substr(0, s.size() - 2).c_str());
  }
  if (!sentences.empty()) {
    this->tx_count_ += sentences.size();
    const std::string &last = sentences.back();
    this->last_sentence_ = last.substr(0, last.size() - 2);  // strip CR LF
  }
}

void FakeGPS::build_sentences_(time_t stamp, std::vector<std::string> &out) {
  // Only ever called post-sync, so fields are unconditionally valid/fixed.
  ESPTime t = (this->time_basis_ == BASIS_LOCAL) ? ESPTime::from_epoch_local(stamp) : ESPTime::from_epoch_utc(stamp);

  char hhmmss[16];
  snprintf(hhmmss, sizeof(hhmmss), "%02u%02u%02u.00", t.hour, t.minute, t.second);

  char lat[16], lon[16];
  char lat_h, lon_h;
  deg_to_dm_(this->latitude_, true, lat, sizeof(lat), lat_h);
  deg_to_dm_(this->longitude_, false, lon, sizeof(lon), lon_h);

  char body[128];

  if (this->en_zda_) {
    snprintf(body, sizeof(body), "GPZDA,%s,%02u,%02u,%04u,00,00", hhmmss, t.day_of_month, t.month, t.year);
    out.push_back(wrap_checksum_(body));
  }
  if (this->en_rmc_) {
    snprintf(body, sizeof(body), "GPRMC,%s,A,%s,%c,%s,%c,000.0,000.0,%02u%02u%02u,,,A", hhmmss, lat, lat_h, lon,
             lon_h, t.day_of_month, t.month, (unsigned) (t.year % 100));
    out.push_back(wrap_checksum_(body));
  }
  if (this->en_gga_) {
    snprintf(body, sizeof(body), "GPGGA,%s,%s,%c,%s,%c,1,%02u,%.1f,%.1f,M,-34.0,M,,", hhmmss, lat, lat_h, lon, lon_h,
             this->sats_, (double) this->hdop_, (double) this->altitude_m_);
    out.push_back(wrap_checksum_(body));
  }
  if (this->en_gsa_) {
    char satlist[64] = {0};
    size_t pos = 0;
    for (uint8_t i = 0; i < 12; i++) {
      if (i < this->sats_)
        pos += snprintf(satlist + pos, sizeof(satlist) - pos, "%02u%s", i + 1, i < 11 ? "," : "");
      else
        pos += snprintf(satlist + pos, sizeof(satlist) - pos, "%s", i < 11 ? "," : "");
    }
    snprintf(body, sizeof(body), "GPGSA,A,3,%s,%.1f,%.1f,%.1f", satlist, (double) (this->hdop_ * 2.0f),
             (double) this->hdop_, (double) (this->hdop_ * 1.667f));
    out.push_back(wrap_checksum_(body));
  }
}

std::string FakeGPS::wrap_checksum_(const std::string &body) {
  uint8_t cs = 0;
  for (char c : body)
    cs ^= (uint8_t) c;
  return str_sprintf("$%s*%02X\r\n", body.c_str(), cs);
}

void FakeGPS::deg_to_dm_(double deg, bool is_lat, char *buf, size_t len, char &hemi) {
  hemi = is_lat ? (deg >= 0 ? 'N' : 'S') : (deg >= 0 ? 'E' : 'W');
  double v = std::fabs(deg);
  int d = (int) v;
  double m = (v - d) * 60.0;
  if (m >= 59.9995) {  // avoid printing 60.000 minutes after rounding
    m = 0.0;
    d += 1;
  }
  snprintf(buf, len, is_lat ? "%02d%06.3f" : "%03d%06.3f", d, m);
}

void FakeGPS::tx_string_(const std::string &s) {
  if (this->tx_gpio_ < 0)
    return;
  const int64_t bit_ns = 1000000000LL / (int64_t) this->baud_;
  int64_t deadline_ns = (int64_t) esp_timer_get_time() * 1000LL;
  for (unsigned char c : s) {
    // Interrupts are re-enabled between bytes; if one ran, restart timing from
    // now so the next start bit is full-width (the line idles meanwhile).
    deadline_ns = std::max(deadline_ns, (int64_t) esp_timer_get_time() * 1000LL);
    InterruptLock lock;
    this->tx_bit_(false, deadline_ns, bit_ns);  // start bit (space)
    uint8_t ones = 0;
    for (uint8_t i = 0; i < this->data_bits_; i++) {
      bool bit = (c >> i) & 1;
      ones += bit;
      this->tx_bit_(bit, deadline_ns, bit_ns);
    }
    if (this->parity_ != PARITY_NONE) {
      bool p = (this->parity_ == PARITY_EVEN) ? (ones & 1) : !(ones & 1);
      this->tx_bit_(p, deadline_ns, bit_ns);
    }
    for (uint8_t i = 0; i < this->stop_bits_; i++)
      this->tx_bit_(true, deadline_ns, bit_ns);  // stop bit(s) = idle (mark)
  }
}

inline void FakeGPS::tx_bit_(bool logical, int64_t &deadline_ns, int64_t bit_ns) {
  // logical true = mark = idle. Non-inverted TTL: mark is high.
  gpio_set_level((gpio_num_t) this->tx_gpio_, this->invert_ ? !logical : logical);
  deadline_ns += bit_ns;
  while ((int64_t) esp_timer_get_time() * 1000LL < deadline_ns) {
  }
}

void FakeGPS::motion_loop_() {
  const uint32_t now = millis();

  if (this->pir_gpio_ >= 0) {
    bool pir = gpio_get_level((gpio_num_t) this->pir_gpio_);
    if (pir != this->pir_last_)
      ESP_LOGD(TAG, "PIR %s", pir ? "high" : "low");  // device log only, not an HA entity
    if (pir && !this->pir_last_ && this->mode_ != MODE_OFF)
      this->motion_event_("PIR");
    this->pir_last_ = pir;
  }

  bool want_on;
  switch (this->mode_) {
    case MODE_ON:
      want_on = true;
      break;
    case MODE_OFF:
      want_on = false;
      break;
    default:
      want_on = this->has_motion_ && (now - this->last_motion_ms_) < this->off_delay_ms_;
      break;
  }
  this->display_on_ = want_on;

  if (this->motion_gpio_ < 0)
    return;

  if (want_on && !this->strobe_active_ &&
      (this->force_strobe_ || (now - this->last_strobe_ms_) >= this->restrobe_period_ms_)) {
    this->strobe_active_ = true;
    this->force_strobe_ = false;
    this->strobe_start_ms_ = now;
    this->last_strobe_ms_ = now;
    this->strobe_count_++;
    gpio_set_level((gpio_num_t) this->motion_gpio_, 1);
  }

  if (this->strobe_active_ && (now - this->strobe_start_ms_) >= this->strobe_pulse_ms_) {
    this->strobe_active_ = false;
    gpio_set_level((gpio_num_t) this->motion_gpio_, 0);
  }
}

void FakeGPS::motion_event_(const char *source) {
  this->last_motion_ms_ = millis();
  this->has_motion_ = true;
  this->last_source_ = source;
  this->force_strobe_ = true;  // strobe right away, not at the next period
  ESP_LOGI(TAG, "Motion event (%s)", source);
}

}  // namespace fakegps
}  // namespace esphome

#endif  // USE_ESP32
