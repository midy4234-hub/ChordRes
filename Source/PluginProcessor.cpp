#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace chordres;

namespace
{
    // |x| が threshold を超えた分だけ tanh で丸めて ±1 に収める
    inline float safetyClip (float x) noexcept
    {
        constexpr float threshold = 0.8f;
        const float ax = std::abs (x);
        if (ax <= threshold)
            return x;
        const float y = threshold + (1.0f - threshold) * std::tanh ((ax - threshold) / (1.0f - threshold));
        return std::copysign (y, x);
    }

    inline void atomicMax (std::atomic<float>& a, float v) noexcept
    {
        float cur = a.load (std::memory_order_relaxed);
        while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    }

    // log(cosh(u)) を大きな u でも溢れないように
    inline double logCosh (double u) noexcept
    {
        const double au = std::abs (u);
        return au + std::log1p (std::exp (-2.0 * au)) - 0.69314718055994530942;
    }

    // Drive: 歪ませる前に -24dB 下げる。Drive 0dB ではほぼ素通し (0dBFS で 0.13% の丸まり) で、
    // 上げるほど歪む。後段は -6dBFS のピークが -6dBFS のままになるように戻す
    // (0dBFS 基準だと小さめの入力で Drive を上げたときに +15dB 近く跳ね上がる)
    constexpr double drivePad = 0.0630957;   // -24dB
    constexpr double driveRef = 0.5;         // -6dBFS
}

//==============================================================================
ChordResAudioProcessor::ChordResAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ChordRes", createLayout())
{
    for (auto& d : voiceDisplay)
        d.store (-1);
    for (auto& l : voiceLevel)
        l.store (0.0f);
}

juce::AudioProcessorValueTreeState::ParameterLayout ChordResAudioProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;

    // 値の表示形式 (decimals 桁 + 単位)
    auto fmt = [] (int decimals, const char* unit)
    {
        return AudioParameterFloatAttributes().withStringFromValueFunction (
            [decimals, unit] (float v, int) { return String (v, decimals) + unit; });
    };

    auto skewed = [] (float lo, float hi, float centre)
    {
        NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    };

    // 内部の DSP 名は comb のまま。表示は String。Tube は保存済みのセットとの互換のため末尾に足している
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode",
                                                    StringArray { "String", "Bank", "Tube" }, 1));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "hold", 1 }, "Note Off",
                                                    StringArray { "Key", "Hold" }, 0));

    // エンベロープ系
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "attack", 1 }, "Attack",
        skewed (0.5f, 2000.0f, 50.0f), 5.0f, fmt (1, " ms")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "decay", 1 }, "Decay",
        skewed (0.05f, 20.0f, 2.0f), 2.0f, fmt (2, " s")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "amount", 1 }, "Amount",
        NormalisableRange<float> (amountOffDb, 12.0f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
            { return v <= amountOffDb + 0.01f ? String ("-inf dB") : String (v, 1) + " dB"; })));

    // 音色系
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "bright", 1 }, "Bright",
        NormalisableRange<float> (0.0f, 1.0f), 0.6f, fmt (2, "")));
    l.add (std::make_unique<AudioParameterInt> (ParameterID { "harm", 1 }, "Harmonics", 1, maxHarmonics, 12));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "stretch", 1 }, "Stretch",
        NormalisableRange<float> (-0.3f, 0.3f), 0.0f, fmt (3, "")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "sat", 1 }, "Drive",
        NormalisableRange<float> (0.0f, 48.0f), 0.0f, fmt (1, " dB")));
    l.add (std::make_unique<AudioParameterInt> (ParameterID { "transpose", 1 }, "Transpose", -24, 24, 0));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "fine", 1 }, "Fine",
        NormalisableRange<float> (-100.0f, 100.0f), 0.0f, fmt (1, " ct")));

    // フィルター系 (共鳴音にだけかかる。Drive の後、Mix の前)
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "ftype", 1 }, "Filter",
                                                    StringArray { "Off", "LP", "HP", "Notch", "Formant" }, 0));
    {
        NormalisableRange<float> r (20.0f, 20000.0f);
        r.setSkewForCentre (632.0f);   // 対数的に (20Hz と 20kHz の幾何平均が真ん中)
        l.add (std::make_unique<AudioParameterFloat> (ParameterID { "fcut", 1 }, "Freq", r, 2000.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
                { return v >= 1000.0f ? String (v / 1000.0f, 2) + " kHz" : String (juce::roundToInt (v)) + " Hz"; })));
    }
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "freso", 1 }, "Reso",
        NormalisableRange<float> (0.0f, 1.0f), 0.2f, fmt (2, "")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "vowel", 1 }, "Vowel",
        NormalisableRange<float> (0.0f, 4.0f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int)
        {
            const char* names[] = { "a", "e", "i", "o", "u" };
            const int i = juce::jlimit (0, 4, (int) std::floor (v + 0.05f));
            const float fr = v - (float) i;
            if (fr < 0.05f || i >= 4) return String (names[i]);
            return String (names[i]) + "-" + names[i + 1] + " " + String (juce::roundToInt (fr * 100.0f)) + "%";
        })));

    // マスタ系
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "width", 1 }, "Width",
        NormalisableRange<float> (0.0f, 50.0f), 6.0f, fmt (1, " ct")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "mix", 1 }, "Mix",
        NormalisableRange<float> (0.0f, 1.0f), 0.5f, fmt (2, "")));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "out", 1 }, "Output",
        NormalisableRange<float> (-24.0f, 12.0f), 0.0f, fmt (1, " dB")));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "safety", 1 }, "Safety", true));

    return l;
}

bool ChordResAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

//==============================================================================
void ChordResAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    settings.sampleRate = sampleRate;
    inBuf.setSize (maxChannels, samplesPerBlock);
    wetBuf.setSize (maxChannels, samplesPerBlock);

    dcCoef    = (float) std::exp (-juce::MathConstants<double>::twoPi * 10.0 / sampleRate);
    gateCoef  = (float) std::exp (-1.0 / (0.002 * sampleRate));   // 入口の開閉は時定数 2ms (クリック防止)
    levelCoef = (float) std::exp (-1.0 / (0.05 * sampleRate));
    inDcX.fill (0.0f); inDcY.fill (0.0f); outDcX.fill (0.0f); outDcY.fill (0.0f);
    satPrev.fill (0.0);

    for (auto* s : { &amountGain, &driveGain, &mixAmt, &outGain, &filterReso, &filterVowel })
        s->reset (sampleRate, 0.02);
    filterCutoff.reset (sampleRate, 0.02);
    filter.prepare (sampleRate);

    readSettings();
    for (auto* s : { &amountGain, &driveGain, &mixAmt, &outGain, &filterReso, &filterVowel })
        s->setCurrentAndTargetValue (s->getTargetValue());
    filterCutoff.setCurrentAndTargetValue (filterCutoff.getTargetValue());

    for (auto& v : voices)
    {
        v.res.reset();
        v.active = false;
        v.note = -1;
    }
    keyDown.fill (false);
    keysHeld = 0;
    sustainDown = false;
    lastMode = -1;
    updateDisplay();
}

void ChordResAudioProcessor::readSettings()
{
    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    settings.mode        = (Mode) juce::jlimit (0, 2, (int) std::lround (get ("mode")));
    settings.decaySec    = get ("decay");
    settings.bright      = get ("bright");
    settings.harmonics   = (int) get ("harm");
    settings.stretch     = get ("stretch");
    settings.pitchOffset = get ("transpose") + get ("fine") / 100.0f;
    settings.widthCents  = get ("width");

    latch    = get ("hold") > 0.5f;

    attackCoef = (float) std::exp (-3.0 / (get ("attack") * 0.001 * settings.sampleRate));   // 約 95% に達する時間

    const float amountDb = get ("amount");
    amountGain.setTargetValue (amountDb <= amountOffDb + 0.01f ? 0.0f : juce::Decibels::decibelsToGain (amountDb));
    driveGain.setTargetValue (juce::Decibels::decibelsToGain (get ("sat")));
    mixAmt.setTargetValue (get ("mix"));
    outGain.setTargetValue (juce::Decibels::decibelsToGain (get ("out")));

    filter.setType ((FilterType) juce::jlimit (0, 4, (int) std::lround (get ("ftype"))));
    filterCutoff.setTargetValue (get ("fcut"));
    filterReso.setTargetValue (get ("freso"));
    filterVowel.setTargetValue (get ("vowel"));
}

//==============================================================================
void ChordResAudioProcessor::startVoice (int note)
{
    // 同じノートが鳴っていれば共鳴を切らずに入口を開け直す
    for (auto& v : voices)
        if (v.active && v.note == note)
        {
            v.released = false;
            v.sustained = false;
            v.order = ++orderCounter;
            return;
        }

    Voice* target = nullptr;
    for (auto& v : voices)
        if (! v.active) { target = &v; break; }

    // 空きが無ければ、離された中で一番小さいもの → 一番古いもの の順で奪う
    if (target == nullptr)
    {
        for (auto& v : voices)
            if (v.released && (target == nullptr || v.level < target->level))
                target = &v;
    }
    if (target == nullptr)
    {
        for (auto& v : voices)
            if (target == nullptr || v.order < target->order)
                target = &v;
    }

    target->note = note;
    target->active = true;
    target->released = false;
    target->sustained = false;
    target->attackEnv = 0.0f;
    target->inGate = 0.0f;
    target->level = 0.0f;
    target->order = ++orderCounter;
    target->res.reset();
    target->res.setNote (note);
    target->res.update (settings);
}

void ChordResAudioProcessor::releaseNote (int note)
{
    for (auto& v : voices)
        if (v.active && v.note == note)
            v.released = true;
}

void ChordResAudioProcessor::releaseAll()
{
    for (auto& v : voices)
        if (v.active)
            v.released = true;
}

void ChordResAudioProcessor::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
    {
        const int n = m.getNoteNumber();

        // Hold: 全部の鍵盤が離れた後の最初のノートで、前のボイシングを入れ替える
        if (latch && keysHeld == 0)
            releaseAll();

        if (! keyDown[(size_t) n])
        {
            keyDown[(size_t) n] = true;
            ++keysHeld;
        }
        startVoice (n);
    }
    else if (m.isNoteOff())
    {
        const int n = m.getNoteNumber();
        if (keyDown[(size_t) n])
        {
            keyDown[(size_t) n] = false;
            --keysHeld;
        }

        if (! latch)
        {
            if (sustainDown)
            {
                for (auto& v : voices)
                    if (v.active && v.note == n)
                        v.sustained = true;
            }
            else
            {
                releaseNote (n);
            }
        }
    }
    else if (m.isSustainPedalOn())
    {
        sustainDown = true;
    }
    else if (m.isSustainPedalOff())
    {
        sustainDown = false;
        if (! latch)
            for (auto& v : voices)
                if (v.active && v.sustained && ! keyDown[(size_t) v.note])
                    v.released = true;
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        keyDown.fill (false);
        keysHeld = 0;
        releaseAll();

        if (m.isAllSoundOff())
            for (auto& v : voices)
            {
                v.active = false;
                v.note = -1;
                v.res.reset();
            }
    }
}

//==============================================================================
void ChordResAudioProcessor::renderVoices (int start, int end, int numCh)
{
    const bool isComb = settings.mode != Mode::bank;   // String と Tube は同じ処理 (係数だけ違う)
    constexpr float silence = 1.0e-5f;   // -100dB

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        const float gateTarget = v.released ? 0.0f : 1.0f;

        for (int i = start; i < end; ++i)
        {
            v.attackEnv = 1.0f + (v.attackEnv - 1.0f) * attackCoef;
            v.inGate = gateTarget + (v.inGate - gateTarget) * gateCoef;

            const float g = v.attackEnv;
            float peak = 0.0f;

            for (int ch = 0; ch < numCh; ++ch)
            {
                const float x = inBuf.getSample (ch, i) * v.inGate;
                const float y = isComb ? v.res.processComb (ch, x) : v.res.processBank (ch, x);
                wetBuf.getWritePointer (ch)[i] += y * g;
                peak = juce::jmax (peak, std::abs (y));
            }

            v.level = juce::jmax (peak, v.level * levelCoef);
        }

        // 入口が閉じて、余韻も消えたら空ける
        if (v.released && v.inGate < silence && v.level < silence)
        {
            v.active = false;
            v.note = -1;
        }
    }
}

// 共鳴の後段の歪み。tanh を 1 次の antiderivative anti-aliasing (ADAA) で通して折り返しを減らす
float ChordResAudioProcessor::saturate (int ch, float x, float g, float makeup) noexcept
{
    const double xd = x;
    const double x1 = satPrev[(size_t) ch];
    satPrev[(size_t) ch] = xd;

    const double dx = xd - x1;
    double y;
    if (std::abs (dx) > 1.0e-5)
        y = (logCosh (g * xd) - logCosh (g * x1)) / (g * dx);
    else
        y = std::tanh (g * 0.5 * (xd + x1));

    return (float) (y * makeup);
}

void ChordResAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numCh = juce::jmin (buffer.getNumChannels(), maxChannels);
    settings.numChannels = numCh;

    if (inBuf.getNumSamples() < numSamples)
    {
        inBuf.setSize (maxChannels, numSamples, false, false, true);
        wetBuf.setSize (maxChannels, numSamples, false, false, true);
    }

    readSettings();

    const int modeNow = (int) settings.mode;
    if (modeNow != lastMode)
    {
        for (auto& v : voices)
            v.res.reset();
        lastMode = modeNow;
    }

    // Hold → Key に切り替えたら、押さえていないボイスを離す
    if (! latch)
        for (auto& v : voices)
            if (v.active && ! v.released && ! keyDown[(size_t) v.note] && ! sustainDown)
                v.released = true;

    for (auto& v : voices)
        if (v.active)
            v.res.update (settings);

    // 入力: DC カット (共鳴体に DC を溜めない)
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* src = buffer.getReadPointer (ch);
        float* dst = inBuf.getWritePointer (ch);
        float xPrev = inDcX[(size_t) ch], yPrev = inDcY[(size_t) ch];
        for (int i = 0; i < numSamples; ++i)
        {
            const float y = src[i] - xPrev + dcCoef * yPrev;
            xPrev = src[i];
            yPrev = y;
            dst[i] = y;
        }
        inDcX[(size_t) ch] = xPrev;
        inDcY[(size_t) ch] = yPrev;
    }

    wetBuf.clear();

    // MIDI イベントの位置でブロックを区切って処理する
    int pos = 0;
    for (const auto meta : midi)
    {
        const int evPos = juce::jlimit (0, numSamples, meta.samplePosition);
        if (evPos > pos)
        {
            renderVoices (pos, evPos, numCh);
            pos = evPos;
        }
        handleMidi (meta.getMessage());
    }
    if (pos < numSamples)
        renderVoices (pos, numSamples, numCh);

    const bool safety = apvts.getRawParameterValue ("safety")->load() > 0.5f;
    atomicMax (meterIn, buffer.getMagnitude (0, numSamples));
    float wetPeak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float amt = amountGain.getNextValue();
        const float dg  = driveGain.getNextValue();
        const float mix = mixAmt.getNextValue();
        const float og  = outGain.getNextValue();

        const float gp     = (float) (dg * drivePad);
        const float makeup = (float) (driveRef / std::tanh (gp * driveRef));

        const float fc = filterCutoff.getNextValue();
        const float fr = filterReso.getNextValue();
        const float fv = filterVowel.getNextValue();
        if ((i & 15) == 0)
            filter.setParams (fc, fr, fv);

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float w = wetBuf.getSample (ch, i);
            float wy = w - outDcX[(size_t) ch] + dcCoef * outDcY[(size_t) ch];
            outDcX[(size_t) ch] = w;
            outDcY[(size_t) ch] = wy;

            wy *= amt;
            wetPeak = juce::jmax (wetPeak, std::abs (wy));
            wy = saturate (ch, wy, gp, makeup);
            wy = filter.process (ch, wy);

            float y = (buffer.getSample (ch, i) * (1.0f - mix) + wy * mix) * og;
            if (safety)
                y = safetyClip (y);
            buffer.setSample (ch, i, y);
        }
    }

    for (int ch = numCh; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    atomicMax (meterWet, wetPeak);
    atomicMax (meterOut, buffer.getMagnitude (0, numSamples));
    updateDisplay();
}

void ChordResAudioProcessor::updateDisplay()
{
    for (size_t i = 0; i < voices.size(); ++i)
    {
        const auto& v = voices[i];
        voiceDisplay[i].store (v.active ? v.note + (v.released ? 1000 : 0) : -1);
        voiceLevel[i].store (v.active ? v.level * v.attackEnv : 0.0f);
    }
}

int ChordResAudioProcessor::activeVoiceCount() const
{
    int n = 0;
    for (auto& v : voices)
        if (v.active)
            ++n;
    return n;
}

//==============================================================================
void ChordResAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void ChordResAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* ChordResAudioProcessor::createEditor()
{
    return new ChordResAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ChordResAudioProcessor();
}
