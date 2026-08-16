# ESPHome A2DP Proxy

Turn a dedicated Bluetooth Classic speaker into a Home Assistant
`media_player` by placing an ESP32 near it. Home Assistant sends media to the
ESP32 over Wi-Fi; the ESP32 decodes it through ESPHome and transmits 44.1 kHz
stereo audio to the speaker as an A2DP source.

This is not ESPHome's BLE Bluetooth Proxy and it does not forward arbitrary
Bluetooth traffic. One ESP32 is dedicated to one A2DP speaker.

## Current scope

The first version supports:

- Home Assistant `media_player.play_media`
- stop, including an actual A2DP media suspend
- software volume and mute
- automatic connection and reconnection to one configured speaker
- SSP pairing and legacy PIN `0000`

It has been built and run with ESPHome 2026.7.4 and ESP-IDF 5.5.5 on an
ESP32-DevKitC-VE carrying an ESP32-WROVER-E module. Playback, stop, volume,
TTS, reconnect after boot, and a complete 20-second MP3 were tested with a
CLS-02 speaker.

## Hardware requirements

- An original ESP32 with Bluetooth Classic and PSRAM
- Currently tested: ESP32-DevKitC-VE / ESP32-WROVER-E, 8 MB flash
- A Bluetooth Classic A2DP speaker
- A 2.4 GHz Wi-Fi network reachable by Home Assistant

ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6 and other BLE-only chips cannot be used:
they do not provide the Bluetooth Classic A2DP source required here. Other
classic ESP32 boards with PSRAM may work but are not yet tested.

## Quick start

1. Copy [`examples/esp32-devkitc-ve.yaml`](examples/esp32-devkitc-ve.yaml) to
   your ESPHome configuration directory.
2. Replace `AA:BB:CC:DD:EE:FF` with the Bluetooth address of your speaker.
3. Add `wifi_ssid`, `wifi_password`, `api_encryption_key`, and `ota_password`
   to ESPHome's `secrets.yaml`.
4. Put the speaker into pairing mode, then install the firmware over USB.
5. Add the discovered ESPHome device to Home Assistant.
6. Call `media_player.play_media`, use TTS, or control it from the media player
   card.

The component connects to the configured address automatically. On the first
boot, leave the speaker in pairing mode until the log reports both pairing and
A2DP connection success.

## Configuration

The component is loaded directly from this repository:

```yaml
external_components:
  - source: github://Khronos31/esphome-a2dp-proxy@main
    components: [a2dp_source]
```

The output speaker accepts 16-bit PCM at 44.1 kHz, mono or stereo. Use
ESPHome's resampler in front of it so media with another sample rate is
converted automatically. The complete wiring is in the example.

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
- Wi-Fi and Bluetooth share one 2.4 GHz radio. A 44.1 kHz stereo PCM WAV needs
  about 176 kB/s and may underrun during direct HTTP playback. Compressed media
  such as MP3 and normal Home Assistant TTS are the intended inputs.

## Implementation and provenance

The component is a project-owned backend built directly on the public ESP-IDF
Bluetooth Classic GAP and A2DP source APIs. It uses ESPHome's `Speaker` and
ring-buffer interfaces and does not include or depend on the separately
evaluated `ESP32-A2DP` Arduino library.

The ESP-IDF A2DP callback pulls PCM from the component's buffer. `start()` and
`stop()` drive `ESP_A2D_MEDIA_CTRL_START` and `ESP_A2D_MEDIA_CTRL_SUSPEND`, so
stopping playback suspends the media stream instead of sending silence forever.

## License

MIT
