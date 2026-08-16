// SPDX-License-Identifier: MIT
#include "a2dp_source_speaker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_a2dp_legacy_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::a2dp_source {

static const char *const TAG = "a2dp_source";
static constexpr uint32_t RECONNECT_DELAY_MS = 5000;
static constexpr uint32_t MEDIA_RETRY_DELAY_MS = 1000;
static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr size_t OUTPUT_FRAME_BYTES = sizeof(int16_t) * 2;
static constexpr size_t MONO_SCRATCH_SAMPLES = 128;

A2dpSourceSpeaker *A2dpSourceSpeaker::instance_ = nullptr;

float A2dpSourceSpeaker::get_setup_priority() const { return setup_priority::HARDWARE; }

void A2dpSourceSpeaker::setup() {
  if (instance_ != nullptr) {
    ESP_LOGE(TAG, "Only one A2DP source speaker is supported");
    this->mark_failed();
    return;
  }
  instance_ = this;

  const size_t buffer_size = static_cast<size_t>(SAMPLE_RATE) * OUTPUT_FRAME_BYTES * this->buffer_duration_ms_ / 1000;
  this->audio_ring_buffer_ = ring_buffer::RingBuffer::create(buffer_size);
  if (this->audio_ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u-byte PCM ring buffer", static_cast<unsigned>(buffer_size));
    this->mark_failed();
    return;
  }

  if (!this->parse_target_address_() || !this->initialize_bluetooth_()) {
    this->mark_failed();
    return;
  }

  this->state_ = speaker::STATE_STOPPED;
  this->set_volume(this->volume_);
  ESP_LOGI(TAG, "A2DP source initialized; waiting for profile-ready event");
}

void A2dpSourceSpeaker::loop() {
  if (this->is_failed() || !this->profile_ready_.load()) {
    return;
  }

  if (this->stats_report_requested_.exchange(false)) {
    this->report_playback_stats_();
  }

  const uint32_t now = millis();
  if (this->reconnect_requested_.exchange(false)) {
    this->next_connect_at_ = now + RECONNECT_DELAY_MS;
  }
  if (this->media_retry_requested_.exchange(false)) {
    this->next_media_check_at_ = now + MEDIA_RETRY_DELAY_MS;
  }

  if (this->connection_state_.load() == ConnectionState::DISCONNECTED &&
      this->pending_action_.load() == PendingAction::NONE && now >= this->next_connect_at_) {
    this->pending_action_.store(PendingAction::CONNECT);
  }

  const bool wants_audio = this->play_requested_.load();
  const MediaState media_state = this->media_state_.load();
  if (wants_audio && this->connection_state_.load() == ConnectionState::CONNECTED && media_state == MediaState::IDLE &&
      this->pending_action_.load() == PendingAction::NONE && now >= this->next_media_check_at_) {
    this->pending_action_.store(PendingAction::CHECK_READY);
  }

  if ((!wants_audio || (this->finish_requested_.load() && this->buffer_drained_.load())) &&
      media_state == MediaState::STARTED &&
      this->pending_action_.load() == PendingAction::NONE) {
    this->pending_action_.store(PendingAction::SUSPEND);
  }

  switch (this->pending_action_.exchange(PendingAction::NONE)) {
    case PendingAction::CONNECT: {
      ESP_LOGI(TAG, "Connecting to %s", this->target_address_.c_str());
      const esp_err_t result = esp_a2d_source_connect(this->target_bda_);
      if (result == ESP_OK) {
        this->connection_state_.store(ConnectionState::CONNECTING);
      } else {
        ESP_LOGW(TAG, "Connect request failed: %s", esp_err_to_name(result));
        this->next_connect_at_ = now + RECONNECT_DELAY_MS;
      }
      break;
    }
    case PendingAction::CHECK_READY: {
      ESP_LOGI(TAG, "Checking A2DP media readiness");
      const esp_err_t result = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
      if (result == ESP_OK) {
        this->media_state_.store(MediaState::CHECKING);
      } else {
        ESP_LOGW(TAG, "Media readiness request failed: %s", esp_err_to_name(result));
        this->request_media_retry_();
      }
      break;
    }
    case PendingAction::START: {
      if (!this->play_requested_.load()) {
        this->media_state_.store(MediaState::IDLE);
        break;
      }
      ESP_LOGI(TAG, "Starting A2DP media stream");
      const esp_err_t result = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
      if (result == ESP_OK) {
        this->media_state_.store(MediaState::STARTING);
      } else {
        ESP_LOGW(TAG, "Media start request failed: %s", esp_err_to_name(result));
        this->media_state_.store(MediaState::IDLE);
        this->request_media_retry_();
      }
      break;
    }
    case PendingAction::SUSPEND: {
      ESP_LOGI(TAG, "Suspending A2DP media stream");
      const esp_err_t result = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
      if (result == ESP_OK) {
        this->media_state_.store(MediaState::SUSPENDING);
      } else {
        ESP_LOGW(TAG, "Media suspend request failed: %s", esp_err_to_name(result));
        this->media_state_.store(MediaState::IDLE);
      }
      break;
    }
    case PendingAction::NONE:
      break;
  }

  if (this->play_requested_.load() && this->media_state_.load() == MediaState::STARTED) {
    this->state_ = speaker::STATE_RUNNING;
  } else if (!this->play_requested_.load() && this->media_state_.load() == MediaState::IDLE) {
    this->state_ = speaker::STATE_STOPPED;
  }
}

void A2dpSourceSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ESP-IDF A2DP Source Speaker:\n"
                "  Target address: %s\n"
                "  Device name: %s\n"
                "  PCM format: 16-bit, mono/stereo, 44100 Hz\n"
                "  Buffer duration: %" PRIu32 " ms\n"
                "  PCM encoder boundary: ESP-IDF legacy source callback",
                this->target_address_.c_str(), this->device_name_.c_str(), this->buffer_duration_ms_);
}

size_t A2dpSourceSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_failed() || data == nullptr || length == 0) {
    return 0;
  }
  if (!this->stream_format_supported_()) {
    if (!this->format_error_logged_) {
      ESP_LOGE(TAG, "Unsupported PCM format: %u-bit, %u channel(s), %" PRIu32 " Hz",
               this->audio_stream_info_.get_bits_per_sample(), this->audio_stream_info_.get_channels(),
               this->audio_stream_info_.get_sample_rate());
      this->format_error_logged_ = true;
    }
    return 0;
  }
  this->format_error_logged_ = false;
  this->source_channels_.store(this->audio_stream_info_.get_channels());

  if (this->state_ == speaker::STATE_STOPPED) {
    this->start();
  }
  if (!this->play_requested_.load()) {
    vTaskDelay(ticks_to_wait);
    return 0;
  }
  // Match ESPHome's speaker contract: the pipeline retains unwritten bytes and retries them.
  // Partial writes are required when a decoded transfer block is larger than this speaker's ring buffer.
  const size_t bytes_written = this->audio_ring_buffer_->write_without_replacement(data, length, ticks_to_wait, true);
  this->stats_bytes_enqueued_.fetch_add(bytes_written);
  return bytes_written;
}

void A2dpSourceSpeaker::start() {
  if (!this->is_ready() || this->is_failed() || this->play_requested_.load()) {
    return;
  }
  if (!this->stream_format_supported_()) {
    ESP_LOGE(TAG, "Cannot start unsupported PCM format: %u-bit, %u channel(s), %" PRIu32 " Hz",
             this->audio_stream_info_.get_bits_per_sample(), this->audio_stream_info_.get_channels(),
             this->audio_stream_info_.get_sample_rate());
    return;
  }
  this->audio_ring_buffer_->reset();
  this->reset_playback_stats_();
  this->source_channels_.store(this->audio_stream_info_.get_channels());
  this->finish_requested_.store(false);
  this->buffer_drained_.store(false);
  this->play_requested_.store(true);
  this->state_ = speaker::STATE_STARTING;
}

void A2dpSourceSpeaker::stop() {
  if (this->is_failed()) {
    return;
  }
  this->play_requested_.store(false);
  this->finish_requested_.store(false);
  this->buffer_drained_.store(false);
  this->audio_ring_buffer_->reset();
  this->state_ = speaker::STATE_STOPPING;
}

void A2dpSourceSpeaker::finish() {
  if (this->is_failed() || this->state_ == speaker::STATE_STOPPED) {
    return;
  }
  this->finish_requested_.store(true);
  if (!this->has_buffered_data()) {
    this->buffer_drained_.store(true);
  }
  this->state_ = speaker::STATE_STOPPING;
}

bool A2dpSourceSpeaker::has_buffered_data() const {
  return this->audio_ring_buffer_ != nullptr && this->audio_ring_buffer_->available() > 0;
}

void A2dpSourceSpeaker::set_volume(float volume) {
  this->volume_ = std::clamp(volume, 0.0f, 1.0f);
  this->volume_q15_.store(static_cast<int32_t>(std::lround(this->volume_ * 32767.0f)));
}

void A2dpSourceSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  this->muted_.store(mute_state);
}

bool A2dpSourceSpeaker::stream_format_supported_() const {
  return this->audio_stream_info_.get_bits_per_sample() == 16 &&
         (this->audio_stream_info_.get_channels() == 1 || this->audio_stream_info_.get_channels() == 2) &&
         this->audio_stream_info_.get_sample_rate() == SAMPLE_RATE;
}

bool A2dpSourceSpeaker::parse_target_address_() {
  unsigned int bytes[ESP_BD_ADDR_LEN];
  if (std::sscanf(this->target_address_.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &bytes[0], &bytes[1],
                  &bytes[2], &bytes[3], &bytes[4], &bytes[5]) != ESP_BD_ADDR_LEN) {
    ESP_LOGE(TAG, "Invalid Bluetooth address: %s", this->target_address_.c_str());
    return false;
  }
  for (size_t i = 0; i < ESP_BD_ADDR_LEN; i++) {
    this->target_bda_[i] = static_cast<uint8_t>(bytes[i]);
  }
  return true;
}

bool A2dpSourceSpeaker::initialize_bluetooth_() {
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
    const esp_err_t release_result = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (release_result != ESP_OK && release_result != ESP_ERR_INVALID_STATE) {
      return this->check_(release_result, "release BLE controller memory");
    }
    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (!this->check_(esp_bt_controller_init(&controller_config), "initialize BT controller")) {
      return false;
    }
  }
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED &&
      !this->check_(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT), "enable Classic BT controller")) {
    return false;
  }

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
    esp_bluedroid_config_t bluedroid_config = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if (!this->check_(esp_bluedroid_init_with_cfg(&bluedroid_config), "initialize Bluedroid")) {
      return false;
    }
  }
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED &&
      !this->check_(esp_bluedroid_enable(), "enable Bluedroid")) {
    return false;
  }

  if (!this->check_(esp_bt_gap_register_callback(gap_callback_), "register GAP callback") ||
      !this->check_(esp_bt_gap_set_device_name(this->device_name_.c_str()), "set Bluetooth device name")) {
    return false;
  }

  esp_bt_io_cap_t io_capability = ESP_BT_IO_CAP_NONE;
  if (!this->check_(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &io_capability, sizeof(io_capability)),
                    "set SSP capability")) {
    return false;
  }
  esp_bt_pin_code_t unused_pin{};
  if (!this->check_(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, unused_pin), "set legacy PIN mode")) {
    return false;
  }

  if (!this->check_(esp_a2d_register_callback(a2dp_callback_), "register A2DP callback") ||
      !this->check_(esp_a2d_source_register_data_callback(audio_data_callback_), "register PCM callback") ||
      !this->check_(esp_a2d_source_init(), "initialize A2DP source")) {
    return false;
  }
  return true;
}

bool A2dpSourceSpeaker::check_(esp_err_t result, const char *operation) {
  if (result == ESP_OK) {
    return true;
  }
  ESP_LOGE(TAG, "Failed to %s: %s", operation, esp_err_to_name(result));
  return false;
}

bool A2dpSourceSpeaker::is_target_(const esp_bd_addr_t address) const {
  return std::memcmp(address, this->target_bda_, ESP_BD_ADDR_LEN) == 0;
}

void A2dpSourceSpeaker::request_media_retry_() {
  this->media_state_.store(MediaState::IDLE);
  this->media_retry_requested_.store(true);
}

void A2dpSourceSpeaker::gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (instance_ != nullptr) {
    instance_->handle_gap_event_(event, param);
  }
}

void A2dpSourceSpeaker::handle_gap_event_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      if (this->is_target_(param->auth_cmpl.bda)) {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
          ESP_LOGI(TAG, "Pairing/authentication succeeded for %s", this->target_address_.c_str());
        } else {
          ESP_LOGW(TAG, "Pairing/authentication failed for %s (status %d)", this->target_address_.c_str(),
                   param->auth_cmpl.stat);
        }
      }
      break;
    case ESP_BT_GAP_PIN_REQ_EVT:
      if (this->is_target_(param->pin_req.bda)) {
        esp_bt_pin_code_t pin{};
        const uint8_t length = param->pin_req.min_16_digit ? 16 : 4;
        if (!param->pin_req.min_16_digit) {
          std::memcpy(pin, "0000", 4);
        }
        ESP_LOGI(TAG, "Replying to legacy PIN request (%u digits)", length);
        esp_bt_gap_pin_reply(param->pin_req.bda, true, length, pin);
      }
      break;
    case ESP_BT_GAP_CFM_REQ_EVT:
      if (this->is_target_(param->cfm_req.bda)) {
        ESP_LOGI(TAG, "Confirming SSP request for %s", this->target_address_.c_str());
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
      }
      break;
    default:
      break;
  }
}

void A2dpSourceSpeaker::a2dp_callback_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  if (instance_ != nullptr) {
    instance_->handle_a2dp_event_(event, param);
  }
}

void A2dpSourceSpeaker::handle_a2dp_event_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  switch (event) {
    case ESP_A2D_PROF_STATE_EVT:
      if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
        ESP_LOGI(TAG, "A2DP profile ready");
        this->profile_ready_.store(true);
        this->next_connect_at_ = 0;
      } else {
        ESP_LOGE(TAG, "A2DP profile initialization failed (state %d)", param->a2d_prof_stat.init_state);
      }
      break;
    case ESP_A2D_CONNECTION_STATE_EVT:
      if (!this->is_target_(param->conn_stat.remote_bda)) {
        break;
      }
      switch (param->conn_stat.state) {
        case ESP_A2D_CONNECTION_STATE_CONNECTING:
          ESP_LOGI(TAG, "A2DP connecting");
          this->connection_state_.store(ConnectionState::CONNECTING);
          break;
        case ESP_A2D_CONNECTION_STATE_CONNECTED:
          ESP_LOGI(TAG, "A2DP connected (MTU %u)", param->conn_stat.audio_mtu);
          this->connection_state_.store(ConnectionState::CONNECTED);
          this->media_state_.store(MediaState::IDLE);
          this->media_retry_requested_.store(false);
          break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
          ESP_LOGI(TAG, "A2DP disconnecting");
          break;
        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
          ESP_LOGW(TAG, "A2DP disconnected (reason %d)", param->conn_stat.disc_rsn);
          this->connection_state_.store(ConnectionState::DISCONNECTED);
          this->media_state_.store(MediaState::IDLE);
          this->pending_action_.store(PendingAction::NONE);
          this->reconnect_requested_.store(true);
          break;
      }
      break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
      switch (param->media_ctrl_stat.cmd) {
        case ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY:
          if (param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGI(TAG, "A2DP media is ready");
            if (this->play_requested_.load()) {
              this->pending_action_.store(PendingAction::START);
            } else {
              this->media_state_.store(MediaState::IDLE);
            }
          } else {
            ESP_LOGW(TAG, "A2DP media not ready (status %d)", param->media_ctrl_stat.status);
            this->request_media_retry_();
          }
          break;
        case ESP_A2D_MEDIA_CTRL_START:
          if (param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGI(TAG, "A2DP media started");
            this->media_state_.store(MediaState::STARTED);
          } else {
            ESP_LOGW(TAG, "A2DP media start failed (status %d)", param->media_ctrl_stat.status);
            this->request_media_retry_();
          }
          break;
        case ESP_A2D_MEDIA_CTRL_SUSPEND:
          if (param->media_ctrl_stat.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGW(TAG, "A2DP media suspend failed (status %d)", param->media_ctrl_stat.status);
          }
          this->media_state_.store(MediaState::IDLE);
          this->play_requested_.store(false);
          this->finish_requested_.store(false);
          this->buffer_drained_.store(false);
          this->stats_report_requested_.store(true);
          break;
        default:
          break;
      }
      break;
    case ESP_A2D_AUDIO_STATE_EVT:
      ESP_LOGD(TAG, "A2DP audio state changed to %d", param->audio_stat.state);
      break;
    default:
      break;
  }
}

int32_t A2dpSourceSpeaker::audio_data_callback_(uint8_t *data, int32_t length) {
  if (instance_ == nullptr) {
    return 0;
  }
  return instance_->fill_audio_(data, length);
}

int32_t A2dpSourceSpeaker::fill_audio_(uint8_t *data, int32_t length) {
  if (length < 0) {
    return 0;
  }
  if (data == nullptr || length == 0) {
    return 0;
  }

  std::memset(data, 0, static_cast<size_t>(length));
  if (!this->play_requested_.load() || this->media_state_.load() != MediaState::STARTED ||
      this->audio_ring_buffer_ == nullptr) {
    return length;
  }

  const size_t output_frames = static_cast<size_t>(length) / OUTPUT_FRAME_BYTES;
  size_t input_frames_read = 0;
  size_t input_bytes_read = 0;

  if (this->source_channels_.load() == 2) {
    const size_t bytes_requested = output_frames * OUTPUT_FRAME_BYTES;
    const size_t bytes_read = this->audio_ring_buffer_->read(data, bytes_requested, 0);
    input_bytes_read = bytes_read;
    input_frames_read = bytes_read / OUTPUT_FRAME_BYTES;
  } else {
    int16_t mono_samples[MONO_SCRATCH_SAMPLES];
    auto *stereo_samples = reinterpret_cast<int16_t *>(data);
    size_t output_frame_offset = 0;
    while (output_frame_offset < output_frames) {
      const size_t requested_frames = std::min(MONO_SCRATCH_SAMPLES, output_frames - output_frame_offset);
      const size_t bytes_read =
          this->audio_ring_buffer_->read(mono_samples, requested_frames * sizeof(int16_t), 0);
      input_bytes_read += bytes_read;
      const size_t frames_read = bytes_read / sizeof(int16_t);
      for (size_t i = 0; i < frames_read; i++) {
        stereo_samples[(output_frame_offset + i) * 2] = mono_samples[i];
        stereo_samples[(output_frame_offset + i) * 2 + 1] = mono_samples[i];
      }
      input_frames_read += frames_read;
      output_frame_offset += frames_read;
      if (frames_read < requested_frames) {
        break;
      }
    }
  }

  const int64_t callback_us = esp_timer_get_time();
  int64_t expected_first_callback_us = 0;
  this->stats_first_callback_us_.compare_exchange_strong(expected_first_callback_us, callback_us);
  this->stats_last_callback_us_.store(callback_us);
  this->stats_callback_count_.fetch_add(1);
  this->stats_input_bytes_consumed_.fetch_add(input_bytes_read);
  this->stats_output_bytes_requested_.fetch_add(static_cast<uint64_t>(length));
  const size_t output_audio_bytes = input_frames_read * OUTPUT_FRAME_BYTES;
  this->stats_silence_bytes_.fetch_add(static_cast<uint64_t>(length) - output_audio_bytes);

  this->apply_volume_(reinterpret_cast<int16_t *>(data), input_frames_read * 2);
  if (input_frames_read > 0) {
    this->audio_output_callback_(input_frames_read, callback_us);
  }
  if (this->finish_requested_.load() && this->audio_ring_buffer_->available() == 0) {
    this->buffer_drained_.store(true);
  }
  return length;
}

void A2dpSourceSpeaker::reset_playback_stats_() {
  this->stats_bytes_enqueued_.store(0);
  this->stats_input_bytes_consumed_.store(0);
  this->stats_output_bytes_requested_.store(0);
  this->stats_silence_bytes_.store(0);
  this->stats_callback_count_.store(0);
  this->stats_first_callback_us_.store(0);
  this->stats_last_callback_us_.store(0);
  this->stats_report_requested_.store(false);
}

void A2dpSourceSpeaker::report_playback_stats_() {
  const int64_t first_callback_us = this->stats_first_callback_us_.load();
  const int64_t last_callback_us = this->stats_last_callback_us_.load();
  const uint64_t callback_span_ms =
      last_callback_us > first_callback_us ? static_cast<uint64_t>(last_callback_us - first_callback_us) / 1000 : 0;
  ESP_LOGI(TAG,
           "Playback stats: input=%" PRIu64 " bytes, consumed=%" PRIu64 " bytes, requested=%" PRIu64
           " output bytes, silence=%" PRIu64 " bytes, callbacks=%" PRIu32 ", callback span=%" PRIu64 " ms",
           this->stats_bytes_enqueued_.load(), this->stats_input_bytes_consumed_.load(),
           this->stats_output_bytes_requested_.load(), this->stats_silence_bytes_.load(),
           this->stats_callback_count_.load(), callback_span_ms);
}

void A2dpSourceSpeaker::apply_volume_(int16_t *samples, size_t sample_count) const {
  const int32_t factor = this->muted_.load() ? 0 : this->volume_q15_.load();
  if (factor >= 32767) {
    return;
  }
  for (size_t i = 0; i < sample_count; i++) {
    samples[i] = static_cast<int16_t>((static_cast<int32_t>(samples[i]) * factor) >> 15);
  }
}

}  // namespace esphome::a2dp_source
