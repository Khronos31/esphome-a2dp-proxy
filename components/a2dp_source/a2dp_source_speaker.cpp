// SPDX-License-Identifier: MIT
#include "a2dp_source_speaker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_a2dp_legacy_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace {

static constexpr int A2DP_HTTP_TX_BUFFER_SIZE = 4096;
static constexpr int ESPHOME_AUDIO_HTTP_RX_BUFFER_SIZE = 2048;
static constexpr uint8_t A2DP_DEFAULT_MAX_BITPOOL = 53;
static const char *const HTTP_COMPAT_TAG = "a2dp_source.http";
static const char *const SBC_COMPAT_TAG = "a2dp_source.sbc";

// Private ESP-IDF Bluedroid type used by bta_av_co_audio_get_sbc_config().
// Keep this local mirror byte-for-byte compatible with tA2D_SBC_CIE so the
// linker wrapper does not require ESP-IDF's non-public Bluetooth headers.
struct A2dpSbcCodecConfig {
  uint8_t sample_frequency;
  uint8_t channel_mode;
  uint8_t block_length;
  uint8_t subbands;
  uint8_t allocation_method;
  uint8_t max_bitpool;
  uint8_t min_bitpool;
};

static std::atomic<uint8_t> a2dp_max_bitpool{A2DP_DEFAULT_MAX_BITPOOL};

}  // namespace

// ESPHome 2026.7 configures the audio reader's receive buffer but leaves its
// transmit buffer at ESP-IDF's 512-byte default. Signed Home Assistant My
// Media URLs can exceed that in the HTTP request line. The linker wrapper
// recognizes AudioReader's exact configuration and raises only its default
// transmit buffer. Other ESP-IDF HTTP users are left untouched.
// esp_http_client_init copies the structure before returning, so the adjusted
// stack copy is safe here.
extern "C" esp_http_client_handle_t __real_esp_http_client_init(const esp_http_client_config_t *config);

extern "C" esp_http_client_handle_t __wrap_esp_http_client_init(const esp_http_client_config_t *config) {
  const bool is_esphome_audio_reader = config != nullptr && config->buffer_size_tx == 0 &&
                                      config->buffer_size == ESPHOME_AUDIO_HTTP_RX_BUFFER_SIZE &&
                                      config->keep_alive_enable && config->event_handler != nullptr &&
                                      config->user_data != nullptr;
  if (!is_esphome_audio_reader) {
    return __real_esp_http_client_init(config);
  }

  const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  esp_http_client_config_t adjusted_config = *config;
  adjusted_config.buffer_size_tx = A2DP_HTTP_TX_BUFFER_SIZE;
  auto client = __real_esp_http_client_init(&adjusted_config);
  ESP_LOGD(HTTP_COMPAT_TAG, "Audio HTTP TX buffer: %d bytes; internal heap free/largest: %u/%u -> %u/%u",
           A2DP_HTTP_TX_BUFFER_SIZE, static_cast<unsigned>(free_before), static_cast<unsigned>(largest_before),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  return client;
}

extern "C" uint8_t __real_bta_av_co_audio_get_sbc_config(A2dpSbcCodecConfig *config, uint16_t *minimum_mtu);

extern "C" uint8_t __wrap_bta_av_co_audio_get_sbc_config(A2dpSbcCodecConfig *config, uint16_t *minimum_mtu) {
  const uint8_t result = __real_bta_av_co_audio_get_sbc_config(config, minimum_mtu);
  if (result == 0 || config == nullptr) {
    return result;
  }

  const uint8_t requested_max = a2dp_max_bitpool.load();
  const uint8_t original_max = config->max_bitpool;
  config->max_bitpool = std::max(config->min_bitpool, std::min(original_max, requested_max));
  ESP_LOGD(SBC_COMPAT_TAG, "SBC bitpool range: %u-%u (negotiated max %u)", config->min_bitpool,
           config->max_bitpool, original_max);
  return result;
}

namespace esphome::a2dp_source {

static const char *const TAG = "a2dp_source";
static constexpr uint32_t RECONNECT_DELAY_MS = 5000;
static constexpr uint32_t MEDIA_RETRY_DELAY_MS = 1000;
static constexpr uint32_t SAMPLE_RATE = 44100;
static constexpr size_t OUTPUT_FRAME_BYTES = sizeof(int16_t) * 2;
static constexpr size_t RESAMPLE_SCRATCH_FRAMES = 128;

A2dpSourceSpeaker *A2dpSourceSpeaker::instance_ = nullptr;

float A2dpSourceSpeaker::get_setup_priority() const { return setup_priority::HARDWARE; }

bool A2dpSourceSpeaker::is_a2dp_connected() const {
  return this->connection_state_.load() == ConnectionState::CONNECTED;
}

float A2dpSourceSpeaker::get_buffer_level_ms() const {
  if (this->audio_ring_buffer_ == nullptr) {
    return 0.0f;
  }
  return static_cast<float>(this->audio_ring_buffer_->available()) * 1000.0f /
         static_cast<float>(SAMPLE_RATE * OUTPUT_FRAME_BYTES);
}

float A2dpSourceSpeaker::take_recent_silence_percent() {
  const uint64_t requested = this->diagnostic_requested_bytes_.exchange(0);
  const uint64_t silence = this->diagnostic_silence_bytes_.exchange(0);
  if (requested == 0) {
    return NAN;
  }
  // The callback updates the two counters independently. Clamp the diagnostic
  // snapshot so a read between those updates cannot briefly report >100%.
  return static_cast<float>(std::min(silence, requested)) * 100.0f /
         static_cast<float>(requested);
}

void A2dpSourceSpeaker::setup() {
  if (instance_ != nullptr) {
    ESP_LOGE(TAG, "Only one A2DP source speaker is supported");
    this->mark_failed();
    return;
  }
  instance_ = this;
  a2dp_max_bitpool.store(this->max_bitpool_);

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
                "  PCM input: 16-bit, mono/stereo, 8000-48000 Hz\n"
                "  A2DP output: 16-bit, stereo, 44100 Hz\n"
                "  SBC maximum bitpool: %u\n"
                "  Buffer duration: %" PRIu32 " ms\n"
                "  PCM encoder boundary: ESP-IDF legacy source callback",
                this->target_address_.c_str(), this->device_name_.c_str(), this->max_bitpool_,
                this->buffer_duration_ms_);
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
  const uint8_t channels = this->audio_stream_info_.get_channels();
  const uint32_t sample_rate = this->audio_stream_info_.get_sample_rate();
  if (this->play_requested_.load() &&
      (channels != this->source_channels_.load() || sample_rate != this->source_sample_rate_.load())) {
    ESP_LOGE(TAG, "PCM format changed during playback");
    return 0;
  }

  if (this->state_ == speaker::STATE_STOPPED) {
    this->start();
  }
  if (!this->play_requested_.load()) {
    vTaskDelay(ticks_to_wait);
    return 0;
  }
  size_t bytes_consumed = 0;
  if (sample_rate == SAMPLE_RATE && channels == 2) {
    // Preserve the cheap passthrough path for the native A2DP PCM format.
    bytes_consumed = this->audio_ring_buffer_->write_without_replacement(data, length, ticks_to_wait, true);
  } else {
    bytes_consumed = this->resample_and_enqueue_(data, length, ticks_to_wait);
  }
  this->stats_bytes_enqueued_.fetch_add(bytes_consumed);
  return bytes_consumed;
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
  this->source_sample_rate_.store(this->audio_stream_info_.get_sample_rate());
  this->resample_next_output_numerator_ = 0;
  this->resample_previous_left_ = 0;
  this->resample_previous_right_ = 0;
  this->resample_has_previous_ = false;
  this->callback_source_frame_remainder_.store(0);
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
         this->audio_stream_info_.get_sample_rate() >= 8000 && this->audio_stream_info_.get_sample_rate() <= 48000;
}

size_t A2dpSourceSpeaker::resample_and_enqueue_(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  const uint8_t channels = this->source_channels_.load();
  const uint32_t source_rate = this->source_sample_rate_.load();
  const size_t input_frame_bytes = sizeof(int16_t) * channels;
  const size_t input_frames = length / input_frame_bytes;
  size_t committed_input_frames = 0;

  while (committed_input_frames < input_frames) {
    std::array<int16_t, RESAMPLE_SCRATCH_FRAMES * 2> scratch{};
    size_t scratch_frames = 0;
    size_t candidate_input_frames = 0;

    bool local_has_previous = this->resample_has_previous_;
    int16_t local_previous_left = this->resample_previous_left_;
    int16_t local_previous_right = this->resample_previous_right_;
    uint32_t local_next_numerator = this->resample_next_output_numerator_;

    size_t output_frame_limit = std::min(RESAMPLE_SCRATCH_FRAMES,
                                         std::max<size_t>(1, this->audio_ring_buffer_->free() / OUTPUT_FRAME_BYTES));

    while (committed_input_frames + candidate_input_frames < input_frames) {
      const size_t input_index = committed_input_frames + candidate_input_frames;
      int16_t current_left = 0;
      int16_t current_right = 0;
      std::memcpy(&current_left, data + input_index * input_frame_bytes, sizeof(current_left));
      if (channels == 2) {
        std::memcpy(&current_right, data + input_index * input_frame_bytes + sizeof(current_left),
                    sizeof(current_right));
      } else {
        current_right = current_left;
      }

      size_t outputs_for_frame = 1;
      if (local_has_previous) {
        outputs_for_frame = 0;
        uint32_t probe_numerator = local_next_numerator;
        while (probe_numerator <= SAMPLE_RATE) {
          outputs_for_frame++;
          probe_numerator += source_rate;
        }
      }

      if (scratch_frames + outputs_for_frame > output_frame_limit) {
        if (candidate_input_frames > 0) {
          break;
        }
        // One low-rate input frame can expand to several output frames. Wait for
        // that small group as a unit so input consumption and phase stay atomic.
        output_frame_limit = std::min(RESAMPLE_SCRATCH_FRAMES, outputs_for_frame);
      }

      if (!local_has_previous) {
        scratch[0] = current_left;
        scratch[1] = current_right;
        scratch_frames = 1;
        local_previous_left = current_left;
        local_previous_right = current_right;
        local_next_numerator = source_rate;
        local_has_previous = true;
      } else {
        while (local_next_numerator <= SAMPLE_RATE) {
          const int64_t inverse = static_cast<int64_t>(SAMPLE_RATE - local_next_numerator);
          const int64_t forward = static_cast<int64_t>(local_next_numerator);
          scratch[scratch_frames * 2] = static_cast<int16_t>(
              (static_cast<int64_t>(local_previous_left) * inverse + static_cast<int64_t>(current_left) * forward +
               SAMPLE_RATE / 2) /
              SAMPLE_RATE);
          scratch[scratch_frames * 2 + 1] = static_cast<int16_t>(
              (static_cast<int64_t>(local_previous_right) * inverse + static_cast<int64_t>(current_right) * forward +
               SAMPLE_RATE / 2) /
              SAMPLE_RATE);
          scratch_frames++;
          local_next_numerator += source_rate;
        }
        local_next_numerator -= SAMPLE_RATE;
        local_previous_left = current_left;
        local_previous_right = current_right;
      }
      candidate_input_frames++;

      if (scratch_frames >= output_frame_limit) {
        break;
      }
    }

    if (candidate_input_frames == 0) {
      break;
    }
    if (scratch_frames > 0 &&
        this->audio_ring_buffer_->write_without_replacement(scratch.data(), scratch_frames * OUTPUT_FRAME_BYTES,
                                                            ticks_to_wait, false) == 0) {
      break;
    }

    this->resample_has_previous_ = local_has_previous;
    this->resample_previous_left_ = local_previous_left;
    this->resample_previous_right_ = local_previous_right;
    this->resample_next_output_numerator_ = local_next_numerator;
    committed_input_frames += candidate_input_frames;
  }

  return committed_input_frames * input_frame_bytes;
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
  const size_t output_bytes_read = this->audio_ring_buffer_->read(data, output_frames * OUTPUT_FRAME_BYTES, 0);
  const size_t output_frames_read = output_bytes_read / OUTPUT_FRAME_BYTES;

  const int64_t callback_us = esp_timer_get_time();
  int64_t expected_first_callback_us = 0;
  this->stats_first_callback_us_.compare_exchange_strong(expected_first_callback_us, callback_us);
  this->stats_last_callback_us_.store(callback_us);
  this->stats_callback_count_.fetch_add(1);
  this->stats_input_bytes_consumed_.fetch_add(output_bytes_read);
  this->stats_output_bytes_requested_.fetch_add(static_cast<uint64_t>(length));
  const size_t output_audio_bytes = output_frames_read * OUTPUT_FRAME_BYTES;
  const uint64_t silence_bytes = static_cast<uint64_t>(length) - output_audio_bytes;
  this->stats_silence_bytes_.fetch_add(silence_bytes);
  this->diagnostic_requested_bytes_.fetch_add(static_cast<uint64_t>(length));
  this->diagnostic_silence_bytes_.fetch_add(silence_bytes);
  const bool underflow = silence_bytes > 0;
  if (underflow) {
    if (!this->diagnostic_underflow_active_.exchange(true)) {
      this->diagnostic_underflow_events_.fetch_add(1);
    }
  } else {
    this->diagnostic_underflow_active_.store(false);
  }

  this->apply_volume_(reinterpret_cast<int16_t *>(data), output_frames_read * 2);
  if (output_frames_read > 0) {
    const uint64_t source_frame_numerator =
        static_cast<uint64_t>(output_frames_read) * this->source_sample_rate_.load() +
        this->callback_source_frame_remainder_.load();
    const size_t source_frames = source_frame_numerator / SAMPLE_RATE;
    this->callback_source_frame_remainder_.store(source_frame_numerator % SAMPLE_RATE);
    if (source_frames > 0) {
      this->audio_output_callback_(source_frames, callback_us);
    }
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
  this->diagnostic_requested_bytes_.store(0);
  this->diagnostic_silence_bytes_.store(0);
  this->diagnostic_underflow_events_.store(0);
  this->diagnostic_underflow_active_.store(false);
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
