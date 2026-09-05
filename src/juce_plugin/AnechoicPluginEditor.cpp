#include "AnechoicAudioProcessor.h"
#include "AnechoicPluginEditor.h"

#include <memory>

//==============================================================================
AnechoicAudioProcessorEditor::AnechoicAudioProcessorEditor(AnechoicAudioProcessor &p,
                                                           juce::AudioProcessorValueTreeState &vts)
        : AudioProcessorEditor(&p), m_valueTreeState(vts), m_processorRef(p) {
    addAndMakeVisible(m_headerLabel);
    m_headerLabel.setText("Anechoic Noise Suppression", juce::dontSendNotification);
    m_headerLabel.setFont(juce::Font(24.0f, juce::Font::bold | juce::Font::underlined));
    m_headerLabel.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(m_rateWarningLabel);
    m_rateWarningLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    m_rateWarningLabel.setJustificationType(juce::Justification::centred);
    m_rateWarningLabel.setText({}, juce::dontSendNotification);

    addAndMakeVisible(m_meterLabel);
    m_meterLabel.setText("Voice activity", juce::dontSendNotification);

    auto thresholdParam = m_processorRef.m_parameters.getParameter("vad_threshold");
    auto gracePeriodParam = m_processorRef.m_parameters.getParameter("vad_grace_period");
    auto retroactiveGracePeriodParam =
            m_processorRef.m_parameters.getParameter("vad_retroactive_grace_period");
    auto hysteresisParam = m_processorRef.m_parameters.getParameter("vad_hysteresis");
    auto comfortNoiseParam = m_processorRef.m_parameters.getParameter("comfort_noise_db");

    m_thresholdLabel.setText(thresholdParam->getName(99), juce::dontSendNotification);
    addAndMakeVisible(m_thresholdLabel);
    addAndMakeVisible(m_thresholdSlider);
    m_thresholdAttachment = std::make_unique<SliderAttachment>(m_valueTreeState,
                                                              thresholdParam->getParameterID(),
                                                              m_thresholdSlider);

    m_gracePeriodLabel.setText(gracePeriodParam->getName(99), juce::dontSendNotification);
    addAndMakeVisible(m_gracePeriodLabel);
    addAndMakeVisible(m_gracePeriodSlider);
    m_gracePeriodAttachment = std::make_unique<SliderAttachment>(m_valueTreeState,
                                                                gracePeriodParam->getParameterID(),
                                                                m_gracePeriodSlider);

    m_retroactiveGracePeriodLabel.setText(retroactiveGracePeriodParam->getName(99),
                                          juce::dontSendNotification);
    addAndMakeVisible(m_retroactiveGracePeriodLabel);
    addAndMakeVisible(m_retroactiveGracePeriodSlider);
    m_retroactiveGracePeriodAttachment =
            std::make_unique<SliderAttachment>(m_valueTreeState,
                                               retroactiveGracePeriodParam->getParameterID(),
                                               m_retroactiveGracePeriodSlider);

    m_hysteresisLabel.setText(hysteresisParam->getName(99), juce::dontSendNotification);
    addAndMakeVisible(m_hysteresisLabel);
    addAndMakeVisible(m_hysteresisSlider);
    m_hysteresisAttachment = std::make_unique<SliderAttachment>(m_valueTreeState,
                                                               hysteresisParam->getParameterID(),
                                                               m_hysteresisSlider);

    m_comfortNoiseLabel.setText(comfortNoiseParam->getName(99), juce::dontSendNotification);
    addAndMakeVisible(m_comfortNoiseLabel);
    addAndMakeVisible(m_comfortNoiseSlider);
    m_comfortNoiseAttachment = std::make_unique<SliderAttachment>(m_valueTreeState,
                                                                 comfortNoiseParam->getParameterID(),
                                                                 m_comfortNoiseSlider);

    addAndMakeVisible(m_statsHeaderLabel);
    m_statsHeaderLabel.setText("Gate Statistics (updated once per second)", juce::dontSendNotification);
    m_statsHeaderLabel.setFont(juce::Font(18.0f, juce::Font::bold));
    m_statsHeaderLabel.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(m_statsHoldLabel);
    addAndMakeVisible(m_statsRewindLabel);
    addAndMakeVisible(m_statsHysteresisLabel);
    addAndMakeVisible(m_statsBacklogLabel);
    addAndMakeVisible(m_statsPaddedLabel);

    setSize(400, 540);
}

void AnechoicAudioProcessorEditor::paint(juce::Graphics &g) {
    g.fillAll(juce::Colours::black);

    auto bounds = m_meterBounds.toFloat();
    if (bounds.isEmpty())
        return;

    g.setColour(juce::Colours::darkgrey);
    g.fillRoundedRectangle(bounds, 3.0f);

    const float thresholdPct = m_processorRef.m_thresholdParam != nullptr
                                   ? m_processorRef.m_thresholdParam->get()
                                   : 55.0f;
    const bool open = m_displayedVad * 100.0f >= thresholdPct;
    g.setColour(open ? juce::Colours::limegreen : juce::Colours::darkorange);
    auto fill = bounds.withWidth(bounds.getWidth() * juce::jlimit(0.0f, 1.0f, m_displayedVad));
    g.fillRoundedRectangle(fill, 3.0f);

    g.setColour(juce::Colours::white);
    g.drawRoundedRectangle(bounds, 3.0f, 1.0f);

    const float x = bounds.getX() + bounds.getWidth() * (thresholdPct / 100.0f);
    g.setColour(juce::Colours::yellow);
    g.drawLine(x, bounds.getY(), x, bounds.getBottom(), 1.0f);
}

void AnechoicAudioProcessorEditor::resized() {
    juce::FlexBox flexBox;
    flexBox.flexWrap = juce::FlexBox::Wrap::wrap;
    flexBox.justifyContent = juce::FlexBox::JustifyContent::flexStart;
    flexBox.alignContent = juce::FlexBox::AlignContent::flexStart;
    flexBox.flexDirection = juce::FlexBox::Direction::column;

    auto area = getLocalBounds();
    const float width = static_cast<float>(area.getWidth());

    flexBox.items.add(juce::FlexItem(m_headerLabel).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_rateWarningLabel).withWidth(width).withFlex(0.6));
    flexBox.items.add(juce::FlexItem(m_meterLabel).withWidth(width).withFlex(0.5));
    // Reserve a strip for the meter drawn in paint().
    flexBox.items.add(juce::FlexItem().withWidth(width).withHeight(22.0f));
    const auto meterItemIndex = flexBox.items.size() - 1;

    flexBox.items.add(juce::FlexItem(m_thresholdLabel).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_thresholdSlider).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_gracePeriodLabel).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_gracePeriodSlider).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_retroactiveGracePeriodLabel).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_retroactiveGracePeriodSlider).withWidth(width).withFlex(1.0));

    flexBox.items.add(juce::FlexItem(m_hysteresisLabel).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_hysteresisSlider).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_comfortNoiseLabel).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_comfortNoiseSlider).withWidth(width).withFlex(1.0));

    flexBox.items.add(juce::FlexItem(m_statsHeaderLabel).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_statsHoldLabel).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_statsRewindLabel).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_statsHysteresisLabel).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_statsBacklogLabel).withWidth(width).withFlex(1.0));
    flexBox.items.add(juce::FlexItem(m_statsPaddedLabel).withWidth(width).withFlex(1.0));

    flexBox.performLayout(area.toFloat());
    m_meterBounds = flexBox.items[(int) meterItemIndex].currentBounds.getSmallestIntegerContainer().reduced(8, 2);
}

void AnechoicAudioProcessorEditor::visibilityChanged() {
    if (isVisible()) {
        timerCallback();
        startTimer(50);
    } else {
        stopTimer();
    }
}

void AnechoicAudioProcessorEditor::timerCallback() {
    const auto snap = m_processorRef.uiSnapshot();
    m_displayedVad = snap.voiceProbability;
    repaint(m_meterBounds);

    if (snap.wrongSampleRate) {
        m_rateWarningLabel.setText("Host is not 48 kHz — suppression bypassed. Set the device to 48000 Hz.",
                                   juce::dontSendNotification);
    } else {
        m_rateWarningLabel.setText({}, juce::dontSendNotification);
    }

    if (++m_statsTick < 20)
        return;
    m_statsTick = 0;

    constexpr long long kMsPerBlock = 10;
    const auto holdDelta = snap.holdBlocks - m_lastStats.holdBlocks;
    const auto rewindDelta = snap.rewindBlocks - m_lastStats.rewindBlocks;
    const auto latchDelta = snap.latchBlocks - m_lastStats.latchBlocks;
    const auto paddedDelta = snap.paddedSamples - m_lastStats.paddedSamples;
    m_lastStats = snap;

    juce::String holdText = juce::String("Held open by grace period: ");
    holdText << static_cast<long long>(holdDelta) * kMsPerBlock << " ms";
    m_statsHoldLabel.setText(holdText, juce::dontSendNotification);

    juce::String rewindText = juce::String("Reopened by retroactive grace: ");
    rewindText << static_cast<long long>(rewindDelta) * kMsPerBlock << " ms";
    m_statsRewindLabel.setText(rewindText, juce::dontSendNotification);

    juce::String hysteresisText = juce::String("Kept open by hysteresis: ");
    hysteresisText << static_cast<long long>(latchDelta) * kMsPerBlock << " ms";
    m_statsHysteresisLabel.setText(hysteresisText, juce::dontSendNotification);

    juce::String backlogText = juce::String("Buffered output: ");
    backlogText << static_cast<long long>(snap.backlogBlocks) * kMsPerBlock << " ms";
    m_statsBacklogLabel.setText(backlogText, juce::dontSendNotification);

    juce::String paddedText = juce::String("Output samples zeroed: ");
    paddedText << static_cast<long long>(paddedDelta);
    m_statsPaddedLabel.setText(paddedText, juce::dontSendNotification);
}

AnechoicAudioProcessorEditor::~AnechoicAudioProcessorEditor() = default;
