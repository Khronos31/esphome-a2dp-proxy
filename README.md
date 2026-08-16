# ESPHome A2DP Proxy

Turn a dedicated Bluetooth Classic speaker into a Home Assistant
`media_player` by placing an original ESP32 near it. Home Assistant sends media
to the ESP32 over Wi-Fi; ESPHome decodes it, and this component transmits it to
the speaker as an A2DP source.

This is not ESPHome's BLE Bluetooth Proxy and it does not forward arbitrary
Bluetooth traffic. One ESP32 is dedicated to one A2DP speaker.

## Current scope

Version 0.1.0 supports:

- Home Assistant `media_player.play_media`
- stop, including an actual A2DP media suspend
- software volume and mute
- automatic connection and reconnection to one configured speaker
- SSP pairing and legacy PIN `0000`
- optional buffer, silence, underflow, connection, and Wi-Fi diagnostics

It has been built and run with ESPHome 2026.7.4 and ESP-IDF 5.5.5 on an
ESP32-DevKitC-VE carrying an ESP32-WROVER-E module. Playback, stop, volume,
TTS, reconnect after boot, local playback, streamed MP3 playback, and M4A/AAC
from Home Assistant My Media were tested with two Bluetooth speakers.

## Hardware requirements

- An original ESP32 with Bluetooth Classic and PSRAM
- Currently tested: ESP32-DevKitC-VE / ESP32-WROVER-E, 8 MB flash
- A Bluetooth Classic A2DP speaker
- A 2.4 GHz Wi-Fi network reachable by Home Assistant

ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6 and other BLE-only chips cannot be used:
they do not provide the Bluetooth Classic A2DP source required here. Other
classic ESP32 boards with PSRAM may work but are not yet tested.

## Quick start

1. Copy the complete YAML below to your ESPHome configuration directory.
2. Replace `AA:BB:CC:DD:EE:FF` with the Bluetooth Classic address of your
   speaker.
3. Add `wifi_ssid`, `wifi_password`, `api_encryption_key`, and `ota_password`
   to ESPHome's `secrets.yaml`.
4. Put the speaker into pairing mode, then install the firmware over USB.
5. Add the discovered ESPHome device to Home Assistant.
6. Call `media_player.play_media`, use TTS, or control it from the media-player
   card.

The same configuration is kept as
[`examples/esp32-devkitc-ve.yaml`](examples/esp32-devkitc-ve.yaml). CI requires
the file and this README block to remain identical.

<!-- BEGIN: esp32-devkitc-ve-example -->
```yaml
substitutions:
  name: a2dp-proxy
  friendly_name: A2DP Proxy
  # Replace this with your speaker's Bluetooth Classic address.
  target_address: "AA:BB:CC:DD:EE:FF"

esphome:
  name: ${name}
  friendly_name: ${friendly_name}
  min_version: 2026.7.4
  project:
    name: Khronos31.esphome-a2dp-proxy
    version: "0.1.0"

esp32:
  # ESP32-DevKitC-VE with an ESP32-WROVER-E module and 8 MB flash.
  board: esp-wrover-kit
  flash_size: 8MB
  framework:
    type: esp-idf
    advanced:
      minimum_chip_revision: "3.1"

psram:

external_components:
  - source: github://Khronos31/esphome-a2dp-proxy@v0.1.0
    components: [a2dp_source]

logger:
  logs:
    # The Classic Bluetooth stack can emit this tag at a very high rate while
    # streaming. API log forwarding then competes with real-time audio work.
    esp-idf: NONE

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  power_save_mode: none

api:
  encryption:
    key: !secret api_encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

speaker:
  - platform: a2dp_source
    id: a2dp_output
    target_address: ${target_address}
    # Reduces Bluetooth airtime enough for reliable simultaneous Wi-Fi input.
    max_bitpool: 32
    buffer_duration: 750ms

media_player:
  - platform: speaker
    id: a2dp_media_player
    name: Bluetooth Speaker
    buffer_size: 400000
    volume_initial: 50%
    announcement_pipeline:
      speaker: a2dp_output
      # Home Assistant transcodes media-source inputs (including M4A/AAC) to
      # this advertised format before the ESP32 downloads them.
      format: MP3
      sample_rate: 22050
      num_channels: 1
```
<!-- END: esp32-devkitc-ve-example -->

The component connects to the configured address automatically. On the first
boot, leave the speaker in pairing mode until the log reports both pairing and
A2DP connection success.

## Media format and audio quality

The output speaker accepts 16-bit mono or stereo PCM from 8 to 48 kHz. It uses
a lightweight fixed-point linear converter internally and feeds the legacy
ESP-IDF A2DP source at 44.1 kHz stereo.

The public example asks Home Assistant for MP3 at 22.05 kHz mono. That smaller
Wi-Fi stream was the stable setting in simultaneous Wi-Fi receive and Bluetooth
A2DP transmit testing. The component expands it to the 44.1 kHz stereo PCM
required by the A2DP source. This input setting can be changed in YAML, but
higher-rate stereo input costs considerably more shared-radio airtime and
decode work.

When content is selected through Home Assistant's media-source path, Home
Assistant transcodes inputs such as M4A/AAC to the advertised MP3 format before
the ESP32 downloads them. This keeps AAC decoding off the device. It does not
make arbitrary M4A HTTP URLs directly playable: direct URLs do not pass through
Home Assistant's transcoder and must already use a format compiled into ESPHome
(MP3, FLAC, WAV, or Ogg/Opus in ESPHome 2026.7).

`max_bitpool` controls the upper SBC bitpool advertised to ESP-IDF's encoder.
It defaults to `32`, the stable value found on the tested WROVER-E. The ESP-IDF
default of `53` produced severe wireless dropouts in that test. Values from `2`
to `53` are accepted, but lower is not automatically better: `24` made the
tested ESP-IDF encoder path starve its PCM callback.

Keep ESPHome's speaker media-player decode task in internal RAM. In particular,
do not add `task_stack_in_psram: true` to the `media_player` configuration. On
the original ESP32, a decode stack in PSRAM could not reliably keep pace while
Wi-Fi and Bluetooth Classic were active. Large file and PCM buffers still use
PSRAM automatically.

## Optional diagnostics

These sensors are intended for installation and RF troubleshooting. They are
not required for playback and can be removed after the path is stable.

```yaml
sensor:
  - platform: wifi_signal
    name: Wi-Fi RSSI
    update_interval: 5s
  - platform: template
    name: A2DP buffer level
    unit_of_measurement: ms
    update_interval: 5s
    lambda: return id(a2dp_output).get_buffer_level_ms();
  - platform: template
    name: A2DP recent silence
    unit_of_measurement: "%"
    update_interval: 5s
    lambda: return id(a2dp_output).take_recent_silence_percent();
  - platform: template
    name: A2DP underflow events
    accuracy_decimals: 0
    update_interval: 5s
    lambda: return id(a2dp_output).get_underflow_events();

binary_sensor:
  - platform: template
    name: A2DP connected
    lambda: return id(a2dp_output).is_a2dp_connected();

text_sensor:
  - platform: wifi_info
    ip_address:
      name: IP address
    bssid:
      name: Wi-Fi BSSID
```

`A2DP recent silence` reports the share of bytes for which ESP-IDF requested
audio but the component had no PCM available during the preceding update
interval. `A2DP underflow events` counts transitions into underflow during the
current playback.

## Important limitations

- The ESP32 connects at boot and keeps ownership of the speaker while idle.
  This is intended for a dedicated, preferably mains-powered speaker. A phone
  generally cannot use the same speaker until the ESP32 is powered off.
- Discovery is not implemented. Obtain the speaker's Bluetooth address with a
  phone or `bluetoothctl` and put it in YAML.
- Legacy pairing supports PIN `0000` only. Speakers requiring another fixed PIN
  are not yet supported.
- There is no Home Assistant control for deleting Bluetooth bonds. With a stale
  bond, recovery may require erasing the ESP32 flash and pairing again.
- Only one A2DP source instance is supported per ESP32.
- There is no pause, seek, metadata, microphone, HFP, LE Audio, synchronized
  playback, or Bluetooth proxying for unrelated devices.
- Wi-Fi and Bluetooth share one 2.4 GHz radio. Results still depend on Wi-Fi
  congestion, RF range, the speaker, and the exact ESP32 module.
- Live sources still depend on Home Assistant's upstream media proxy. During
  RTSP testing, `esphome.ffmpeg_proxy` could stop delivering bytes while the
  ESP32 Wi-Fi and A2DP links remained connected. Static media and the A2DP link
  are separate from that upstream failure mode.

## Implementation and provenance

The component is a project-owned backend built directly on the public ESP-IDF
Bluetooth Classic GAP and A2DP source APIs. It uses ESPHome's `Speaker` and
ring-buffer interfaces and does not include or depend on the separately
evaluated `ESP32-A2DP` Arduino library.

The ESP-IDF A2DP callback pulls PCM from the component's buffer. `start()` and
`stop()` drive `ESP_A2D_MEDIA_CTRL_START` and `ESP_A2D_MEDIA_CTRL_SUSPEND`, so
stopping playback suspends the media stream instead of sending silence forever.

Home Assistant My Media uses long signed URLs. ESPHome 2026.7 leaves the
ESP-IDF HTTP transmit buffer at 512 bytes, which is too small for some request
lines. The component links a narrow compatibility wrapper that raises
undersized HTTP transmit buffers to 4 KiB. It does not replace ESPHome's media
reader or HTTP implementation.

The legacy ESP-IDF A2DP source API hard-codes an SBC maximum bitpool of 53. A
second narrow linker wrapper caps the encoder's negotiated maximum at the
configured `max_bitpool`; it leaves the rest of ESP-IDF's codec negotiation
unchanged.

Releases are source-only. CI compiles the component against both the minimum
supported ESPHome version and the current ESPHome release, but generated
firmware is not published because it contains device-specific network,
authentication, and speaker settings.

## Versioning

`VERSION` is the canonical release version. Run
`python tools/release_version.py set X.Y.Z` when preparing a release. CI checks
the component version, ESPHome `project.version`, public example, README copy,
and `vX.Y.Z` Git tag for exact agreement.

## License

MIT
