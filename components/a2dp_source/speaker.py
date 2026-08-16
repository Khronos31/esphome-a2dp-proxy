# SPDX-License-Identifier: MIT

import re

import esphome.codegen as cg
from esphome.components import audio, esp32, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_BUFFER_DURATION,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
)

CONF_TARGET_ADDRESS = "target_address"
CONF_DEVICE_NAME = "device_name"

a2dp_source_ns = cg.esphome_ns.namespace("a2dp_source")
A2dpSourceSpeaker = a2dp_source_ns.class_(
    "A2dpSourceSpeaker", cg.Component, speaker.Speaker
)

AUTO_LOAD = ["audio", "ring_buffer"]
DEPENDENCIES = ["esp32"]


def _validate_bt_address(value):
    value = cv.string_strict(value)
    if not re.fullmatch(r"(?i)[0-9a-f]{2}(?::[0-9a-f]{2}){5}", value):
        raise cv.Invalid(
            "target_address must be a Bluetooth address such as AA:BB:CC:DD:EE:FF"
        )
    return value.upper()


def _set_stream_limits(config):
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=1,
        max_channels=2,
        min_sample_rate=44100,
        max_sample_rate=44100,
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(A2dpSourceSpeaker),
            cv.Required(CONF_TARGET_ADDRESS): _validate_bt_address,
            cv.Optional(CONF_DEVICE_NAME, default="ESPHome A2DP Proxy"): cv.string_strict,
            cv.Optional(
                CONF_BUFFER_DURATION, default="500ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_BITS_PER_SAMPLE, default=16): cv.one_of(16, int=True),
            cv.Optional(CONF_NUM_CHANNELS, default=2): cv.int_range(min=1, max=2),
            cv.Optional(CONF_SAMPLE_RATE, default=44100): cv.one_of(44100, int=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _set_stream_limits,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    cg.add(var.set_target_address(config[CONF_TARGET_ADDRESS]))
    cg.add(var.set_device_name(config[CONF_DEVICE_NAME]))
    cg.add(var.set_buffer_duration(config[CONF_BUFFER_DURATION]))

    esp32.request_bluetooth()
    esp32.request_software_coexistence()
    esp32.add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BT_CLASSIC_ENABLED", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BT_A2DP_ENABLE", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BT_BLE_ENABLED", False)
    esp32.add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BLE_ONLY", False)
    esp32.add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BTDM", False)
