#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"

// Live 純正デバイス寄りの見た目: 細い線のノブ、小さい文字、角の小さいスイッチ
class ChordResLookAndFeel  : public juce::LookAndFeel_V4
{
public:
    ChordResLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool, bool) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
};

class ChordResAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                      private juce::Timer
{
public:
    explicit ChordResAudioProcessorEditor (ChordResAudioProcessor&);
    ~ChordResAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintDisplay (juce::Graphics&);
    void paintMeters (juce::Graphics&);

    using SliderAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<SliderAttach> attach;
    };

    // 選択式パラメータ (Choice) を横に並べたボタンで切り替える。並び順とパラメータの番号は別に指定できる
    struct Switch
    {
        std::vector<std::unique_ptr<juce::TextButton>> buttons;
        std::vector<int> values;
        std::unique_ptr<juce::ParameterAttachment> attach;
        void setBounds (juce::Rectangle<int>);
    };

    void setUpKnob (Knob&, const juce::String& paramId, const juce::String& text);
    void setUpSwitch (Switch&, const juce::String& paramId, std::initializer_list<std::pair<const char*, int>> items);

    ChordResAudioProcessor& proc;
    ChordResLookAndFeel lnf;

    // エンベロープ系 / 音色系 / マスタ系
    Knob attack, decay, amount,
         bright, harm, stretch, drive, transpose, fine,
         freq, reso, vowel,
         width, mix, output;

    Switch noteOffSwitch, modeSwitch, filterSwitch;

    juce::TextButton safetyBtn { "Safety" };
    std::unique_ptr<ButtonAttach> safetyAttach;

    // 表示用
    std::array<int,   chordres::maxVoices> shownNotes {};
    std::array<float, chordres::maxVoices> shownLevel {};
    float meterIn = -100.0f, meterWet = -100.0f, meterOut = -100.0f;   // dB
    float holdWet = -100.0f;
    int   holdWetAge = 0;

    juce::Rectangle<int> displayArea, meterArea, envPanel, tonePanel, filterPanel, masterPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordResAudioProcessorEditor)
};
