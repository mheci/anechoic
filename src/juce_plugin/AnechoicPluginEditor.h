#pragma once

#include "AnechoicAudioProcessor.h"

//==============================================================================
class AnechoicAudioProcessorEditor : public juce::AudioProcessorEditor, public juce::Timer {
public:
    explicit AnechoicAudioProcessorEditor(AnechoicAudioProcessor &p,
                                          juce::AudioProcessorValueTreeState &vts);

    ~AnechoicAudioProcessorEditor() override;

    void resized() override;

    void paint(juce::Graphics &g) override;

    void visibilityChanged() override;

    void timerCallback() override;

private:
    typedef juce::AudioProcessorValueTreeState::SliderAttachment SliderAttachment;

    juce::AudioProcessorValueTreeState &m_valueTreeState;

    juce::Label m_headerLabel;
    juce::Label m_rateWarningLabel;
    juce::Label m_meterLabel;

    juce::Label m_thresholdLabel;
    juce::Slider m_thresholdSlider;
    std::unique_ptr<SliderAttachment> m_thresholdAttachment;

    juce::Label m_gracePeriodLabel;
    juce::Slider m_gracePeriodSlider;
    std::unique_ptr<SliderAttachment> m_gracePeriodAttachment;

    juce::Label m_retroactiveGracePeriodLabel;
    juce::Slider m_retroactiveGracePeriodSlider;
    std::unique_ptr<SliderAttachment> m_retroactiveGracePeriodAttachment;

    juce::Label m_hysteresisLabel;
    juce::Slider m_hysteresisSlider;
    std::unique_ptr<SliderAttachment> m_hysteresisAttachment;

    juce::Label m_comfortNoiseLabel;
    juce::Slider m_comfortNoiseSlider;
    std::unique_ptr<SliderAttachment> m_comfortNoiseAttachment;

    juce::Label m_statsHeaderLabel;
    juce::Label m_statsHoldLabel;
    juce::Label m_statsRewindLabel;
    juce::Label m_statsHysteresisLabel;
    juce::Label m_statsBacklogLabel;
    juce::Label m_statsPaddedLabel;

    juce::Rectangle<int> m_meterBounds;
    float m_displayedVad = 0.0f;
    int m_statsTick = 0;
    AnechoicAudioProcessor::UiSnapshot m_lastStats;

    AnechoicAudioProcessor &m_processorRef;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnechoicAudioProcessorEditor)
};
