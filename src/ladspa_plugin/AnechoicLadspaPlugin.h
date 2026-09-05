#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>

#include "anechoic/SpeechEnhancer.h"
#include "ladspa++.h"

using namespace ladspa;

namespace port_info_custom {
    // Voice activation threshold, reported in percent so the port stays in a
    // human friendly integer range. Internally the value is divided by 100.
    constexpr static port_info_t vad_threshold_input = {
            "VAD Threshold (%)",
            "If probability of sound being a voice is lower than this threshold - silence will be returned",
            port_types::input | port_types::control,
            {
                    port_hints::bounded_below | port_hints::bounded_above | port_hints::integer |
                    port_hints::default_low,
                    0.f,
                    99.f
            }
    };
    // Post-voice hold period, in milliseconds. Converted to 10 ms frames in the
    // host-facing glue so the units match the DSP gate.
    constexpr static port_info_t vad_grace_period_input = {
            "VAD Grace Period (ms)",
            "For how long after the last voice detection the output won't be silenced. This helps when ends of words/sentences are being cut off.",
            port_types::input | port_types::control,
            {
                    port_hints::bounded_below | port_hints::bounded_above | port_hints::integer |
                    port_hints::default_low,
                    0.f,
                    1000.f
            }
    };
    // Retroactive (word-onset) grace, also in milliseconds. Adds latency.
    constexpr static port_info_t retroactive_vad_grace_input = {
            "Retroactive VAD Grace (ms)",
            "Similar to 'VAD Grace Period' but for starts of words/sentences. Warning, this introduces latency!",
            port_types::input | port_types::control,
            {
                    port_hints::bounded_below | port_hints::bounded_above | port_hints::integer |
                    port_hints::default_low,
                    0.f,
                    200.f
            }
    };
    // Hysteresis, in percent. Subtracted from the activation threshold while
    // voice was recently detected to stop the gate from flapping.
    constexpr static port_info_t vad_hysteresis_input = {
            "VAD Hysteresis (%)",
            "Amount subtracted from the voice activation threshold while a voice was recently "
            "detected. Reduces mouth clicks and threshold toggling.",
            port_types::input | port_types::control,
            {
                    port_hints::bounded_below | port_hints::bounded_above | port_hints::integer |
                    port_hints::default_low,
                    0.f,
                    50.f
            }
    };
    // Comfort noise floor in dB. -90 is effectively the old digital mute.
    constexpr static port_info_t comfort_noise_input = {
            "Comfort Noise Floor (dB)",
            "While silenced, output a denoised ambience at this level instead of hard digital "
            "silence. -90 is effectively the old digital mute.",
            port_types::input | port_types::control,
            {
                    port_hints::bounded_below | port_hints::bounded_above | port_hints::default_low,
                    -90.f,
                    -30.f
            }
    };
}

// Shared glue between the mono and stereo LADSPA instances: translates the
// human-facing port values into GateSettings and hands the block to the core.
class SuppressorInstance {
public:
    struct Controls {
        float thresholdPercent;
        uint32_t graceMs;
        uint32_t retroactiveMs;
        float hysteresisPercent;
        float comfortNoiseDb;
    };

    SuppressorInstance(uint32_t channels, sample_rate_t sampleRate)
        : m_suppressor(channels), m_channels(channels), m_sampleRate(sampleRate) {}

    void run(const float* const* in, float** out, sample_size_t sampleCount,
             const Controls& controls) {
        if (m_sampleRate != anechoic::kEngineRateHz) {
            for (uint32_t ch = 0; ch < m_channels; ++ch) {
                if (in[ch] != out[ch]) {
                    std::copy(in[ch], in[ch] + sampleCount, out[ch]);
                }
            }
            return;
        }
        anechoic::GateSettings settings;
        settings.threshold = std::min(controls.thresholdPercent / 100.0f, 0.99f);
        settings.holdBlocks = controls.graceMs / 10u;
        settings.rewindBlocks = controls.retroactiveMs / 10u;
        settings.hysteresis = std::min(controls.hysteresisPercent / 100.0f, 0.5f);
        settings.muteGainDb = controls.comfortNoiseDb;
        m_suppressor.process(in, out, sampleCount, settings);
    }

private:
    anechoic::Suppressor m_suppressor;
    uint32_t m_channels;
    sample_rate_t m_sampleRate;
};

struct AnechoicMono : private SuppressorInstance {
    enum class port_names {
        in_1,
        out_1,
        in_vad_threshold,
        in_vad_grace_period,
        in_retroactive_vad_grace,
        in_vad_hysteresis,
        in_comfort_noise,
        size
    };

    static constexpr port_info_t port_info[] =
            {
                    port_info_common::audio_input,
                    port_info_common::audio_output,
                    port_info_custom::vad_threshold_input,
                    port_info_custom::vad_grace_period_input,
                    port_info_custom::retroactive_vad_grace_input,
                    port_info_custom::vad_hysteresis_input,
                    port_info_custom::comfort_noise_input,
                    port_info_common::final_port
            };

    static constexpr info_t info =
            {
                    9354877, // unique id (kept for PipeWire / PulseAudio compatibility)
                    "noise_suppressor_mono",
                    properties::realtime,
                    "Anechoic Noise Suppression (Mono)",
                    "mheci",
                    "Removes a wide range of noises from voice in real time, based on Xiph's RNNoise library.",
                    {"voice", "noise suppression", "de-noise"},
                    strings::copyright::gpl3,
                    nullptr // implementation data
            };

    explicit AnechoicMono(sample_rate_t rate) : SuppressorInstance(1, rate) {}

    void run(port_array_t<port_names, port_info>& ports) {
        const_buffer in_buffer = ports.get<port_names::in_1>();
        buffer out_buffer = ports.get<port_names::out_1>();

        Controls controls;
        controls.thresholdPercent = ports.get<port_names::in_vad_threshold>();
        controls.graceMs = static_cast<uint32_t>(ports.get<port_names::in_vad_grace_period>());
        controls.retroactiveMs = static_cast<uint32_t>(ports.get<port_names::in_retroactive_vad_grace>());
        controls.hysteresisPercent = ports.get<port_names::in_vad_hysteresis>();
        controls.comfortNoiseDb = ports.get<port_names::in_comfort_noise>();

        const float* in[] = {in_buffer.data()};
        float* out[] = {out_buffer.data()};
        SuppressorInstance::run(in, out, in_buffer.size(), controls);
    }
};

struct AnechoicStereo : private SuppressorInstance {
    enum class port_names {
        in_1,
        in_r,
        out_1,
        out_r,
        in_vad_threshold,
        in_vad_grace_period,
        in_retroactive_vad_grace,
        in_vad_hysteresis,
        in_comfort_noise,
        size
    };

    static constexpr port_info_t port_info[] =
            {
                    port_info_common::audio_input_l,
                    port_info_common::audio_input_r,
                    port_info_common::audio_output_l,
                    port_info_common::audio_output_r,
                    port_info_custom::vad_threshold_input,
                    port_info_custom::vad_grace_period_input,
                    port_info_custom::retroactive_vad_grace_input,
                    port_info_custom::vad_hysteresis_input,
                    port_info_custom::comfort_noise_input,
                    port_info_common::final_port
            };

    static constexpr info_t info =
            {
                    9354878, // distinct from mono 9354877; LADSPA unique ids must not collide
                    "noise_suppressor_stereo",
                    properties::realtime,
                    "Anechoic Noise Suppression (Stereo)",
                    "mheci",
                    "Removes a wide range of noises from voice in real time, based on Xiph's RNNoise library.",
                    {"voice", "noise suppression", "de-noise"},
                    strings::copyright::gpl3,
                    nullptr // implementation data
            };

    explicit AnechoicStereo(sample_rate_t rate) : SuppressorInstance(2, rate) {}

    void run(port_array_t<port_names, port_info>& ports) {
        const_buffer in_l = ports.get<port_names::in_1>();
        const_buffer in_r = ports.get<port_names::in_r>();
        buffer out_l = ports.get<port_names::out_1>();
        buffer out_r = ports.get<port_names::out_r>();

        Controls controls;
        controls.thresholdPercent = ports.get<port_names::in_vad_threshold>();
        controls.graceMs = static_cast<uint32_t>(ports.get<port_names::in_vad_grace_period>());
        controls.retroactiveMs = static_cast<uint32_t>(ports.get<port_names::in_retroactive_vad_grace>());
        controls.hysteresisPercent = ports.get<port_names::in_vad_hysteresis>();
        controls.comfortNoiseDb = ports.get<port_names::in_comfort_noise>();

        const float* in[] = {in_l.data(), in_r.data()};
        float* out[] = {out_l.data(), out_r.data()};
        SuppressorInstance::run(in, out, in_l.size(), controls);
    }
};