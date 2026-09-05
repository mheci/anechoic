#include "anechoic/SpeechEnhancer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

#include <rnnoise.h>

namespace anechoic {

namespace {

// Preallocated ring and staging sizes. Chosen so a 1 s host callback plus the
// maximum rewind window never heap-allocates on the audio thread.
constexpr std::size_t kMaxBufferedFrames = 256;
constexpr std::size_t kMaxPendingSamples = 96000;

// Gain-envelope time constants, in samples. The gate closes (fades to the
// comfort floor) noticeably faster than it reopens (8 ms), which makes state
// transitions click-free without pumping the background.
constexpr float kCloseTauSamples = 4.0f * kEngineRateHz / 1000.0f;   // 192
constexpr float kOpenTauSamples = 8.0f * kEngineRateHz / 1000.0f;    // 384

constexpr float kS16FullScale = 32768.0f;

float toLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

std::uint32_t normalizeChannelCount(std::uint32_t channels) {
    return std::min(std::max(channels, std::uint32_t{1}), kMaxChannels);
}

}  // namespace

// RNNoise needs its input scaled to 16-bit headroom and reports a probability
// of voice per processed frame. The scaling is folded into this engine so the
// rest of the chain never sees integer cast rounding.
namespace {

class RnnoiseEngine final : public Engine {
public:
    RnnoiseEngine() : m_state(rnnoise_create(nullptr), &rnnoise_destroy) {}

    float render(const float* input, float* output) override {
        std::array<float, kFrameSamples> boosted{};
        for (std::size_t i = 0; i < kFrameSamples; ++i) {
            boosted[i] = input[i] * kS16FullScale;
        }
        const float probability = rnnoise_process_frame(m_state.get(), output, boosted.data());
        for (std::size_t i = 0; i < kFrameSamples; ++i) {
            output[i] /= kS16FullScale;
        }
        return probability;
    }

    std::unique_ptr<Engine> clone() const override {
        return std::make_unique<RnnoiseEngine>();
    }

private:
    std::unique_ptr<DenoiseState, void (*)(DenoiseState*)> m_state;
};

}  // namespace

std::unique_ptr<Engine> makeDefaultEngine() {
    return std::make_unique<RnnoiseEngine>();
}

struct Suppressor::Impl {
    explicit Impl(std::uint32_t channels)
        : engines(channels),
          frames(channels, std::vector<std::array<float, kFrameSamples>>(kMaxBufferedFrames)),
          pending(channels),
          metas(kMaxBufferedFrames),
          gain(channels, 1.0f) {
        for (auto& lane : pending) {
            lane.reserve(kMaxPendingSamples);
        }
    }

    // A buffered frame plus the gate decision reached for it.
    struct Frame {
        std::uint64_t slot = 0;
        float maxVad = 0.0f;
        bool open = false;
        bool rewindCounted = false;
    };

    std::vector<std::unique_ptr<Engine>> engines;
    std::vector<std::vector<std::array<float, kFrameSamples>>> frames;  // [ch][ring]
    std::vector<std::vector<float>> pending;
    std::vector<Frame> metas;  // ring of kMaxBufferedFrames

    std::size_t metaHead = 0;
    std::size_t metaCount = 0;

    std::uint64_t nextSlot = 0;
    std::uint64_t lastVoiceSlot = 0;
    std::uint64_t lastConfirmSlot = 0;
    bool voiceLatched = false;
    bool haveVoice = false;  // hold must not fire before the first confirmation

    std::size_t headSpent = 0;                      // samples already emitted out of the head frame
    std::vector<float> gain;                        // per-channel output envelope state

    std::atomic<std::uint64_t> holdBlocks{0};
    std::atomic<std::uint64_t> rewindBlocks{0};
    std::atomic<std::uint64_t> latchBlocks{0};
    std::atomic<std::uint32_t> backlogBlocks{0};
    std::atomic<std::uint64_t> paddedSamples{0};
    std::atomic<float> voiceProbability{0.0f};

    std::size_t ringIndex(std::size_t logical) const {
        return (metaHead + logical) % kMaxBufferedFrames;
    }

    Frame& metaAt(std::size_t logical) { return metas[ringIndex(logical)]; }
    const Frame& metaAt(std::size_t logical) const { return metas[ringIndex(logical)]; }

    std::array<float, kFrameSamples>& frameAt(std::uint32_t ch, std::size_t logical) {
        return frames[ch][ringIndex(logical)];
    }

    std::size_t bufferedSamples() const {
        return metaCount * kFrameSamples - headSpent;
    }

    void popFront() {
        metaHead = (metaHead + 1) % kMaxBufferedFrames;
        if (metaCount > 0) {
            --metaCount;
        }
    }
};

Suppressor::Suppressor(std::uint32_t channels)
    : Suppressor(channels, makeDefaultEngine()) {}

Suppressor::Suppressor(std::uint32_t channels, std::unique_ptr<Engine> engine)
    : m_impl(std::make_unique<Impl>(normalizeChannelCount(channels))) {
    for (std::uint32_t ch = 0; ch < m_impl->engines.size(); ++ch) {
        m_impl->engines[ch] = engine->clone();
    }
}

Suppressor::~Suppressor() = default;

void Suppressor::process(const float* const* in, float** out, std::size_t sampleFrames,
                         const GateSettings& settings) {
    if (sampleFrames == 0) {
        return;
    }

    const std::uint32_t channels = static_cast<std::uint32_t>(m_impl->engines.size());

    // 1. Stage the incoming samples on each channel's pending buffer.
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        auto& lane = m_impl->pending[ch];
        if (lane.size() + sampleFrames > lane.capacity()) {
            // Fallback: keep working rather than drop audio if a host sends a
            // pathological block. Common-case callbacks stay within reserve.
            lane.reserve(lane.size() + sampleFrames);
        }
        lane.insert(lane.end(), in[ch], in[ch] + sampleFrames);
    }

    // 2. Render every newly completed frame on every channel and aggregate the
    //    strongest voice vote across channels for the shared gate decision.
    const std::size_t newFrames = m_impl->pending[0].size() / kFrameSamples;
    float lastVad = m_impl->voiceProbability.load(std::memory_order_relaxed);
    for (std::size_t f = 0; f < newFrames; ++f) {
        float maxVad = 0.0f;
        if (m_impl->metaCount == kMaxBufferedFrames) {
            m_impl->popFront();
            m_impl->headSpent = 0;
        }
        const std::size_t slotIndex = m_impl->ringIndex(m_impl->metaCount);
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            const float* frameIn = &m_impl->pending[ch][f * kFrameSamples];
            const float probability =
                m_impl->engines[ch]->render(frameIn, m_impl->frames[ch][slotIndex].data());
            maxVad = std::max(maxVad, probability);
        }
        m_impl->metas[slotIndex] = Impl::Frame{m_impl->nextSlot, maxVad, false, false};
        ++m_impl->metaCount;
        ++m_impl->nextSlot;
        lastVad = maxVad;
    }
    m_impl->voiceProbability.store(lastVad, std::memory_order_relaxed);

    // 3. Drop the consumed input from the pending buffers.
    if (newFrames > 0) {
        const std::size_t consumed = newFrames * kFrameSamples;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            auto& lane = m_impl->pending[ch];
            lane.erase(lane.begin(), lane.begin() + static_cast<std::ptrdiff_t>(consumed));
        }
    }

    // 4. Run the gate over the new frames, oldest first, so the latch and the
    //    hold period see the exact same sequential view a live stream has.
    const std::size_t decideFrom = m_impl->metaCount - newFrames;
    for (std::size_t f = decideFrom; f < m_impl->metaCount; ++f) {
        Impl::Frame& frame = m_impl->metaAt(f);

        if (frame.maxVad >= settings.threshold) {
            // Genuine voice. This is the only branch that renews the hold
            // window, so a burst of noise near the gate edge cannot keep it
            // latched open.
            m_impl->lastConfirmSlot = frame.slot;
            m_impl->lastVoiceSlot = frame.slot;
            m_impl->voiceLatched = true;
            m_impl->haveVoice = true;
            frame.open = true;
        } else {
            const float activeThreshold =
                m_impl->voiceLatched
                    ? std::max(0.0f, settings.threshold - settings.hysteresis)
                    : settings.threshold;
            if (frame.maxVad >= activeThreshold) {
                // Latch band: only reachable right after a confirmed voice
                // frame. The gate stays open, but the hold window is not
                // extended by these frames.
                m_impl->latchBlocks.fetch_add(1, std::memory_order_relaxed);
                m_impl->voiceLatched = true;
                frame.open = true;
            } else {
                m_impl->voiceLatched = false;
                if (m_impl->haveVoice &&
                    frame.slot >= m_impl->lastVoiceSlot &&
                    (frame.slot - m_impl->lastVoiceSlot) <= settings.holdBlocks) {
                    frame.open = true;
                    m_impl->holdBlocks.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    // 5. Retroactive rewind: once voice is confirmed, mute decisions inside the
    //    rewind window are undone so word onsets survive the gate.
    if (settings.rewindBlocks > 0 && m_impl->haveVoice) {
        for (std::size_t i = 0; i < m_impl->metaCount; ++i) {
            Impl::Frame& frame = m_impl->metaAt(i);
            if (!frame.open && frame.slot <= m_impl->lastConfirmSlot) {
                if ((m_impl->lastConfirmSlot - frame.slot) <= settings.rewindBlocks) {
                    frame.open = true;
                    if (!frame.rewindCounted) {
                        frame.rewindCounted = true;
                        m_impl->rewindBlocks.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    // 6. Emit samples that are no longer inside the rewind window. Partial
    //    fills pad the tail of this callback rather than replacing the whole
    //    block with silence, so the first host buffer is not a dropout.
    const std::size_t capacity = m_impl->bufferedSamples();
    const std::size_t holdBack = static_cast<std::size_t>(settings.rewindBlocks) * kFrameSamples;
    const std::size_t available = capacity > holdBack ? capacity - holdBack : 0;
    const std::size_t emit = std::min(sampleFrames, available);
    const float muteGain = toLinear(settings.muteGainDb);

    {
        const std::size_t totalConsumed = m_impl->headSpent + emit;
        const std::size_t popFronts = totalConsumed / kFrameSamples;
        const std::size_t remainder = totalConsumed % kFrameSamples;

        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            float* lane = out[ch];
            float& env = m_impl->gain[ch];
            std::size_t written = 0;
            for (std::size_t f = 0; f < popFronts + (remainder > 0 ? 1u : 0u) && written < emit; ++f) {
                const Impl::Frame& frame = m_impl->metaAt(f);
                const std::array<float, kFrameSamples>& buffer = m_impl->frameAt(ch, f);

                const float target = frame.open ? 1.0f : muteGain;
                const float tau = target > env ? kOpenTauSamples : kCloseTauSamples;
                const float coefficient = 1.0f - std::exp(-1.0f / tau);

                const std::size_t offset = f == 0 ? m_impl->headSpent : 0;
                const std::size_t take =
                    std::min(std::min(sampleFrames - written, emit - written),
                             kFrameSamples - offset);
                for (std::size_t i = 0; i < take; ++i) {
                    env += (target - env) * coefficient;
                    lane[written + i] = buffer[offset + i] * env;
                }
                written += take;
            }

            for (std::size_t i = written; i < sampleFrames; ++i) {
                lane[i] = 0.0f;
            }
        }

        for (std::size_t i = 0; i < popFronts; ++i) {
            m_impl->popFront();
        }
        m_impl->headSpent = remainder;

        if (emit < sampleFrames) {
            m_impl->paddedSamples.fetch_add(sampleFrames - emit, std::memory_order_relaxed);
        }
    }

    m_impl->backlogBlocks.store(static_cast<std::uint32_t>(m_impl->metaCount),
                                std::memory_order_relaxed);
}

void Suppressor::reset() {
    for (auto& lane : m_impl->pending) {
        lane.clear();
    }
    m_impl->metaHead = 0;
    m_impl->metaCount = 0;
    m_impl->nextSlot = 0;
    m_impl->lastVoiceSlot = 0;
    m_impl->lastConfirmSlot = 0;
    m_impl->voiceLatched = false;
    m_impl->haveVoice = false;
    m_impl->headSpent = 0;
    std::fill(m_impl->gain.begin(), m_impl->gain.end(), 1.0f);
    m_impl->holdBlocks.store(0, std::memory_order_relaxed);
    m_impl->rewindBlocks.store(0, std::memory_order_relaxed);
    m_impl->latchBlocks.store(0, std::memory_order_relaxed);
    m_impl->backlogBlocks.store(0, std::memory_order_relaxed);
    m_impl->paddedSamples.store(0, std::memory_order_relaxed);
    m_impl->voiceProbability.store(0.0f, std::memory_order_relaxed);
}

GateDiagnostics Suppressor::diagnostics() const {
    GateDiagnostics result;
    result.holdBlocks = m_impl->holdBlocks.load(std::memory_order_relaxed);
    result.rewindBlocks = m_impl->rewindBlocks.load(std::memory_order_relaxed);
    result.latchBlocks = m_impl->latchBlocks.load(std::memory_order_relaxed);
    result.backlogBlocks = m_impl->backlogBlocks.load(std::memory_order_relaxed);
    result.paddedSamples = m_impl->paddedSamples.load(std::memory_order_relaxed);
    result.voiceProbability = m_impl->voiceProbability.load(std::memory_order_relaxed);
    return result;
}

void Suppressor::clearDiagnostics() {
    m_impl->holdBlocks.store(0, std::memory_order_relaxed);
    m_impl->rewindBlocks.store(0, std::memory_order_relaxed);
    m_impl->latchBlocks.store(0, std::memory_order_relaxed);
    m_impl->paddedSamples.store(0, std::memory_order_relaxed);
    // backlogBlocks and voiceProbability are live state, not counters.
}

}  // namespace anechoic
