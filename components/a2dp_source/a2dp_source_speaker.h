// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <memory>
#include <string>

#include "esp_a2dp_api.h"
#include "esp_bt_defs.h"
#include "esp_gap_bt_api.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

namespace esphome::a2dp_source {

class A2dpSourceSpeaker : public Component, public speaker::Speaker {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }
  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;
  void set_volume(float volume) override;
  void set_mute_state(bool mute_state) override;

  void set_target_address(const std::string &address) { this->target_address_ = address; }
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_buffer_duration(uint32_t duration_ms) { this->buffer_duration_ms_ = duration_ms; }

 protected:
  enum class ConnectionState : uint8_t { DISCONNECTED, CONNECTING, CONNECTED };
  enum class MediaState : uint8_t { IDLE, CHECKING, STARTING, STARTED, SUSPENDING };
  enum class PendingAction : uint8_t { NONE, CONNECT, CHECK_READY, START, SUSPEND };

  static void gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void a2dp_callback_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  static int32_t audio_data_callback_(uint8_t *data, int32_t length);

  void handle_gap_event_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  void handle_a2dp_event_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  int32_t fill_audio_(uint8_t *data, int32_t length);
  bool parse_target_address_();
  bool initialize_bluetooth_();
  bool check_(esp_err_t result, const char *operation);
  bool is_target_(const esp_bd_addr_t address) const;
  bool stream_format_supported_() const;
  void request_media_retry_();
  void apply_volume_(int16_t *samples, size_t sample_count) const;
  void reset_playback_stats_();
  void report_playback_stats_();

  static A2dpSourceSpeaker *instance_;

  std::string target_address_;
  std::string device_name_;
  esp_bd_addr_t target_bda_{};
  uint32_t buffer_duration_ms_{500};
  std::unique_ptr<ring_buffer::RingBuffer> audio_ring_buffer_;

  std::atomic<bool> profile_ready_{false};
  std::atomic<bool> play_requested_{false};
  std::atomic<bool> finish_requested_{false};
  std::atomic<bool> buffer_drained_{false};
  std::atomic<bool> reconnect_requested_{false};
  std::atomic<bool> media_retry_requested_{false};
  std::atomic<uint8_t> source_channels_{2};
  std::atomic<int32_t> volume_q15_{32767};
  std::atomic<bool> muted_{false};
  std::atomic<ConnectionState> connection_state_{ConnectionState::DISCONNECTED};
  std::atomic<MediaState> media_state_{MediaState::IDLE};
  std::atomic<PendingAction> pending_action_{PendingAction::NONE};

  std::atomic<uint64_t> stats_bytes_enqueued_{0};
  std::atomic<uint64_t> stats_input_bytes_consumed_{0};
  std::atomic<uint64_t> stats_output_bytes_requested_{0};
  std::atomic<uint64_t> stats_silence_bytes_{0};
  std::atomic<uint32_t> stats_callback_count_{0};
  std::atomic<int64_t> stats_first_callback_us_{0};
  std::atomic<int64_t> stats_last_callback_us_{0};
  std::atomic<bool> stats_report_requested_{false};

  uint32_t next_connect_at_{0};
  uint32_t next_media_check_at_{0};
  bool format_error_logged_{false};
};

}  // namespace esphome::a2dp_source
