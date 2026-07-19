#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/components/time/real_time_clock.h"

#include <string>
#include <vector>

namespace esphome {
namespace fakegps {

enum Parity : uint8_t { PARITY_NONE = 0, PARITY_EVEN, PARITY_ODD };
enum TimeBasis : uint8_t { BASIS_LOCAL = 0, BASIS_UTC };
enum MotionMode : uint8_t { MODE_AUTO = 0, MODE_ON, MODE_OFF };

class FakeGPS : public Component {
 public:
  // --- wiring / build-time config (from codegen) ---
  void set_time_source(time::RealTimeClock *rtc) { this->rtc_ = rtc; }
  void set_data_bits(uint8_t v) { this->data_bits_ = v; }
  void set_stop_bits(uint8_t v) { this->stop_bits_ = v; }
  void set_parity(Parity v) { this->parity_ = v; }
  void set_time_basis(TimeBasis v) { this->time_basis_ = v; }
  void set_time_valid_gate(bool v) { this->valid_gate_ = v; }
  void set_altitude(float v) { this->altitude_m_ = v; }
  void set_hdop(float v) { this->hdop_ = v; }

  // --- runtime-adjustable (config sets defaults; HA entities call these live) ---
  // Pins are runtime-remappable via the GPIO matrix; -1 = unused.
  void set_tx_gpio(int8_t pin);
  void set_pir_gpio(int8_t pin);
  void set_motion_gpio(int8_t pin);
  void set_stream_enabled(bool v) { this->stream_enabled_ = v; }
  void set_baud(uint32_t v) { this->baud_ = v; }
  void set_invert(bool v);
  void set_time_offset_ms(int32_t v);
  void set_sentence_interval_s(uint32_t v);
  void set_en_gpzda(bool v) { this->en_zda_ = v; }
  void set_en_gprmc(bool v) { this->en_rmc_ = v; }
  void set_en_gpgga(bool v) { this->en_gga_ = v; }
  void set_en_gpgsa(bool v) { this->en_gsa_ = v; }
  void set_latitude(double v) { this->latitude_ = v; }
  void set_longitude(double v) { this->longitude_ = v; }
  void set_sats(uint8_t v) { this->sats_ = v; }
  void set_off_delay_ms(uint32_t v) { this->off_delay_ms_ = v; }
  void set_strobe_pulse_ms(uint32_t v) { this->strobe_pulse_ms_ = v; }
  void set_restrobe_period_ms(uint32_t v) { this->restrobe_period_ms_ = v; }
  void set_motion_mode(MotionMode m);
  void tickle();

  // --- status getters (for HA template entities / OLED lambdas) ---
  bool is_synced() const { return this->synced_; }
  bool stream_enabled() const { return this->stream_enabled_; }
  uint32_t tx_count() const { return this->tx_count_; }
  std::string last_sentence() const { return this->last_sentence_; }
  std::string last_motion_source() const { return this->last_source_; }
  bool display_on() const { return this->display_on_; }
  bool pir_state() const { return this->pir_last_; }
  bool motion_line_active() const { return this->strobe_active_; }
  float seconds_since_motion() const;
  float countdown_remaining_s() const;
  uint32_t active_baud() const { return this->baud_; }
  bool inverted() const { return this->invert_; }
  int32_t time_offset_ms() const { return this->time_offset_ms_; }
  uint32_t sentence_interval_s() const { return this->interval_s_; }
  MotionMode motion_mode() const { return this->mode_; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void emit_burst_(time_t stamp);
  void build_sentences_(time_t stamp, std::vector<std::string> &out);
  void tx_string_(const std::string &s);
  inline void tx_bit_(bool logical, int64_t &deadline_ns, int64_t bit_ns);
  void motion_loop_();
  void motion_event_(const char *source);
  void config_tx_pin_();
  void config_pir_pin_();
  void config_motion_pin_();
  static std::string wrap_checksum_(const std::string &body);
  static void deg_to_dm_(double deg, bool is_lat, char *buf, size_t len, char &hemi);

  // wiring
  time::RealTimeClock *rtc_{nullptr};
  int8_t tx_gpio_{0};
  int8_t pir_gpio_{1};
  int8_t motion_gpio_{10};

  // serial parameters
  uint32_t baud_{9600};
  bool invert_{false};
  uint8_t data_bits_{8};
  uint8_t stop_bits_{1};
  Parity parity_{PARITY_NONE};

  // NMEA content
  bool en_zda_{true}, en_rmc_{true}, en_gga_{true}, en_gsa_{true};
  double latitude_{40.2333}, longitude_{-82.85};
  uint8_t sats_{10};
  float altitude_m_{100.0f};
  float hdop_{0.9f};
  TimeBasis time_basis_{BASIS_LOCAL};
  bool valid_gate_{true};

  // emission scheduling
  bool stream_enabled_{true};
  int32_t time_offset_ms_{0};
  uint32_t interval_s_{5};
  int64_t next_emit_us_{0};
  int64_t stamp_second_{0};
  bool synced_{false};
  uint32_t tx_count_{0};
  std::string last_sentence_{};
  HighFrequencyLoopRequester hf_;
  bool hf_active_{false};

  // motion
  MotionMode mode_{MODE_AUTO};
  uint32_t off_delay_ms_{300000};
  uint32_t strobe_pulse_ms_{250};
  uint32_t restrobe_period_ms_{5000};
  bool pir_last_{false};
  bool has_motion_{false};
  uint32_t last_motion_ms_{0};
  std::string last_source_{"none"};
  bool display_on_{false};
  bool strobe_active_{false};
  bool force_strobe_{false};
  uint32_t strobe_start_ms_{0};
  uint32_t last_strobe_ms_{0};
};

}  // namespace fakegps
}  // namespace esphome
