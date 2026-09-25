#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include "Resonator.h"
#include "WetFilter.h"

class ChordResAudioProcessor  : public juce::AudioProcessor
{
public:
    ChordResAudioProcessor();
    ~ChordResAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 20.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    static constexpr float amountOffDb = -48.0f;   // Amount の左端 = 無音

    // UI 表示用。鳴っているボイスのノート番号 (無ければ -1)。鍵盤が離されて余韻だけのものは +1000
    std::array<std::atomic<int>, chordres::maxVoices> voiceDisplay;
    std::array<std::atomic<float>, chordres::maxVoices> voiceLevel;   // 共鳴の出力ピーク (Amount 前)

    // メーター: 前回 UI が読んでからの最大ピーク。UI は exchange(0) で読む
    std::atomic<float> meterIn { 0.0f }, meterWet { 0.0f }, meterOut { 0.0f };

    // テスト用
    int activeVoiceCount() const;

private:
    struct Voice
    {
        chordres::Resonator res;
        int      note = -1;
        bool     active = false;
        bool     released = false;   // 鍵盤が離された (入口を閉じる)
        bool     sustained = false;
        float    attackEnv = 0.0f;   // 出口側。鳴り始めのフェードインだけ
        float    inGate = 0.0f;      // 入口側。押している間 1、離すと数 ms で 0
        float    level = 0.0f;       // 出力の減衰ピーク。消えたかどうかの判定と、ボイスを奪う順番に使う
        uint64_t order = 0;
    };

    void readSettings();
    void handleMidi (const juce::MidiMessage&);
    void startVoice (int note);
    void releaseNote (int note);
    void releaseAll();
    void renderVoices (int start, int end, int numCh);
    float saturate (int ch, float x, float g, float makeup) noexcept;
    void updateDisplay();

    std::array<Voice, chordres::maxVoices> voices;
    std::array<bool, 128> keyDown {};
    int  keysHeld = 0;
    bool sustainDown = false;
    uint64_t orderCounter = 0;

    chordres::Settings settings;
    bool  latch = false;
    float attackCoef = 0.0f, gateCoef = 0.0f, levelCoef = 0.0f;
    int   lastMode = -1;

    juce::AudioBuffer<float> inBuf, wetBuf;
    std::array<float, chordres::maxChannels> inDcX {}, inDcY {}, outDcX {}, outDcY {};
    std::array<double, chordres::maxChannels> satPrev {};
    float dcCoef = 0.999f;

    juce::SmoothedValue<float> amountGain, driveGain, mixAmt, outGain, filterReso, filterVowel;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> filterCutoff;
    chordres::WetFilter filter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChordResAudioProcessor)
};
