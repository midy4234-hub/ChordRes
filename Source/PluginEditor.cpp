#include "PluginEditor.h"

namespace
{
    namespace col
    {
        const juce::Colour bg          { 0xff2e2e2e };
        const juce::Colour panel       { 0xff363636 };
        const juce::Colour panelStroke { 0xff404040 };
        const juce::Colour text        { 0xffd2d2d2 };
        const juce::Colour dim         { 0xff8c8c8c };
        const juce::Colour lcd         { 0xff1f1f1f };
        const juce::Colour lcdGrid     { 0xff333333 };
        const juce::Colour partial     { 0xff6ec8ff };
        const juce::Colour accent      { 0xffffa400 };
        const juce::Colour meter       { 0xff8ee06b };
        const juce::Colour over        { 0xffff5a5a };
        const juce::Colour keyWhite    { 0xff9a9a9a };
        const juce::Colour keyBlack    { 0xff2a2a2a };
        const juce::Colour track       { 0xff555555 };
        const juce::Colour switchOff   { 0xff2a2a2a };
        const juce::Colour onText      { 0xff1e1e1e };
    }

    constexpr int kbLow  = 24;    // C0 (Live 表記、C3 = 60)
    constexpr int kbHigh = 107;   // B6

    constexpr float meterMinDb = -48.0f, meterMaxDb = 12.0f;

    bool isBlackKey (int n)
    {
        const int pc = ((n % 12) + 12) % 12;
        return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
    }

    juce::String noteName (int n)
    {
        return juce::MidiMessage::getMidiNoteName (n, true, true, 3);
    }

    // 共鳴の大きさ (リニア) → 0..1 の明るさ。-48dB で 0、0dB で 1
    float levelToBrightness (float lin)
    {
        const float db = juce::Decibels::gainToDecibels (lin, -100.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 48.0f) / 48.0f);
    }
}

//==============================================================================
ChordResLookAndFeel::ChordResLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, col::text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, col::accent.withAlpha (0.4f));
    setColour (juce::Label::textColourId, col::text);
    setColour (juce::TextEditor::backgroundColourId, col::lcd);
    setColour (juce::TextEditor::textColourId, col::text);
    setColour (juce::TextEditor::focusedOutlineColourId, col::accent);
    setColour (juce::CaretComponent::caretColourId, col::accent);
}

void ChordResLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                            float startAngle, float endAngle, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat();
    const float r = juce::jmin (16.0f, juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 3.0f);
    const auto c = bounds.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);

    // 両側に振れるパラメータ (Transpose など) は 0 から弧を描く
    float origin = startAngle;
    if ((bool) s.getProperties()["bipolar"])
        origin = startAngle + (float) s.valueToProportionOfLength (0.0) * (endAngle - startAngle);

    juce::Path trackArc;
    trackArc.addCentredArc (c.x, c.y, r, r, 0.0f, startAngle, endAngle, true);
    g.setColour (col::track);
    g.strokePath (trackArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));

    if (std::abs (angle - origin) > 0.001f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc (c.x, c.y, r, r, 0.0f, juce::jmin (origin, angle), juce::jmax (origin, angle), true);
        g.setColour (col::accent);
        g.strokePath (valueArc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }

    const juce::Point<float> tip (c.x + r * std::sin (angle), c.y - r * std::cos (angle));
    g.setColour (col::text);
    g.drawLine ({ c, tip }, 1.5f);
}

void ChordResLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                                                bool hover, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();

    auto fill = on ? col::accent : col::switchOff;
    if (hover && ! on) fill = fill.brighter (0.15f);
    if (down) fill = fill.darker (0.1f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, 2.0f);
    if (! on)
    {
        g.setColour (col::panelStroke.brighter (0.1f));
        g.drawRoundedRectangle (r, 2.0f, 1.0f);
    }
}

void ChordResLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool hover, bool)
{
    const bool on = b.getToggleState();
    g.setColour (on ? col::onText : (hover ? col::text : col::dim));
    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
}

juce::Font ChordResLookAndFeel::getLabelFont (juce::Label&)
{
    return juce::FontOptions (10.5f);
}

juce::Label* ChordResLookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (juce::FontOptions (10.5f));
    l->setColour (juce::Label::textColourId, col::text);
    l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    return l;
}

//==============================================================================
ChordResAudioProcessorEditor::ChordResAudioProcessorEditor (ChordResAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);

    setUpKnob (attack,    "attack",    "Attack");
    setUpKnob (decay,     "decay",     "Decay");
    setUpKnob (amount,    "amount",    "Amount");

    setUpKnob (bright,    "bright",    "Bright");
    setUpKnob (harm,      "harm",      "Harmonics");
    setUpKnob (stretch,   "stretch",   "Stretch");
    setUpKnob (drive,     "sat",       "Drive");
    setUpKnob (transpose, "transpose", "Transpose");
    setUpKnob (fine,      "fine",      "Fine");
    for (auto* k : { &stretch, &transpose, &fine })
        k->slider.getProperties().set ("bipolar", true);

    setUpKnob (freq,      "fcut",      "Freq");
    setUpKnob (reso,      "freso",     "Reso");
    setUpKnob (vowel,     "vowel",     "Vowel");

    setUpKnob (width,     "width",     "Width");
    setUpKnob (mix,       "mix",       "Mix");
    setUpKnob (output,    "out",       "Output");

    setUpSwitch (noteOffSwitch, "hold",  { { "Key", 0 }, { "Hold", 1 } });
    setUpSwitch (modeSwitch,    "mode",  { { "String", 0 }, { "Tube", 2 }, { "Bank", 1 } });
    setUpSwitch (filterSwitch,  "ftype", { { "Off", 0 }, { "LP", 1 }, { "HP", 2 }, { "Notch", 3 }, { "Formant", 4 } });

    safetyBtn.setClickingTogglesState (true);
    addAndMakeVisible (safetyBtn);
    safetyAttach = std::make_unique<ButtonAttach> (proc.apvts, "safety", safetyBtn);

    shownNotes.fill (-1);
    shownLevel.fill (0.0f);

    setSize (900, 480);
    startTimerHz (30);
}

ChordResAudioProcessorEditor::~ChordResAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void ChordResAudioProcessorEditor::setUpKnob (Knob& k, const juce::String& paramId, const juce::String& t)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 14);
    k.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    k.slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    k.slider.setColour (juce::Slider::textBoxTextColourId, col::text);
    addAndMakeVisible (k.slider);

    k.label.setText (t, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    k.label.setColour (juce::Label::textColourId, col::dim);
    addAndMakeVisible (k.label);

    k.attach = std::make_unique<SliderAttach> (proc.apvts, paramId, k.slider);

    // ダブルクリックで初期値に戻す
    if (auto* param = proc.apvts.getParameter (paramId))
        k.slider.setDoubleClickReturnValue (true, param->convertFrom0to1 (param->getDefaultValue()));
}

void ChordResAudioProcessorEditor::setUpSwitch (Switch& sw, const juce::String& paramId,
                                                std::initializer_list<std::pair<const char*, int>> items)
{
    auto* swPtr = &sw;
    for (auto& [text, value] : items)
    {
        auto b = std::make_unique<juce::TextButton> (text);
        const int v = value;
        b->onClick = [swPtr, v] { swPtr->attach->setValueAsCompleteGesture ((float) v); };
        addAndMakeVisible (*b);
        sw.buttons.push_back (std::move (b));
        sw.values.push_back (value);
    }

    sw.attach = std::make_unique<juce::ParameterAttachment> (*proc.apvts.getParameter (paramId),
        [swPtr] (float v)
        {
            for (size_t i = 0; i < swPtr->buttons.size(); ++i)
                swPtr->buttons[i]->setToggleState (juce::roundToInt (v) == swPtr->values[i], juce::dontSendNotification);
        });
    sw.attach->sendInitialUpdate();
}

void ChordResAudioProcessorEditor::Switch::setBounds (juce::Rectangle<int> r)
{
    const int n = (int) buttons.size();
    for (int i = 0; i < n; ++i)
    {
        const int x0 = r.getX() + r.getWidth() * i / n, x1 = r.getX() + r.getWidth() * (i + 1) / n;
        buttons[(size_t) i]->setBounds (x0, r.getY(), x1 - x0, r.getHeight());
    }
}

//==============================================================================
void ChordResAudioProcessorEditor::timerCallback()
{
    for (size_t i = 0; i < shownNotes.size(); ++i)
    {
        shownNotes[i] = proc.voiceDisplay[i].load();
        shownLevel[i] = proc.voiceLevel[i].load();
    }

    // メーター: 上がるときは即座に、下がるときは 1 フレーム 1.5dB (= 45dB/s)
    auto fall = [] (float& shown, std::atomic<float>& src)
    {
        const float db = juce::Decibels::gainToDecibels (src.exchange (0.0f), -100.0f);
        shown = juce::jmax (db, shown - 1.5f);
    };
    fall (meterIn,  proc.meterIn);
    fall (meterWet, proc.meterWet);
    fall (meterOut, proc.meterOut);

    // WET のピークホールド 1.5 秒
    if (meterWet >= holdWet || ++holdWetAge > 45)
    {
        holdWet = meterWet;
        holdWetAge = 0;
    }

    // 効かないつまみは薄くする
    auto enable = [] (Knob& k, bool on)
    {
        k.slider.setEnabled (on);
        k.slider.setAlpha (on ? 1.0f : 0.35f);
        k.label.setAlpha (on ? 1.0f : 0.35f);
    };
    const bool bank = juce::roundToInt (proc.apvts.getRawParameterValue ("mode")->load()) == (int) chordres::Mode::bank;
    enable (harm, bank);
    enable (stretch, bank);

    const int ft = juce::roundToInt (proc.apvts.getRawParameterValue ("ftype")->load());
    enable (freq,  ft >= 1 && ft <= 3);
    enable (reso,  ft != 0);
    enable (vowel, ft == 4);

    repaint (displayArea.getUnion (meterArea));
}

void ChordResAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);

    g.setColour (col::text);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText ("ChordRes", 16, 6, 200, 24, juce::Justification::centredLeft);

    auto drawPanel = [&g] (juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (col::panel);
        g.fillRoundedRectangle (r.toFloat(), 2.0f);
        g.setColour (col::panelStroke);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 2.0f, 1.0f);
        g.setColour (col::dim);
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (title, r.getX() + 10, r.getY() + 6, 100, 18, juce::Justification::centredLeft);
    };
    drawPanel (envPanel,    "ENVELOPE");
    drawPanel (tonePanel,   "TONE");
    drawPanel (filterPanel, "FILTER");
    drawPanel (masterPanel, "MASTER");

    paintDisplay (g);
    paintMeters (g);
}

void ChordResAudioProcessorEditor::paintDisplay (juce::Graphics& g)
{
    const auto area = displayArea.toFloat();
    g.setColour (col::lcd);
    g.fillRoundedRectangle (area, 2.0f);

    const auto inner = area.reduced (10.0f, 8.0f);
    const float keyH = 46.0f;
    const auto kb = inner.withTop (inner.getBottom() - keyH);
    const float specTop = inner.getY() + 16.0f, specBot = kb.getY() - 6.0f;

    // 鍵盤の座標。整数ノートは鍵の中心 (黒鍵は白鍵の境目)、小数はその間を線形に
    int whiteCount = 0;
    for (int n = kbLow; n <= kbHigh; ++n)
        if (! isBlackKey (n)) ++whiteCount;
    const float ww = kb.getWidth() / (float) whiteCount;

    auto keyCentre = [&] (int n)
    {
        int whitesBefore = 0;
        for (int m = kbLow; m < n; ++m)
            if (! isBlackKey (m)) ++whitesBefore;
        return kb.getX() + ww * (float) whitesBefore + (isBlackKey (n) ? 0.0f : ww * 0.5f);
    };
    std::array<float, kbHigh - kbLow + 2> centres {};
    for (int n = kbLow; n <= kbHigh + 1; ++n)
        centres[(size_t) (n - kbLow)] = keyCentre (n);

    auto noteX = [&] (float m)
    {
        if (m <= (float) kbLow)  return centres.front();
        if (m >= (float) kbHigh) return centres[(size_t) (kbHigh - kbLow)] + (m - (float) kbHigh) * ww * 0.58f;
        const int i = (int) std::floor (m);
        const float f = m - (float) i;
        return centres[(size_t) (i - kbLow)] * (1.0f - f) + centres[(size_t) (i - kbLow + 1)] * f;
    };

    // オクターブの区切り
    g.setColour (col::lcdGrid);
    for (int n = kbLow; n <= kbHigh; n += 12)
    {
        const float x = noteX ((float) n) - ww * 0.5f;
        g.drawVerticalLine ((int) x, specTop, specBot);
    }
    g.drawHorizontalLine ((int) specBot, kb.getX(), kb.getRight());

    // 鳴っているボイス
    std::array<int, 128> keyState {};        // 0 = なし, 1 = 余韻だけ, 2 = 押している
    std::array<float, 128> keyBright {};
    auto get = [this] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
    const auto mode  = (chordres::Mode) juce::jlimit (0, 2, juce::roundToInt (get ("mode")));
    const auto ftype = (chordres::FilterType) juce::jlimit (0, 4, juce::roundToInt (get ("ftype")));
    const float fcut = get ("fcut"), freso = get ("freso"), fvowel = get ("vowel");
    const double fs = proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 48000.0;
    auto filterAt = [&] (float midiNote)
    {
        return chordres::filterMagnitude (ftype, fcut, freso, fvowel, chordres::noteToHz (midiNote), fs);
    };

    // フィルターのカーブ (0dB が表示の半分の高さ、+12dB で上端)
    if (ftype != chordres::FilterType::off)
    {
        juce::Path curve;
        for (int px = 0; px <= (int) kb.getWidth(); px += 2)
        {
            const float x = kb.getX() + (float) px;
            // x → ノート番号 (noteX の逆。鍵盤上ではほぼ線形なので近似で十分)
            const float m = (float) kbLow + (x - kb.getX()) / kb.getWidth() * (float) (kbHigh - kbLow + 1) - 0.5f;
            const float db = juce::Decibels::gainToDecibels (filterAt (m), -60.0f);
            const float y = juce::jlimit (specTop, specBot, specBot - (specBot - specTop) * 0.5f * (1.0f + db / 12.0f));
            if (px == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
        }
        g.setColour (col::accent.withAlpha (0.35f));
        g.strokePath (curve, juce::PathStrokeType (1.2f));
    }
    const float offset = get ("transpose") + get ("fine") / 100.0f;
    const int harmonics = (int) get ("harm");
    const float stretchV = get ("stretch"), brightV = get ("bright");

    std::array<float, chordres::maxDisplayPartials> pos {}, amp {};
    const float specH = specBot - specTop;

    for (size_t i = 0; i < shownNotes.size(); ++i)
    {
        const int v = shownNotes[i];
        if (v < 0) continue;
        const bool held = v < 1000;
        const int n = v % 1000;
        const float b = levelToBrightness (shownLevel[i]);

        keyState[(size_t) n] = juce::jmax (keyState[(size_t) n], held ? 2 : 1);
        keyBright[(size_t) n] = juce::jmax (keyBright[(size_t) n], b);

        // 倍音の位置。入力が無くても位置が見えるように最低 25% の高さは出す
        const int count = chordres::displayPartials (n + offset, mode, harmonics, stretchV, brightV, pos, amp);
        g.setColour (col::partial.withAlpha (held ? 1.0f : 0.45f));
        for (int k = 0; k < count; ++k)
        {
            const float x = noteX (pos[(size_t) k]);
            if (x > kb.getRight()) break;
            const float h = juce::jmin (specH, specH * amp[(size_t) k] * filterAt (pos[(size_t) k]) * (0.25f + 0.75f * b));
            g.fillRect (juce::Rectangle<float> (x - 0.75f, specBot - h, 1.5f, h));
        }
    }

    // 白鍵
    for (int n = kbLow; n <= kbHigh; ++n)
    {
        if (isBlackKey (n)) continue;
        const auto r = juce::Rectangle<float> (keyCentre (n) - ww * 0.5f + 0.5f, kb.getY(), ww - 1.0f, kb.getHeight());
        g.setColour (col::keyWhite);
        g.fillRect (r);
        if (const int st = keyState[(size_t) n])
        {
            g.setColour (col::accent.withAlpha ((st == 2 ? 0.35f : 0.15f) + 0.65f * keyBright[(size_t) n]));
            g.fillRect (r);
        }
        if (n % 12 == 0)
        {
            g.setColour (col::keyBlack);
            g.setFont (juce::FontOptions (8.5f));
            g.drawText (noteName (n), r.withTrimmedTop (r.getHeight() - 12.0f), juce::Justification::centred);
        }
    }

    // 黒鍵
    for (int n = kbLow; n <= kbHigh; ++n)
    {
        if (! isBlackKey (n)) continue;
        const auto r = juce::Rectangle<float> (keyCentre (n) - ww * 0.32f, kb.getY(), ww * 0.64f, kb.getHeight() * 0.6f);
        g.setColour (col::keyBlack);
        g.fillRect (r);
        if (const int st = keyState[(size_t) n])
        {
            g.setColour (col::accent.withAlpha ((st == 2 ? 0.35f : 0.15f) + 0.65f * keyBright[(size_t) n]));
            g.fillRect (r);
        }
    }

    // ノート名
    juce::StringArray held, ringing;
    for (int n = 0; n < 128; ++n)
    {
        if (keyState[(size_t) n] == 2) held.add (noteName (n));
        else if (keyState[(size_t) n] == 1) ringing.add (noteName (n));
    }

    g.setFont (juce::FontOptions (10.5f));
    const auto line = juce::Rectangle<float> (inner.getX(), inner.getY(), inner.getWidth(), 12.0f);
    if (held.isEmpty() && ringing.isEmpty())
    {
        g.setColour (col::dim);
        g.drawText ("No notes  -  route MIDI: MIDI track > MIDI To > this track > ChordRes", line, juce::Justification::centredLeft);
    }
    else
    {
        g.setColour (col::dim);
        g.drawText ("Notes", line, juce::Justification::centredLeft);
        g.setColour (col::text);
        g.drawText (held.joinIntoString ("  "), line.withTrimmedLeft (38.0f), juce::Justification::centredLeft);
        if (! ringing.isEmpty())
        {
            g.setColour (col::dim);
            g.drawText ("Ringing  " + ringing.joinIntoString ("  "), line, juce::Justification::centredRight);
        }
    }
}

void ChordResAudioProcessorEditor::paintMeters (juce::Graphics& g)
{
    const auto area = meterArea.toFloat();
    g.setColour (col::lcd);
    g.fillRoundedRectangle (area, 2.0f);

    const float top = area.getY() + 10.0f, bottom = area.getBottom() - 22.0f;
    const float barW = 10.0f, gapW = 13.0f;
    const float x0 = area.getCentreX() - (3.0f * barW + 2.0f * gapW) * 0.5f;

    auto dbToY = [&] (float db)
    {
        const float t = juce::jlimit (0.0f, 1.0f, (db - meterMinDb) / (meterMaxDb - meterMinDb));
        return bottom - t * (bottom - top);
    };
    const float zeroY = dbToY (0.0f);

    const std::array<std::pair<const char*, float>, 3> meters { { { "IN", meterIn }, { "WET", meterWet }, { "OUT", meterOut } } };
    for (size_t i = 0; i < meters.size(); ++i)
    {
        const float x = x0 + (float) i * (barW + gapW);
        const auto bar = juce::Rectangle<float> (x, top, barW, bottom - top);
        g.setColour (col::lcdGrid);
        g.fillRect (bar);

        const float y = dbToY (meters[i].second);
        if (y < bottom)
        {
            g.setColour (col::meter);
            g.fillRect (bar.withTop (juce::jmax (y, zeroY)));
            if (y < zeroY)
            {
                g.setColour (col::over);
                g.fillRect (bar.withTop (y).withBottom (zeroY));
            }
        }

        if (i == 1 && holdWet > meterMinDb)
        {
            g.setColour (holdWet > 0.0f ? col::over : col::text);
            g.fillRect (juce::Rectangle<float> (x - 2.0f, dbToY (holdWet) - 0.75f, barW + 4.0f, 1.5f));
        }

        g.setColour (col::dim);
        g.setFont (juce::FontOptions (9.5f));
        g.drawText (meters[i].first, juce::Rectangle<float> (x - 8.0f, bottom + 4.0f, barW + 16.0f, 12.0f),
                    juce::Justification::centred);
    }

    // 0dBFS の目盛り
    g.setColour (col::dim.withAlpha (0.6f));
    g.drawHorizontalLine ((int) zeroY, area.getX() + 4.0f, x0 - 3.0f);
}

void ChordResAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (16, 0);
    area.removeFromTop (34);

    auto top = area.removeFromTop (150);
    meterArea = top.removeFromRight (82);
    top.removeFromRight (10);
    displayArea = top;

    area.removeFromTop (12);
    area.removeFromBottom (14);

    // 4 つのパネルを 2 : 3 : 3 : 2 列で並べる
    constexpr int gap = 10;
    const int colW = (area.getWidth() - 3 * gap) / 10;
    envPanel    = area.removeFromLeft (colW * 2);
    area.removeFromLeft (gap);
    tonePanel   = area.removeFromLeft (colW * 3);
    area.removeFromLeft (gap);
    filterPanel = area.removeFromLeft (colW * 3);
    area.removeFromLeft (gap);
    masterPanel = area;

    // パネルのタイトル行の右端にスイッチ
    auto placeSwitch = [] (Switch& sw, juce::Rectangle<int> panelArea, int width)
    {
        auto r = panelArea.withTrimmedTop (6).withHeight (18).withTrimmedRight (8);
        sw.setBounds (r.removeFromRight (width));
    };
    placeSwitch (noteOffSwitch, envPanel, 88);
    placeSwitch (modeSwitch, tonePanel, 150);
    // FILTER は種類が多いので、タイトルの下の行に全幅で
    filterSwitch.setBounds (filterPanel.reduced (8, 0).withTrimmedTop (30).withHeight (18));

    auto layoutGrid = [] (juce::Rectangle<int> panelArea, std::initializer_list<Knob*> knobs, int cols, int rows = 2)
    {
        auto inner = panelArea.reduced (4, 6).withTrimmedTop (22);
        const int w = inner.getWidth() / cols;
        const int h = inner.getHeight() / rows;
        int i = 0;
        for (auto* k : knobs)
        {
            auto cell = juce::Rectangle<int> (inner.getX() + (i % cols) * w, inner.getY() + (i / cols) * h, w, h)
                            .withSizeKeepingCentre (w, juce::jmin (h, 92));
            k->label.setBounds (cell.removeFromTop (14));
            k->slider.setBounds (cell.reduced (2, 0));
            ++i;
        }
        return inner;
    };

    layoutGrid (envPanel,  { &attack, &decay, &amount }, 2);
    layoutGrid (tonePanel, { &bright, &harm, &stretch, &drive, &transpose, &fine }, 3);
    layoutGrid (filterPanel.withTrimmedTop (26), { &freq, &reso, &vowel }, 3, 1);
    auto inner = layoutGrid (masterPanel, { &width, &mix, &output }, 2);

    // Safety は MASTER の右下のマス
    const int w = inner.getWidth() / 2, h = inner.getHeight() / 2;
    safetyBtn.setBounds (juce::Rectangle<int> (inner.getX() + w, inner.getY() + h, w, h).withSizeKeepingCentre (64, 20));
}
