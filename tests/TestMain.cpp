//  ChordResTest — DAW を開かずに挙動を確かめるオフラインドライバ
//
//   ./ChordResTest   … 音程・音量・ノートオフ挙動・CPU を測り、UI を chordres_ui.png に保存

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

namespace
{
    constexpr double sr = 48000.0;
    constexpr int block = 256;

    void setParam (ChordResAudioProcessor& p, const char* id, float value)
    {
        auto* param = p.apvts.getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    std::unique_ptr<ChordResAudioProcessor> makeProc()
    {
        auto p = std::make_unique<ChordResAudioProcessor>();
        p->setPlayConfigDetails (2, 2, sr, block);
        p->prepareToPlay (sr, block);
        setParam (*p, "width", 0.0f);
        setParam (*p, "mix", 1.0f);
        setParam (*p, "safety", 0.0f);
        return p;
    }

    struct Event { int sample; juce::MidiMessage msg; };

    // input を block ごとに流し、events を該当位置に入れる。出力を返す
    juce::AudioBuffer<float> run (ChordResAudioProcessor& p, const juce::AudioBuffer<float>& input,
                                  const std::vector<Event>& events)
    {
        const int n = input.getNumSamples();
        juce::AudioBuffer<float> out (2, n);
        juce::AudioBuffer<float> buf (2, block);

        for (int start = 0; start < n; start += block)
        {
            const int len = juce::jmin (block, n - start);
            buf.setSize (2, len, false, false, true);
            for (int ch = 0; ch < 2; ++ch)
                buf.copyFrom (ch, 0, input, ch, start, len);

            juce::MidiBuffer midi;
            for (auto& e : events)
                if (e.sample >= start && e.sample < start + len)
                    midi.addEvent (e.msg, e.sample - start);

            p.processBlock (buf, midi);

            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, start, buf, ch, 0, len);
        }
        return out;
    }

    juce::AudioBuffer<float> noise (int n, float amp = 0.25f, int seed = 1)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (2, n);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
                b.setSample (ch, i, amp * (r.nextFloat() * 2.0f - 1.0f));
        return b;
    }

    double rms (const juce::AudioBuffer<float>& b, int ch, int start, int len)
    {
        double s = 0.0;
        for (int i = start; i < start + len; ++i)
            s += (double) b.getSample (ch, i) * b.getSample (ch, i);
        return std::sqrt (s / len);
    }

    double db (double x) { return 20.0 * std::log10 (juce::jmax (1e-12, x)); }

    // start から 2^order サンプルの区間で、fLo..fHi にある最大ピークの周波数 (放物線補間)
    double peakHz (const juce::AudioBuffer<float>& b, int start, double fLo, double fHi)
    {
        constexpr int order = 16;
        constexpr int n = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> data (2 * n, 0.0f);
        for (int i = 0; i < n; ++i)
        {
            const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (n - 1));
            data[(size_t) i] = (float) (b.getSample (0, start + i) * w);
        }
        fft.performFrequencyOnlyForwardTransform (data.data());

        const int lo = (int) (fLo * n / sr), hi = (int) (fHi * n / sr);
        int best = lo;
        for (int k = lo; k <= hi; ++k)
            if (data[(size_t) k] > data[(size_t) best]) best = k;

        const double a = std::log (data[(size_t) best - 1] + 1e-20);
        const double c = std::log (data[(size_t) best] + 1e-20);
        const double d = std::log (data[(size_t) best + 1] + 1e-20);
        const double delta = 0.5 * (a - d) / (a - 2.0 * c + d);
        return (best + delta) * sr / n;
    }

    bool finite (const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite (b.getSample (ch, i))) return false;
        return true;
    }

    const char* modeName (int m) { return m == 0 ? "String" : m == 1 ? "Bank  " : "Tube  "; }
}

//==============================================================================
static juce::AudioBuffer<float> impulse (int n)
{
    juce::AudioBuffer<float> b (2, n);
    b.clear();
    b.setSample (0, 100, 1.0f);
    b.setSample (1, 100, 1.0f);
    return b;
}

// 50ms 窓の RMS を 2 点で測って、その傾きから T60 を出す
static double measureT60 (const juce::AudioBuffer<float>& src, double t1, double t2)
{
    // String は 0Hz 近くに小さなモードが残るので、20Hz 以下を落としてから測る
    juce::AudioBuffer<float> b (1, src.getNumSamples());
    b.copyFrom (0, 0, src, 0, 0, src.getNumSamples());
    for (int pass = 0; pass < 2; ++pass)
    {
        juce::IIRFilter hp;
        hp.setCoefficients (juce::IIRCoefficients::makeHighPass (sr, 20.0));
        hp.processSamples (b.getWritePointer (0), b.getNumSamples());
    }
    const int w = (int) (sr * 0.05);
    const double r1 = rms (b, 0, (int) (t1 * sr), w);
    const double r2 = rms (b, 0, (int) (t2 * sr), w);
    const double dbPerSec = (db (r2) - db (r1)) / (t2 - t1);
    return dbPerSec < 0.0 ? -60.0 / dbPerSec : 1.0e9;
}

static void testTuning()
{
    std::printf ("\n[1] Tuning + decay (impulse in, 1 note held, Decay 2s)\n");
    std::printf ("  mode  bright note  expected Hz   measured Hz  cents     T60\n");

    for (int mode : { 0, 2, 1 })
        for (float bright : { 0.0f, 0.6f, 1.0f })
            for (int note : { 24, 36, 48, 60, 72, 84, 96, 108 })
            {
                auto p = makeProc();
                setParam (*p, "mode", (float) mode);
                setParam (*p, "decay", 2.0f);
                setParam (*p, "bright", bright);

                const int n = (int) sr * 3;
                auto out = run (*p, impulse (n), { { 0, juce::MidiMessage::noteOn (1, note, (juce::uint8) 100) } });

                const double expect = chordres::noteToHz (note);
                const double got = peakHz (out, (int) (sr * 0.02), expect * 0.9, expect * 1.1);
                const double cents = 1200.0 * std::log2 (got / expect);
                std::printf ("  %s  %.1f  %3d   %10.3f   %10.3f   %+6.2f  %5.2fs%s\n", modeName (mode), bright, note,
                             expect, got, cents, measureT60 (out, 0.3, 0.8),
                             std::abs (cents) > 3.0 ? "  <-- off" : "");
            }
}

static void testStability()
{
    std::printf ("\n[1b] Stability: 60 s of noise, Decay 20s, every Bright, 4 notes. Output peak per 10 s (must not grow)\n");
    const int n = (int) sr * 60;
    auto in = noise (n, 0.25f, 7);
    for (int mode : { 0, 2, 1 })
        for (float bright : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            auto p = makeProc();
            setParam (*p, "mode", (float) mode);
            setParam (*p, "decay", 20.0f);
            setParam (*p, "bright", bright);
            auto out = run (*p, in, { { 0, juce::MidiMessage::noteOn (1, 28, (juce::uint8) 100) },
                                      { 0, juce::MidiMessage::noteOn (1, 57, (juce::uint8) 100) },
                                      { 0, juce::MidiMessage::noteOn (1, 83, (juce::uint8) 100) },
                                      { 0, juce::MidiMessage::noteOn (1, 107, (juce::uint8) 100) } });
            std::printf ("  %s bright %.2f :", modeName (mode), bright);
            for (int s = 0; s < 6; ++s)
                std::printf (" %+6.1f", db (out.getMagnitude (0, s * (int) sr * 10, (int) sr * 10)));
            std::printf ("  finite=%s\n", finite (out) ? "yes" : "NO");
        }
}

static void testLevel()
{
    std::printf ("\n[2] Level: white noise in -> out (1 note, A3), RMS dB relative to input\n");
    const int n = (int) sr * 4;
    auto in = noise (n);
    const double inRms = rms (in, 0, n / 2, n / 2);

    for (int mode = 0; mode < 2; ++mode)
        for (float decay : { 0.2f, 2.0f, 20.0f })
            for (float bright : { 0.0f, 0.6f, 1.0f })
            {
                auto p = makeProc();
                setParam (*p, "mode", (float) mode);
                setParam (*p, "decay", decay);
                setParam (*p, "bright", bright);
                auto out = run (*p, in, { { 0, juce::MidiMessage::noteOn (1, 57, (juce::uint8) 100) } });
                std::printf ("  %s decay %5.1fs bright %.1f : %+6.1f dB\n", modeName (mode), decay, bright,
                             db (rms (out, 0, n / 2, n / 2) / inRms));
            }

    std::printf ("\n    worst case: sine exactly at the note (A3 220Hz, -12dBFS), Decay 20s, Safety OFF/ON\n");
    for (int mode = 0; mode < 2; ++mode)
        for (int safety : { 0, 1 })
        {
            auto p = makeProc();
            setParam (*p, "mode", (float) mode);
            setParam (*p, "decay", 20.0f);
            setParam (*p, "bright", 1.0f);
            setParam (*p, "safety", (float) safety);

            juce::AudioBuffer<float> sine (2, n);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i)
                    sine.setSample (ch, i, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / sr));

            auto out = run (*p, sine, { { 0, juce::MidiMessage::noteOn (1, 57, (juce::uint8) 100) } });
            std::printf ("  %s safety %s : peak %+6.1f dBFS\n", modeName (mode), safety ? "ON " : "OFF",
                         db (out.getMagnitude (0, 0, n)));
        }
}

static void testNoteOff()
{
    std::printf ("\n[3] Note Off (C-E-G held 1s then released, noise keeps coming in, Decay 1s)\n");
    const int n = (int) sr * 4;
    auto in = noise (n);
    const int off = (int) sr;

    std::vector<Event> ev;
    for (int note : { 60, 64, 67 })
    {
        ev.push_back ({ 0,   juce::MidiMessage::noteOn  (1, note, (juce::uint8) 100) });
        ev.push_back ({ off, juce::MidiMessage::noteOff (1, note) });
    }
    // 3 秒目に単音 D を押す (Hold ならここで C-E-G が入れ替わる)
    ev.push_back ({ 3 * (int) sr, juce::MidiMessage::noteOn (1, 62, (juce::uint8) 100) });

    for (int mode = 0; mode < 2; ++mode)
        for (int hold = 0; hold < 2; ++hold)
        {
            auto p = makeProc();
            setParam (*p, "mode", (float) mode);
            setParam (*p, "hold", (float) hold);
            setParam (*p, "decay", 1.0f);

            auto out = run (*p, in, ev);
            const int w = 2400;
            std::printf ("  %s %-7s: held %+6.1f | +0.1s %+6.1f | +0.5s %+6.1f | +0.9s %+6.1f dB | T60 after off %5.2fs | voices at end %d\n",
                         modeName (mode), hold ? "Hold" : "Key",
                         db (rms (out, 0, (int) (sr * 0.5), w)),
                         db (rms (out, 0, (int) (sr * 1.1), w)),
                         db (rms (out, 0, (int) (sr * 1.5), w)),
                         db (rms (out, 0, (int) (sr * 1.9), w)),
                         measureT60 (out, 1.1, 1.6),
                         p->activeVoiceCount());
        }
    std::printf ("  (expect: Key -> decays with T60 ~1s although noise continues; Hold -> stays at held level)\n");
}

static void testAmountDrive()
{
    std::printf ("\n[3b] Amount / Drive (Bank, C-E-G-B chord, noise in). Wet peak before Drive is printed for reference\n");
    const int n = (int) sr * 2;
    std::vector<Event> chord;
    for (int note : { 60, 64, 67, 71 })
        chord.push_back ({ 0, juce::MidiMessage::noteOn (1, note, (juce::uint8) 100) });

    for (float inDb : { -18.0f, -6.0f })
    {
        auto in = noise (n, juce::Decibels::decibelsToGain (inDb));
        auto measure = [&] (float amount, float drive, double& rmsOut, double& peakOut, double& diffDb)
        {
            auto p = makeProc();
            setParam (*p, "amount", amount);
            setParam (*p, "sat", drive);
            auto out = run (*p, in, chord);

            // Drive 0dB (0dBFS でも 0.13% しか丸まらない = ほぼ線形) の出力との差を歪みとみなす
            auto pl = makeProc();
            setParam (*pl, "amount", amount);
            auto lin = run (*pl, in, chord);
            // 音量差は除く (最小二乗で合わせたゲインを掛けた残差)
            double xy = 0.0, xx = 0.0;
            for (int i = n / 2; i < n; ++i)
            {
                xy += (double) out.getSample (0, i) * lin.getSample (0, i);
                xx += (double) lin.getSample (0, i) * lin.getSample (0, i);
            }
            const double a = xy / juce::jmax (1e-30, xx);
            double e = 0.0, r = 0.0;
            for (int i = n / 2; i < n; ++i)
            {
                const double ref = a * lin.getSample (0, i);
                e += (out.getSample (0, i) - ref) * (out.getSample (0, i) - ref);
                r += ref * ref;
            }
            diffDb = db (std::sqrt (e / juce::jmax (1e-30, r)));
            rmsOut = rms (out, 0, n / 2, n / 2);
            peakOut = out.getMagnitude (0, n / 2, n / 2);
            return finite (out);
        };

        double r0, p0, d0;
        measure (0.0f, 0.0f, r0, p0, d0);
        std::printf ("  noise %+.0f dBFS in -> wet peak %+5.1f dBFS\n", inDb, db (p0));

        for (float drive : { 0.0f, 3.0f, 6.0f, 12.0f, 24.0f, 36.0f, 48.0f })
        {
            double r, pk, d;
            const bool ok = measure (0.0f, drive, r, pk, d);
            std::printf ("    Drive %4.0f dB : rms %+6.1f dB  peak %+6.1f dBFS  distortion (vs clean) %+6.1f dB%s\n",
                         drive, db (r / r0), db (pk), d, ok ? "" : "  NaN!");
        }
    }
    {
        auto in = noise (n);
        auto p = makeProc();
        setParam (*p, "amount", -6.0f);
        auto a = run (*p, in, chord);
        auto p2 = makeProc();
        auto b = run (*p2, in, chord);
        std::printf ("  Amount -6dB : %+.2f dB\n", db (rms (a, 0, n / 2, n / 2) / rms (b, 0, n / 2, n / 2)));
    }
}

static void testTube()
{
    std::printf ("\n[1c] Tube: odd harmonics only (impulse in, A2 110Hz, Bright 1, Decay 2s). Level of partial k rel. to k=1\n");
    for (int mode : { 0, 2 })
    {
    auto p = makeProc();
    setParam (*p, "mode", (float) mode);
    setParam (*p, "bright", 1.0f);
    setParam (*p, "decay", 2.0f);
    const int n = (int) sr * 3;
    auto out = run (*p, impulse (n), { { 0, juce::MidiMessage::noteOn (1, 45, (juce::uint8) 100) } });

    constexpr int order = 16, len = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> d (2 * len, 0.0f);
    const int st = (int) (sr * 0.05);
    for (int i = 0; i < len; ++i)
        d[(size_t) i] = out.getSample (0, st + i) * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (len - 1)));
    fft.performFrequencyOnlyForwardTransform (d.data());
    // 倍音は少し低めにずれるので ±2% (±34 セント) の範囲で最大を探し、ずれも記録する
    std::array<double, 9> detune {};
    auto peakNear = [&] (double hz, int k = 0)
    {
        const int lo = (int) (hz * 0.98 * len / sr), hi = (int) (hz * 1.02 * len / sr);
        int best = lo;
        for (int b = lo; b <= hi; ++b) if (d[(size_t) b] > d[(size_t) best]) best = b;
        if (k > 0) detune[(size_t) k] = 1200.0 * std::log2 (best * sr / len / hz);
        return d[(size_t) best];
    };
    const float ref = peakNear (110.0);
    std::printf ("  %s", modeName (mode));
    for (int k = 1; k <= 8; ++k)
        std::printf (" k%d %+6.1f", k, db (peakNear (110.0 * k, k) / ref));
    std::printf ("\n     detune (cents, bin-limited +-1.6):");
    for (int k = 1; k <= 8; k += (mode == 2 ? 2 : 1))
        std::printf (" k%d %+5.1f", k, detune[(size_t) k]);
    std::printf ("\n");
    }
    std::printf ("  (expect: Tube even k far below odd k)\n");
}

static void testFilter()
{
    std::printf ("\n[3c] Wet filter (Bank, Harmonics 24, Bright 1, chord C2-E2-G2, noise in). Level per band rel. to Filter Off\n");
    const int n = (int) sr * 2;
    auto in = noise (n);
    std::vector<Event> chord { { 0, juce::MidiMessage::noteOn (1, 36, (juce::uint8) 100) },
                               { 0, juce::MidiMessage::noteOn (1, 40, (juce::uint8) 100) },
                               { 0, juce::MidiMessage::noteOn (1, 43, (juce::uint8) 100) } };

    auto bandLevels = [&] (int type, float cut, float reso, float vowel, std::array<double, 6>& lv, double& total)
    {
        auto p = makeProc();
        setParam (*p, "harm", 24.0f);
        setParam (*p, "bright", 1.0f);
        setParam (*p, "ftype", (float) type);
        setParam (*p, "fcut", cut);
        setParam (*p, "freso", reso);
        setParam (*p, "vowel", vowel);
        auto out = run (*p, in, chord);

        constexpr int order = 15, len = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> d (2 * len, 0.0f);
        for (int i = 0; i < len; ++i)
            d[(size_t) i] = out.getSample (0, n / 2 + i);
        fft.performFrequencyOnlyForwardTransform (d.data());
        const double edges[7] = { 60, 250, 500, 1000, 2000, 4000, 8000 };
        for (int b = 0; b < 6; ++b)
        {
            double e = 0.0;
            for (int k = (int) (edges[b] * len / sr); k < (int) (edges[b + 1] * len / sr); ++k) e += (double) d[(size_t) k] * d[(size_t) k];
            lv[(size_t) b] = e;
        }
        total = rms (out, 0, n / 2, n / 2);
        return finite (out);
    };

    std::array<double, 6> ref {};
    double refTotal;
    bandLevels (0, 1000.0f, 0.2f, 0.0f, ref, refTotal);
    std::printf ("                         60-250  250-500  500-1k   1k-2k   2k-4k   4k-8k   total\n");

    struct Case { const char* name; int type; float cut, reso, vowel; };
    for (auto c : { Case { "LP 1kHz reso 0.2",      1, 1000.0f, 0.2f, 0.0f },
                    Case { "HP 1kHz reso 0.2",      2, 1000.0f, 0.2f, 0.0f },
                    Case { "Notch 1kHz reso 0.2",   3, 1000.0f, 0.2f, 0.0f },
                    Case { "LP 1kHz reso 0.8",      1, 1000.0f, 0.8f, 0.0f },
                    Case { "Formant a",             4, 1000.0f, 0.2f, 0.0f },
                    Case { "Formant i",             4, 1000.0f, 0.2f, 2.0f },
                    Case { "Formant u",             4, 1000.0f, 0.2f, 4.0f } })
    {
        std::array<double, 6> lv {};
        double total;
        const bool ok = bandLevels (c.type, c.cut, c.reso, c.vowel, lv, total);
        std::printf ("  %-22s", c.name);
        for (int b = 0; b < 6; ++b)
            std::printf (" %+7.1f", 10.0 * std::log10 (juce::jmax (1e-30, lv[(size_t) b]) / ref[(size_t) b]));
        std::printf ("  %+6.1f%s\n", db (total / refTotal), ok ? "" : "  NaN!");
    }
}

static void testCpu()
{
    std::printf ("\n[4] CPU: 12 voices, 10 s stereo @48k\n");
    const int n = (int) sr * 10;
    auto in = noise (n);
    std::vector<Event> ev;
    for (int i = 0; i < 12; ++i)
        ev.push_back ({ 0, juce::MidiMessage::noteOn (1, 48 + i * 3, (juce::uint8) 100) });

    for (int mode = 0; mode < 2; ++mode)
    {
        auto p = makeProc();
        setParam (*p, "mode", (float) mode);
        setParam (*p, "harm", 24.0f);
        setParam (*p, "bright", 1.0f);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto out = run (*p, in, ev);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("  %s : %.0f ms for 10 s audio (%.2f%% of realtime), finite=%s\n",
                     modeName (mode), ms, ms / 100.0, finite (out) ? "yes" : "NO");
    }
}

static void snapshotUi()
{
    auto p = makeProc();
    setParam (*p, "ftype", 1.0f);
    setParam (*p, "fcut", 1500.0f);
    setParam (*p, "freso", 0.6f);
    // 表示用にコードを鳴らしておく
    auto in = noise (block * 4);
    run (*p, in, { { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
                   { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100) },
                   { 0, juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100) },
                   { 0, juce::MidiMessage::noteOn (1, 71, (juce::uint8) 100) } });

    std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditor());
    ed->setVisible (true);
    // タイマーを待たずに表示を更新
    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile ("chordres_ui.png");
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat().writeImageToStream (img, os);
    std::printf ("\n[5] UI snapshot -> %s\n", f.getFullPathName().toRawUTF8());
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    testTuning();
    testStability();
    testTube();
    testLevel();
    testNoteOff();
    testAmountDrive();
    testFilter();
    testCpu();
    snapshotUi();
    return 0;
}
