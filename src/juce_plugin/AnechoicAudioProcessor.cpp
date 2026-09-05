#include "AnechoicAudioProcessor.h"
#include "AnechoicPluginEditor.h"

#include "anechoic/SpeechEnhancer.h"

#include <cmath>
#include <cstdint>
#include <memory>

//==============================================================================
AnechoicAudioProcessor::AnechoicAudioProcessor()
        : AudioProcessor(BusesProperties()
                                 .withInput("Input", juce::AudioChannelSet::namedChannelSet(NUM_CHANNELS), true)
                                 .withOutput("Output", juce::AudioChannelSet::namedChannelSet(NUM_CHANNELS), true)),
          m_parameters(*this, nullptr, juce::Identifier("Anechoic"),
                       {
                               std::make_unique<juce::AudioParameterFloat>("vad_threshold",
                                                                           "VAD Threshold (%)",
                                                                           juce::NormalisableRange<float>(0.0f, 99.0f, 1.0f),
                                                                           55.0f),
                               std::make_unique<juce::AudioParameterInt>("vad_grace_period",
                                                                         "VAD Grace Period (ms)",
                                                                         0,
                                                                         1000,
                                                                         200),
                               std::make_unique<juce::AudioParameterInt>("vad_retroactive_grace_period",
                                                                         "Retroactive VAD Grace Period (ms)",
                                                                         0,
                                                                         200,
                                                                         0),
                               std::make_unique<juce::AudioParameterFloat>("vad_hysteresis",
                                                                           "VAD Hysteresis (%)",
                                                                           juce::NormalisableRange<float>(0.0f, 50.0f, 1.0f),
                                                                           0.0f),
                               std::make_unique<juce::AudioParameterFloat>("comfort_noise_db",
                                                                           "Comfort Noise Floor (dB)",
                                                                           juce::NormalisableRange<float>(-90.0f, -30.0f, 0.5f),
                                                                           -40.0f)
                       }) {
    m_thresholdParam = static_cast<juce::AudioParameterFloat *>(m_parameters.getParameter("vad_threshold"));
    m_gracePeriodParam = static_cast<juce::AudioParameterInt *>(m_parameters.getParameter("vad_grace_period"));
    m_retroactiveGracePeriodParam =
            static_cast<juce::AudioParameterInt *>(m_parameters.getParameter("vad_retroactive_grace_period"));
    m_hysteresisParam = static_cast<juce::AudioParameterFloat *>(m_parameters.getParameter("vad_hysteresis"));
    m_comfortNoiseDbParam = static_cast<juce::AudioParameterFloat *>(m_parameters.getParameter("comfort_noise_db"));

    m_parameters.addParameterListener("vad_retroactive_grace_period", this);
}

AnechoicAudioProcessor::~AnechoicAudioProcessor() {
    m_parameters.removeParameterListener("vad_retroactive_grace_period", this);
}

//==============================================================================
const juce::String AnechoicAudioProcessor::getName() const {
    return JucePlugin_Name;
}

bool AnechoicAudioProcessor::acceptsMidi() const {
    return false;
}

bool AnechoicAudioProcessor::producesMidi() const {
    return false;
}

double AnechoicAudioProcessor::getTailLengthSeconds() const {
    const double grace = m_gracePeriodParam != nullptr
                             ? static_cast<double>(m_gracePeriodParam->get()) / 1000.0
                             : 0.2;
    return grace + 0.02;  // hold plus the 8 ms open envelope
}

int AnechoicAudioProcessor::getNumPrograms() {
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
    // so this should be at least 1, even if you're not really implementing programs.
}

int AnechoicAudioProcessor::getCurrentProgram() {
    return 0;
}

void AnechoicAudioProcessor::setCurrentProgram(int index) {
    juce::ignoreUnused(index);
}

const juce::String AnechoicAudioProcessor::getProgramName(int index) {
    juce::ignoreUnused(index);
    return {};
}

void AnechoicAudioProcessor::changeProgramName(int index, const juce::String &newName) {
    juce::ignoreUnused(index, newName);
}

//==============================================================================
void AnechoicAudioProcessor::updateLatency() {
    const auto rewindMs = m_retroactiveGracePeriodParam != nullptr
                              ? m_retroactiveGracePeriodParam->get()
                              : 0;
    const int rewindBlocks = rewindMs / 10;
    setLatencySamples(rewindBlocks * static_cast<int>(anechoic::kFrameSamples));
}

void AnechoicAudioProcessor::parameterChanged(const juce::String &parameterID, float newValue) {
    juce::ignoreUnused(newValue);
    if (parameterID == "vad_retroactive_grace_period")
        updateLatency();
}

void AnechoicAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    juce::ignoreUnused(samplesPerBlock);

    m_wrongSampleRate.store(std::abs(sampleRate - static_cast<double>(anechoic::kEngineRateHz)) > 0.5,
                            std::memory_order_relaxed);

    const auto channels = static_cast<std::uint32_t>(juce::jmax(1, getTotalNumInputChannels()));
    m_suppressor = std::make_unique<anechoic::Suppressor>(channels);
    updateLatency();
}

void AnechoicAudioProcessor::releaseResources() {
    m_suppressor.reset();
}

bool AnechoicAudioProcessor::isBusesLayoutSupported(const BusesLayout &layouts) const {
    if (layouts.getMainInputChannelSet() == juce::AudioChannelSet::disabled()
        || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::disabled())
        return false;

    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == layouts.getMainOutputChannelSet();
}

void AnechoicAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                          juce::MidiBuffer &midiMessages) {
    juce::ignoreUnused(midiMessages);

    juce::ScopedNoDenormals noDenormals;

    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    if (m_wrongSampleRate.load(std::memory_order_relaxed) || !m_suppressor) {
        return;
    }

    if (totalNumInputChannels == 0) {
        buffer.clear();
        return;
    }

    const float *in[anechoic::kMaxChannels] = {nullptr};
    float *out[anechoic::kMaxChannels] = {nullptr};
    for (int channel = 0; channel < totalNumInputChannels; ++channel) {
        in[channel] = buffer.getReadPointer(channel);
        out[channel] = buffer.getWritePointer(channel);
    }

    anechoic::GateSettings settings;
    settings.threshold = m_thresholdParam->get() / 100.0f;
    settings.holdBlocks = static_cast<std::uint32_t>(m_gracePeriodParam->get()) / 10u;
    settings.rewindBlocks = static_cast<std::uint32_t>(m_retroactiveGracePeriodParam->get()) / 10u;
    settings.hysteresis = m_hysteresisParam->get() / 100.0f;
    settings.muteGainDb = m_comfortNoiseDbParam->get();

    m_suppressor->process(in, out, static_cast<size_t>(buffer.getNumSamples()), settings);

    const auto stats = m_suppressor->diagnostics();
    m_uiHold.store(stats.holdBlocks, std::memory_order_relaxed);
    m_uiRewind.store(stats.rewindBlocks, std::memory_order_relaxed);
    m_uiLatch.store(stats.latchBlocks, std::memory_order_relaxed);
    m_uiBacklog.store(stats.backlogBlocks, std::memory_order_relaxed);
    m_uiPadded.store(stats.paddedSamples, std::memory_order_relaxed);
    m_uiVad.store(stats.voiceProbability, std::memory_order_relaxed);
}

AnechoicAudioProcessor::UiSnapshot AnechoicAudioProcessor::uiSnapshot() const {
    UiSnapshot snap;
    snap.holdBlocks = m_uiHold.load(std::memory_order_relaxed);
    snap.rewindBlocks = m_uiRewind.load(std::memory_order_relaxed);
    snap.latchBlocks = m_uiLatch.load(std::memory_order_relaxed);
    snap.backlogBlocks = m_uiBacklog.load(std::memory_order_relaxed);
    snap.paddedSamples = m_uiPadded.load(std::memory_order_relaxed);
    snap.voiceProbability = m_uiVad.load(std::memory_order_relaxed);
    snap.wrongSampleRate = m_wrongSampleRate.load(std::memory_order_relaxed);
    return snap;
}

//==============================================================================
bool AnechoicAudioProcessor::hasEditor() const {
    return true;
}

juce::AudioProcessorEditor *AnechoicAudioProcessor::createEditor() {
    return new AnechoicAudioProcessorEditor(*this, m_parameters);
}

//==============================================================================
void AnechoicAudioProcessor::getStateInformation(juce::MemoryBlock &destData) {
    auto state = m_parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void AnechoicAudioProcessor::setStateInformation(const void *data, int sizeInBytes) {
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));

    if (!xml || !xml->hasTagName(m_parameters.state.getType()))
        return;

    m_parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
    return new AnechoicAudioProcessor();
}
