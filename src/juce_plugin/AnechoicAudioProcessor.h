#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

namespace anechoic {
class Suppressor;
}

// The JUCE host adapter. Shares the audio bus between the DSP core and the
// host: one Suppressor is created per input bus layout in prepareToPlay.
class AnechoicAudioProcessor : public juce::AudioProcessor,
                               public juce::AudioProcessorValueTreeState::Listener {
public:
    AnechoicAudioProcessor();

    ~AnechoicAudioProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;

    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout &layouts) const override;

    void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;

    juce::AudioProcessorEditor *createEditor() override;

    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;

    bool producesMidi() const override;

    double getTailLengthSeconds() const override;

    int getNumPrograms() override;

    int getCurrentProgram() override;

    void setCurrentProgram(int index) override;

    const juce::String getProgramName(int index) override;

    void changeProgramName(int index, const juce::String &newName) override;

    void getStateInformation(juce::MemoryBlock &destData) override;

    void setStateInformation(const void *data, int sizeInBytes) override;

    void parameterChanged(const juce::String &parameterID, float newValue) override;

    // Snapshot of gate diagnostics for the editor. Updated on the audio thread;
    // the editor must not touch m_suppressor.
    struct UiSnapshot {
        std::uint64_t holdBlocks = 0;
        std::uint64_t rewindBlocks = 0;
        std::uint64_t latchBlocks = 0;
        std::uint32_t backlogBlocks = 0;
        std::uint64_t paddedSamples = 0;
        float voiceProbability = 0.0f;
        bool wrongSampleRate = false;
    };

    UiSnapshot uiSnapshot() const;

    juce::AudioProcessorValueTreeState m_parameters;

    juce::AudioParameterFloat *m_thresholdParam = nullptr;
    juce::AudioParameterInt *m_gracePeriodParam = nullptr;
    juce::AudioParameterInt *m_retroactiveGracePeriodParam = nullptr;
    juce::AudioParameterFloat *m_hysteresisParam = nullptr;
    juce::AudioParameterFloat *m_comfortNoiseDbParam = nullptr;

    std::unique_ptr<anechoic::Suppressor> m_suppressor;

private:
    void updateLatency();

    std::atomic<std::uint64_t> m_uiHold{0};
    std::atomic<std::uint64_t> m_uiRewind{0};
    std::atomic<std::uint64_t> m_uiLatch{0};
    std::atomic<std::uint32_t> m_uiBacklog{0};
    std::atomic<std::uint64_t> m_uiPadded{0};
    std::atomic<float> m_uiVad{0.0f};
    std::atomic<bool> m_wrongSampleRate{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnechoicAudioProcessor)
};
