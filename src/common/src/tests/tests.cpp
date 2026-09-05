#define CATCH_CONFIG_MAIN
#include <catch.hpp>

#include "anechoic/SpeechEnhancer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

using namespace anechoic;

namespace {

// Deterministic stand-in for the enhancement engine. It passes the input
// through untouched and reports a voice probability derived from the first
// sample's magnitude, so tests can drive the gate with the signal level alone.
class ProbeEngine final : public Engine {
public:
    explicit ProbeEngine(std::function<float(const float*)> probe)
        : m_probe(std::move(probe)) {}

    float render(const float* input, float* output) override {
        std::copy(input, input + kFrameSamples, output);
        return m_probe(input);
    }

    std::unique_ptr<Engine> clone() const override {
        return std::make_unique<ProbeEngine>(m_probe);
    }

private:
    std::function<float(const float*)> m_probe;
};

std::unique_ptr<Engine> levelProbe() {
    return std::make_unique<ProbeEngine>([](const float* in) { return std::fabs(in[0]); });
}

struct Fixture {
    std::unique_ptr<Suppressor> unit;
    std::vector<float> input;
    std::vector<float> output;

    explicit Fixture(std::uint32_t channels, float seed = -1.0f) {
        const std::size_t capacity = kFrameSamples * 256;
        input.assign(capacity, seed);
        output.assign(capacity, seed);
        unit = std::make_unique<Suppressor>(channels, levelProbe());
    }

    // Fill `input` with `n` samples of constant `amplitude` starting at `offset`.
    void stamp(std::size_t offset, std::size_t n, float amplitude) {
        std::fill(input.begin() + offset, input.begin() + offset + n, amplitude);
    }

    // Process one call of `n` samples at offset `position`, with all output
    // lanes pre-filled with -1 so untouched lanes are detectable.
    void run(std::size_t position, std::size_t n, const GateSettings& settings, float* lane) {
        std::vector<const float*> inputs{ &input[position] };
        std::vector<float*> outputs{ lane };
        unit->process(inputs.data(), outputs.data(), n, settings);
    }
};

}  // namespace

TEST_CASE("Construction, reset and repeated cycles stay stable", "[core]") {
    for (std::uint32_t channels : {1u, 2u, 4u}) {
        Suppressor unit(channels);
        for (int i = 0; i < 8; ++i) {
            unit.reset();
            unit.clearDiagnostics();
        }
        REQUIRE(unit.diagnostics().backlogBlocks == 0);
    }
}

TEST_CASE("Reset keeps the suppressor usable", "[core]") {
    Fixture fx(1);
    GateSettings s;
    s.threshold = 0.5f;
    s.holdBlocks = 0;

    fx.stamp(0, kFrameSamples * 4, 0.8f);
    fx.run(0, kFrameSamples * 2, s, fx.output.data());
    fx.unit->reset();
    REQUIRE(fx.unit->diagnostics().backlogBlocks == 0);

    // A second cycle after reset must not crash and must emit audio again.
    std::fill(fx.output.begin(), fx.output.begin() + kFrameSamples * 2, -1.0f);
    fx.run(0, kFrameSamples * 2, s, fx.output.data());
    REQUIRE(std::fabs(fx.output[200]) > 0.5f);
}

TEST_CASE("Real-time calls emit available audio instead of dropping the block", "[core]") {
    Fixture fx(1);
    GateSettings s;
    s.threshold = 0.5f;
    s.holdBlocks = 0;

    fx.stamp(0, 1024, 0.8f);

    // 512-sample request: one rendered frame (480) is emitted and the tail is padded.
    fx.run(0, 512, s, fx.output.data());
    REQUIRE(std::fabs(fx.output[0]) > 0.5f);
    REQUIRE(std::fabs(fx.output[480]) < 1e-6f);
    REQUIRE(fx.unit->diagnostics().paddedSamples == 32);

    // Second call tops the remainder and emits more real audio.
    fx.run(512, 512, s, &fx.output[512]);
    REQUIRE(fx.output[512] != -1.0f);
}

TEST_CASE("A confirmed voice frame opens the gate at unity gain", "[core]") {
    Fixture fx(1);
    GateSettings s;
    s.threshold = 0.5f;

    fx.stamp(0, 960, 0.8f);  // two voice-level frames

    // Feed both frames in one call so the request is answered at once.
    fx.run(0, kFrameSamples * 2, s, fx.output.data());
    // Skip the leading envelope tail; the steady samples are passed through.
    for (std::size_t i = 200; i < kFrameSamples * 2; ++i) {
        REQUIRE(std::fabs(fx.output[i]) > 0.5f);
    }
}

TEST_CASE("Hold period keeps the gate open after the last voice frame", "[core]") {
    Fixture fx(1);
    GateSettings s;
    s.threshold = 0.5f;
    s.holdBlocks = 3;

    fx.stamp(0, kFrameSamples, 0.8f);                  // frame 0: voice
    fx.stamp(kFrameSamples, kFrameSamples * 8, 0.1f);  // frames 1..8: silence

    for (std::size_t f = 0; f < 9; ++f) {
        const std::size_t at = f * kFrameSamples;
        fx.run(at, kFrameSamples, s, &fx.output[at]);
    }

    // Frames 0..3 are open (voice + hold); the muting envelope has fully
    // settled well before frame 8, so its start sample is nearly silent.
    for (std::size_t f = 1; f <= 3; ++f) {
        REQUIRE(std::fabs(fx.output[f * kFrameSamples]) > 0.05f);
    }
    REQUIRE(std::fabs(fx.output[8 * kFrameSamples]) < 0.002f);

    REQUIRE(fx.unit->diagnostics().holdBlocks == 3);
}

TEST_CASE("Hold does not open before the first confirmed voice", "[core]") {
    Fixture fx(1);
    GateSettings s;
    s.threshold = 0.5f;
    s.holdBlocks = 20;
    s.muteGainDb = -90.0f;

    fx.stamp(0, kFrameSamples * 4, 0.1f);  // silence only

    fx.run(0, kFrameSamples * 4, s, fx.output.data());

    // Envelope starts at unity and closes over ~4 ms, so skip the fade and
    // require the later samples to sit at the mute floor.
    REQUIRE(std::fabs(fx.output[kFrameSamples * 2]) < 0.005f);
    REQUIRE(fx.unit->diagnostics().holdBlocks == 0);
}

TEST_CASE("Hysteresis latches the gate across a hovering probability", "[core]") {
    Fixture fx(1);
    GateSettings s;
    s.threshold = 0.5f;
    s.hysteresis = 0.1f;
    s.holdBlocks = 0;
    s.muteGainDb = -90.0f;

    fx.stamp(0, kFrameSamples, 0.55f);                 // frame 0: voice
    fx.stamp(kFrameSamples, kFrameSamples * 3, 0.46f); // hovering in the latch band
    fx.stamp(kFrameSamples * 4, kFrameSamples * 9, 0.3f);  // clearly below the latch threshold

    // One call answers once every frame is buffered. The latch holds frames
    // 1..3 open; everything from frame 4 on is muted and fades out.
    const std::size_t total = kFrameSamples * 13;
    fx.run(0, total, s, fx.output.data());

    for (std::size_t f = 1; f <= 3; ++f) {
        REQUIRE(std::fabs(fx.output[f * kFrameSamples]) > 0.3f);  // kept open by the latch
    }
    REQUIRE(std::fabs(fx.output[12 * kFrameSamples]) < 0.002f);   // latch released

    REQUIRE(fx.unit->diagnostics().latchBlocks == 3);
}

TEST_CASE("Rewind reopens muted frames that precede a confirmed voice", "[core]") {
    Fixture fx(1);
    GateSettings s;
    s.threshold = 0.5f;
    s.holdBlocks = 0;
    s.rewindBlocks = 2;

    fx.stamp(0, kFrameSamples, 0.8f);                  // frame 0: voice
    fx.stamp(kFrameSamples, kFrameSamples * 2, 0.1f);  // frames 1..2: mute
    fx.stamp(kFrameSamples * 3, kFrameSamples, 0.8f);  // frame 3: voice

    // The rewind window (request + 2 frames) keeps everything buffered until
    // call 2, so the muted frames are still buffered when frame 3 confirms.
    for (std::size_t f = 0; f < 4; ++f) {
        const std::size_t at = f * kFrameSamples;
        fx.run(at, kFrameSamples, s, &fx.output[at]);
    }

    // Frame 3's confirmation reopened muted frames 1..2 (still buffered), so
    // call 3 emits frame 1 at unity gain instead of the comfort floor.
    REQUIRE(std::fabs(fx.output[3 * kFrameSamples + 200]) > 0.05f);
    REQUIRE(fx.unit->diagnostics().rewindBlocks == 2);
}

TEST_CASE("Comfort floor replaces digital zero while the gate is closed", "[core]") {
    Fixture fx(1);
    GateSettings s;
    s.threshold = 0.5f;
    s.holdBlocks = 0;
    s.muteGainDb = -30.0f;  // linear ~0.0316

    // Voice first, then a long silence so the envelope settles.
    fx.stamp(0, kFrameSamples, 0.8f);
    fx.stamp(kFrameSamples, kFrameSamples * 12, 0.1f);

    const std::size_t total = kFrameSamples * 13;
    fx.run(0, total, s, fx.output.data());

    const float expected = 0.0316f * 0.1f;
    const std::size_t tail = kFrameSamples * 10;  // well past the ~4 ms close envelope
    REQUIRE(std::fabs(fx.output[tail]) > 0.001f);
    REQUIRE(std::fabs(fx.output[tail]) < expected * 1.5f);
}

TEST_CASE("A short leftover after framing is emitted and the tail is padded", "[core]") {
    Fixture fx(1);
    GateSettings s;
    s.threshold = 0.5f;
    s.holdBlocks = 0;
    s.muteGainDb = -90.0f;

    // 500 samples = one rendered frame (480) plus 20 leftover. The leftover
    // cannot be emitted yet, so the last 20 samples of the callback are zeros.
    fx.stamp(0, 500, 0.8f);
    fx.run(0, 500, s, fx.output.data());

    REQUIRE(fx.output[0] != -1.0f);
    REQUIRE(std::fabs(fx.output[480]) < 1e-6f);
    REQUIRE(fx.unit->diagnostics().paddedSamples == 20);
}

TEST_CASE("Channels share the gate and follow the strongest voice vote", "[core]") {
    Fixture fx(2);
    GateSettings s;
    s.threshold = 0.5f;

    // Left channel carries voice on frame 0, right channel stays quiet.
    fx.stamp(0, kFrameSamples, 0.8f);
    fx.stamp(kFrameSamples, kFrameSamples * 3, 0.1f);
    std::vector<float> rightIn(64 * kFrameSamples, 0.1f);
    std::vector<float> rightOut(64 * kFrameSamples, -1.0f);

    for (std::size_t f = 0; f < 4; ++f) {
        const std::size_t at = f * kFrameSamples;
        std::vector<const float*> inputs{ &fx.input[at], &rightIn[at] };
        std::vector<float*> outputs{ &fx.output[at], &rightOut[at] };
        fx.unit->process(inputs.data(), outputs.data(), kFrameSamples, s);
    }

    // Both lanes were written on the voice frame: the left with its own voice
    // content and the right at its own quieter level, not clipped to zero.
    REQUIRE(std::fabs(fx.output[20] - 0.8f) < 0.05f);
    REQUIRE(std::fabs(rightOut[20] - 0.1f) < 0.05f);
    REQUIRE(rightOut[20] != -1.0f);
    REQUIRE(fx.unit->diagnostics().paddedSamples == 0);
}

TEST_CASE("Backlog reports the frames withheld for the rewind window", "[core]") {
    Fixture fx(1);
    GateSettings s;
    s.threshold = 0.5f;
    s.rewindBlocks = 3;

    fx.stamp(0, kFrameSamples, 0.1f);
    // One voice frame is not enough to pay the 4-frame (request + rewind) debt.
    fx.run(0, kFrameSamples, s, fx.output.data());
    REQUIRE(fx.unit->diagnostics().backlogBlocks > 0);

    // In steady state a frame is emitted per call, so the backlog can never
    // fall below the rewind window itself: the buffer must keep it available
    // for retroactive reopening.
    for (std::size_t f = 1; f < 8; ++f) {
        fx.run(f * kFrameSamples, kFrameSamples, s, &fx.output[f * kFrameSamples]);
    }
    REQUIRE(fx.unit->diagnostics().backlogBlocks == s.rewindBlocks);

    // reset() also drops the withheld backlog.
    fx.unit->reset();
    REQUIRE(fx.unit->diagnostics().backlogBlocks == 0);
}

TEST_CASE("Diagnostics expose the latest voice probability", "[core]") {
    Fixture fx(1);
    GateSettings s;
    s.threshold = 0.5f;
    s.holdBlocks = 0;

    fx.stamp(0, kFrameSamples, 0.8f);
    fx.run(0, kFrameSamples, s, fx.output.data());
    REQUIRE(fx.unit->diagnostics().voiceProbability == Approx(0.8f).margin(1e-5f));
}
