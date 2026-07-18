#include "fakegps.h"

#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <esp_timer.h>
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
  if (this->tx_pin_ != nullptr) {
    this->tx_pin_->setup();
    this->tx_pin_->digital_write(!this->invert_);  // idle level
  }
  if (this->motion_pin_ != nullptr) {
    this->motion_pin_->setup();
    this->motion_pin_->digital_write(false);
  }
  if (this->pir_pin_ != nullptr) {
    this->pir_pin_->setup();
    this->pir_last_ = this->pir_pin_->digital_read();
  }
}

void FakeGPS::dump_config() {
  ESP_LOGCONFIG(TAG, "FakeGPS:");
  LOG_PIN("  TX pin: ", this->tx_pin_);
  LOG_PIN("  PIR in pin: ", this->pir_pin_);
  LOG_PIN("  Motion out pin: ", this->motion_pin_);
  ESP_LOGCONFIG(TAG, "  Serial: %" PRIu32 " baud, %uN%u, %s", this->baud_, this->data_bits_, this->stop_bits_,
                this->invert_ ? "inverted (idle low)" : "TTL (idle high)");
  ESP_LOGCONFIG(TAG, "  Sentences: %s%s%s%s", this->en_zda_ ? "ZDA " : "", this->en_rmc_ ? "RMC " : "",
                this->en_gga_ ? "GGA " : "", this->en_gsa_ ? "GSA" : "");
  ESP_LOGCONFIG(TAG, "  Time basis: %s, offset %+" PRId32 " ms, valid gate %s, every %" PRIu32 " s",
                this->time_basis_ == BASIS_LOCAL ? "local" : "utc", this->time_offset_ms_, YESNO(this->valid_gate_),
                this->interval_s_);
  ESP_LOGCONFIG(TAG, "  Motion: off delay %" PRIu32 " ms, strobe %" PRIu32 " ms every %" PRIu32 " ms",
                this->off_delay_ms_, this->strobe_pulse_ms_, this->restrobe_period_ms_);
}

void FakeGPS::set_invert(bool v) {
  this->invert_ = v;
  if (this->tx_pin_ != nullptr)
    this->tx_pin_->digital_write(!v);  // re-assert idle level with new polarity
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

  if (!this->stream_enabled_ || this->tx_pin_ == nullptr) {
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
  }
  if (!sentences.empty()) {
    this->tx_count_ += sentences.size();
    const std::string &last = sentences.back();
    this->last_sentence_ = last.substr(0, last.size() - 2);  // strip CR LF
  }
}

void FakeGPS::build_sentences_(time_t stamp, std::vector<std::string> &out) {
  ESPTime t = (this->time_basis_ == BASIS_LOCAL) ? ESPTime::from_epoch_local(stamp) : ESPTime::from_epoch_utc(stamp);
  const bool fix = this->synced_ || !this->valid_gate_;

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
    snprintf(body, sizeof(body), "GPRMC,%s,%c,%s,%c,%s,%c,000.0,000.0,%02u%02u%02u,,,%c", hhmmss, fix ? 'A' : 'V', lat,
             lat_h, lon, lon_h, t.day_of_month, t.month, (unsigned) (t.year % 100), fix ? 'A' : 'N');
    out.push_back(wrap_checksum_(body));
  }
  if (this->en_gga_) {
    snprintf(body, sizeof(body), "GPGGA,%s,%s,%c,%s,%c,%d,%02u,%.1f,%.1f,M,-34.0,M,,", hhmmss, lat, lat_h, lon, lon_h,
             fix ? 1 : 0, this->sats_, (double) this->hdop_, (double) this->altitude_m_);
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
    snprintf(body, sizeof(body), "GPGSA,A,%d,%s,%.1f,%.1f,%.1f", fix ? 3 : 1, satlist, (double) (this->hdop_ * 2.0f),
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
  this->tx_pin_->digital_write(this->invert_ ? !logical : logical);
  deadline_ns += bit_ns;
  while ((int64_t) esp_timer_get_time() * 1000LL < deadline_ns) {
  }
}

void FakeGPS::motion_loop_() {
  const uint32_t now = millis();

  if (this->pir_pin_ != nullptr) {
    bool pir = this->pir_pin_->digital_read();
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

  if (this->motion_pin_ == nullptr)
    return;

  if (want_on && !this->strobe_active_ &&
      (this->force_strobe_ || (now - this->last_strobe_ms_) >= this->restrobe_period_ms_)) {
    this->strobe_active_ = true;
    this->force_strobe_ = false;
    this->strobe_start_ms_ = now;
    this->last_strobe_ms_ = now;
    this->motion_pin_->digital_write(true);
  }

  if (this->strobe_active_ && (now - this->strobe_start_ms_) >= this->strobe_pulse_ms_) {
    this->strobe_active_ = false;
    this->motion_pin_->digital_write(false);
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
