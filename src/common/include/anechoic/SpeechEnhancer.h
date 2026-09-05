#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

namespace anechoic {

// Fixed frame geometry shared by every host adapter and the enhancement core.
// One frame is 10 ms of audio at the engine rate.
constexpr std::size_t kFrameSamples = 480;
constexpr std::uint32_t kEngineRateHz = 48000;
constexpr std::uint32_t kMaxChannels = 8;

// Voice-gate control values.
//
// The enhancer produces a voice probability per frame. The gate below decides
// whether that frame reaches the output:
//
//   - a frame is kept when its probability is at or above `threshold`
//   - after the last such frame, output stays open for `holdBlocks` more frames
//   - a muted frame is reopened when a confirmed voice frame lies within
//     `rewindBlocks` of it (adds latency, restores word onsets)
//   - while voice stays active the effective threshold drops by `hysteresis`
//   - while the gate is closed the output is faded to `muteGainDb` instead of
//     hard digital zero, so the room ambience stays audible
struct GateSettings {
    float threshold = 0.55f;
    std::uint32_t holdBlocks = 20;
    std::uint32_t rewindBlocks = 0;
    float hysteresis = 0.0f;
    float muteGainDb = -40.0f;
};

// Diagnostics. Block counters count 10 ms frames; all values stay monotonic
// between calls to clearDiagnostics() except voiceProbability, which is the
// most recent aggregated VAD in [0, 1].
struct GateDiagnostics {
    std::uint64_t holdBlocks = 0;     // frames kept open by the hold period
    std::uint64_t rewindBlocks = 0;   // muted frames reopened by the rewind period
    std::uint64_t latchBlocks = 0;    // frames kept open by hysteresis latching
    std::uint32_t backlogBlocks = 0;  // buffered, not yet emitted frames (latency)
    std::uint64_t paddedSamples = 0;  // samples zero-filled at the end of a call
    float voiceProbability = 0.0f;    // last max-across-channels VAD in [0, 1]
};

// A single-frame enhancement pass. The engine is separate from the gate so a
// different network (e.g. DeepFilterNet) can be slotted in later without
// touching the gate, buffering or host code. Each channel runs its own engine
// instance; the factory clone() mirrors that.
class Engine {
public:
    virtual ~Engine() = default;

    // Consume `input` (kFrameSamples samples, normalised to [-1, 1]), write the
    // enhanced frame to `output` and return the voice probability in [0, 1].
    virtual float render(const float* input, float* output) = 0;

    // Independent copy of this engine, one per channel.
    virtual std::unique_ptr<Engine> clone() const = 0;
};

// Default engine backed by the vendored RNNoise v0.2 library.
std::unique_ptr<Engine> makeDefaultEngine();

// Multi-channel voice gate around an enhancement engine.
//
// All channels are suppressed together: a frame passes the gate when *any*
// channel reports voice, so a stereo pair never splits. Audio is buffered in
// frame units. Frames inside the rewind window are withheld so a later
// confirmation can reopen them; everything older is emitted, and a short call
// is padded at the tail rather than replacing the whole block with silence.
class Suppressor {
public:
    explicit Suppressor(std::uint32_t channels);
    // Test hook: substitute a custom engine for the RNNoise default. A clone
    // of `engine` is handed to every channel.
    Suppressor(std::uint32_t channels, std::unique_ptr<Engine> engine);
    ~Suppressor();

    Suppressor(const Suppressor&) = delete;
    Suppressor& operator=(const Suppressor&) = delete;

    // Drop all buffered state. The engines' own history is kept.
    void reset();

    // Process `sampleFrames` samples per channel. `in` and `out` must each
    // point at `channels` planar buffers. Settings are read once per call.
    void process(const float* const* in, float** out, std::size_t sampleFrames,
                 const GateSettings& settings);

    GateDiagnostics diagnostics() const;
    void clearDiagnostics();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace anechoic
