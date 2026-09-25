#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>

//  共鳴音 (Wet) にだけかけるフィルター。Drive の後、Mix の前。
//
//  LP / HP / Notch : 12dB/oct の状態変数フィルター (TPT 形式)。カットオフを動かしてもプチッと鳴りにくい
//  Formant         : F1〜F3 を並列のバンドパスで作り、母音 a-e-i-o-u の間を連続的に移動する
namespace chordres
{
    enum class FilterType { off = 0, lp = 1, hp = 2, notch = 3, formant = 4 };

    struct Formant { float f[3]; float gainDb[3]; };

    // 成人男性の母音の第 1〜第 3 フォルマント (Peterson & Barney 1952 の平均値に近い代表値)
    inline const std::array<Formant, 5>& vowelTable()
    {
        static const std::array<Formant, 5> t { {
            { { 730.0f, 1090.0f, 2440.0f }, { 0.0f,  -4.0f, -12.0f } },   // a
            { { 530.0f, 1840.0f, 2480.0f }, { 0.0f,  -6.0f, -10.0f } },   // e
            { { 270.0f, 2290.0f, 3010.0f }, { 0.0f, -10.0f, -12.0f } },   // i
            { { 570.0f,  840.0f, 2410.0f }, { 0.0f,  -3.0f, -16.0f } },   // o
            { { 300.0f,  870.0f, 2240.0f }, { 0.0f,  -8.0f, -18.0f } },   // u
        } };
        return t;
    }

    // vowel = 0..4 (a, e, i, o, u) の間を、周波数は対数で、ゲインは dB で補間
    inline Formant interpolateVowel (float vowel)
    {
        const auto& t = vowelTable();
        const float v = juce::jlimit (0.0f, 4.0f, vowel);
        const int i = juce::jmin (3, (int) std::floor (v));
        const float fr = v - (float) i;
        Formant out {};
        for (int k = 0; k < 3; ++k)
        {
            out.f[k] = std::exp (std::log (t[(size_t) i].f[k]) * (1.0f - fr) + std::log (t[(size_t) i + 1].f[k]) * fr);
            out.gainDb[k] = t[(size_t) i].gainDb[k] * (1.0f - fr) + t[(size_t) i + 1].gainDb[k] * fr;
        }
        return out;
    }

    // Reso (0..1) → Q。LP/HP/Notch は 0.5〜12、Formant は 3〜24
    inline float resoToQ (FilterType type, float reso)
    {
        return type == FilterType::formant ? 3.0f * std::pow (8.0f, reso)
                                           : 0.5f * std::pow (24.0f, reso);
    }

    // Formant は帯域を切り出すぶん音量が下がるので持ち上げる (ノイズ入力で Off と揃うように実測で決めた値)
    constexpr float formantMakeupDb = 9.0f;

    // 表示用: 周波数 hz での振幅 (リニア)。双一次変換の周波数ゆがみを含めた厳密値
    inline float filterMagnitude (FilterType type, float cutoff, float reso, float vowel, double hz, double fs)
    {
        if (type == FilterType::off || hz >= fs * 0.5)
            return 1.0f;

        const double q = resoToQ (type, reso);
        auto warped = [fs] (double f) { return std::tan (juce::MathConstants<double>::pi * juce::jmin (f, fs * 0.499) / fs); };
        auto svf = [&] (double fc, int kind)   // 0 = LP, 1 = HP, 2 = Notch, 3 = BP (ピーク 1)
        {
            const double x = warped (hz) / warped (fc);
            const double den = std::sqrt ((1.0 - x * x) * (1.0 - x * x) + (x / q) * (x / q));
            switch (kind)
            {
                case 0:  return 1.0 / den;
                case 1:  return x * x / den;
                case 2:  return std::abs (1.0 - x * x) / den;
                default: return (x / q) / den;
            }
        };

        switch (type)
        {
            case FilterType::lp:    return (float) svf (cutoff, 0);
            case FilterType::hp:    return (float) svf (cutoff, 1);
            case FilterType::notch: return (float) svf (cutoff, 2);
            case FilterType::formant:
            {
                // 並列なので位相まで含めて足すべきだが、表示用なので振幅の和で近似
                const auto fm = interpolateVowel (vowel);
                double sum = 0.0;
                for (int k = 0; k < 3; ++k)
                    sum += juce::Decibels::decibelsToGain ((double) fm.gainDb[k]) * svf (fm.f[k], 3);
                return (float) (sum * juce::Decibels::decibelsToGain ((double) formantMakeupDb));
            }
            case FilterType::off:
            default:                return 1.0f;
        }
    }

    class WetFilter
    {
    public:
        void prepare (double sampleRate)
        {
            fs = sampleRate;
            reset();
        }

        void reset()
        {
            for (auto& ch : state)
                for (auto& s : ch)
                    s = {};
        }

        // ブロックの頭で呼ぶ。type が変わったら状態を捨てる (別の形のフィルターの状態は使い回せない)
        void setType (FilterType t)
        {
            if (t != type)
                reset();
            type = t;
        }

        // 係数は 16 サンプルごとに更新する (カットオフのスムージングはプロセッサ側)
        void setParams (float cutoff, float reso, float vowel)
        {
            const float q = resoToQ (type, reso);
            if (type == FilterType::formant)
            {
                const auto fm = interpolateVowel (vowel);
                for (int k = 0; k < 3; ++k)
                {
                    coef[(size_t) k] = makeCoef (fm.f[k], q);
                    gain[(size_t) k] = juce::Decibels::decibelsToGain (fm.gainDb[k] + formantMakeupDb);
                }
            }
            else
            {
                coef[0] = makeCoef (cutoff, q);
            }
        }

        inline float process (int ch, float x) noexcept
        {
            switch (type)
            {
                case FilterType::lp:    return tick (state[(size_t) ch][0], coef[0], x).low;
                case FilterType::hp:    return tick (state[(size_t) ch][0], coef[0], x).high;
                case FilterType::notch: { const auto o = tick (state[(size_t) ch][0], coef[0], x); return o.low + o.high; }
                case FilterType::formant:
                {
                    float y = 0.0f;
                    for (size_t k = 0; k < 3; ++k)
                        y += gain[k] * coef[k].k * tick (state[(size_t) ch][k], coef[k], x).band;   // k 倍でピーク 1 のバンドパス
                    return y;
                }
                case FilterType::off:
                default:                return x;
            }
        }

    private:
        struct Coef  { float k = 1.0f, a1 = 1.0f, a2 = 0.0f, a3 = 0.0f; };
        struct State { float ic1 = 0.0f, ic2 = 0.0f; };
        struct Out   { float low, band, high; };

        Coef makeCoef (float fc, float q) const
        {
            Coef c;
            const double g = std::tan (juce::MathConstants<double>::pi * juce::jlimit (10.0, fs * 0.49, (double) fc) / fs);
            c.k  = 1.0f / q;
            c.a1 = (float) (1.0 / (1.0 + g * (g + c.k)));
            c.a2 = (float) g * c.a1;
            c.a3 = (float) g * c.a2;
            return c;
        }

        // Cytomic (Andrew Simper) の TPT 状態変数フィルター
        static inline Out tick (State& s, const Coef& c, float v0) noexcept
        {
            const float v3 = v0 - s.ic2;
            const float v1 = c.a1 * s.ic1 + c.a2 * v3;
            const float v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
            s.ic1 = 2.0f * v1 - s.ic1;
            s.ic2 = 2.0f * v2 - s.ic2;
            return { v2, v1, v0 - c.k * v1 - v2 };
        }

        double fs = 48000.0;
        FilterType type = FilterType::off;
        std::array<Coef, 3> coef {};
        std::array<float, 3> gain {};
        std::array<std::array<State, 3>, 2> state {};
    };
}
