#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

//  1 ボイス = 1 ノートぶんの共鳴体。時間領域だけで処理する (FFT は使わない)。
//
//  Comb : 1 周期ぶんのディレイ + ループ内ローパスのフィードバック (Karplus-Strong 系)。
//         倍音が全部鳴る。ループ内ローパスの位相遅れを差し引いて音程を合わせる。
//  Bank : 基音と倍音を 2 次レゾネーター (零点を ±1 に置いた BPF) で個別に拾う。
//
//  どちらもホワイトノイズを入れたときの出力 RMS が入力と揃うように正規化している。
//  音程に一致した持続音を入れると大きく持ち上がる (Decay が長いほど) ので、出力段に Safety がある。
namespace chordres
{
    constexpr int maxVoices    = 12;
    constexpr int maxHarmonics = 24;
    constexpr int maxChannels  = 2;
    constexpr int delaySize    = 16384;               // 2 の冪。20Hz @ 192kHz = 9600 サンプルが入る
    constexpr int delayMask    = delaySize - 1;

    // 値はパラメータ "mode" の番号と同じ (保存済みのセットとの互換のため Tube は末尾)
    enum class Mode { comb = 0, bank = 1, tube = 2 };

    struct Settings
    {
        Mode   mode        = Mode::bank;
        double sampleRate  = 48000.0;
        float  decaySec    = 2.0f;     // 基音の T60
        float  bright      = 0.6f;     // 0 = 暗い / 1 = 明るい
        int    harmonics   = 12;       // Bank のみ
        float  stretch     = 0.0f;     // Bank のみ。倍音 k の周波数 = f * k^(1+stretch)
        float  pitchOffset = 0.0f;     // 半音単位 (Transpose + Fine)
        float  widthCents  = 0.0f;     // L/R を ±width/2 セントずらす
        int    numChannels = 2;

        bool operator== (const Settings& o) const
        {
            return mode == o.mode && sampleRate == o.sampleRate && decaySec == o.decaySec
                && bright == o.bright && harmonics == o.harmonics && stretch == o.stretch
                && pitchOffset == o.pitchOffset && widthCents == o.widthCents && numChannels == o.numChannels;
        }
    };

    inline double noteToHz (double note)
    {
        return 440.0 * std::pow (2.0, (note - 69.0) / 12.0);
    }

    // UI 表示用: 1 ノートぶんの共鳴する倍音の位置 (MIDI ノート単位) と、基音を 1 とした大きさの目安。
    // DSP と同じ式から出しているが、String / Tube の大きさはループ内ローパスの振幅だけを見た近似
    constexpr int maxDisplayPartials = 32;
    inline int displayPartials (double note, Mode mode, int harmonics, float stretch, float bright,
                                std::array<float, maxDisplayPartials>& pos,
                                std::array<float, maxDisplayPartials>& amp)
    {
        const double tilt = 1.0 - bright;
        const int nh = mode == Mode::bank ? juce::jlimit (1, maxHarmonics, harmonics) : 2 * maxDisplayPartials;
        const int step = mode == Mode::tube ? 2 : 1;   // Tube は奇数倍音だけ
        const double lpRatio = std::pow (2.0, 1.5 + 6.5 * bright);   // String のループ内ローパス / 基音
        int count = 0;
        for (int k = 1; k <= nh && count < maxDisplayPartials; k += step)
        {
            const double ratio = mode == Mode::bank ? std::pow ((double) k, 1.0 + (double) stretch) : (double) k;
            const double p = note + 12.0 * std::log2 (ratio);
            if (p > 135.0)
                break;
            pos[(size_t) count] = (float) p;
            amp[(size_t) count] = (float) (mode == Mode::bank
                                               ? std::pow ((double) k, -1.5 * tilt)
                                               : 1.0 / std::sqrt (1.0 + (k / lpRatio) * (k / lpRatio)));
            ++count;
        }
        return count;
    }

    class Resonator
    {
    public:
        Resonator()
        {
            for (auto& d : delay)
                d.assign ((size_t) delaySize, 0.0f);
        }

        void setNote (int n) { note = n; coeffsValid = false; }

        void reset()
        {
            for (auto& d : delay)
                std::fill (d.begin(), d.end(), 0.0f);

            for (int ch = 0; ch < maxChannels; ++ch)
            {
                comb[ch].writePos = 0;
                comb[ch].lp = comb[ch].hpX1 = comb[ch].hpY1 = 0.0f;
                bank[ch].x1 = bank[ch].x2 = 0.0f;
                bank[ch].y1.fill (0.0f);
                bank[ch].y2.fill (0.0f);
            }
        }

        void update (const Settings& s)
        {
            // 毎ブロック呼ばれるので、変化が無ければ計算しない
            if (coeffsValid && s == lastSettings)
                return;
            lastSettings = s;
            coeffsValid = true;

            const double fs = s.sampleRate;

            for (int ch = 0; ch < s.numChannels; ++ch)
            {
                double cents = 0.0;
                if (s.numChannels == 2)
                    cents = (ch == 0 ? -0.5 : 0.5) * s.widthCents;

                const double f0 = noteToHz (note + s.pitchOffset + cents / 100.0);

                if (s.mode == Mode::bank)
                    updateBank (ch, f0, s, fs);
                else
                    updateComb (ch, f0, s, fs, s.mode == Mode::tube);
            }
        }

        //==============================================================================
        inline float processComb (int ch, float x) noexcept
        {
            auto& c = comb[ch];
            auto* buf = delay[ch].data();

            // y[n - D] を 4 点ラグランジュで読む (D >= 2 が前提)
            const int   n = c.writePos;
            const float t = c.frac;
            const float x0 = buf[(n - c.dInt + 1) & delayMask];
            const float x1 = buf[(n - c.dInt)     & delayMask];
            const float x2 = buf[(n - c.dInt - 1) & delayMask];
            const float x3 = buf[(n - c.dInt - 2) & delayMask];

            const float c0 = -t * (t - 1.0f) * (t - 2.0f) * (1.0f / 6.0f);
            const float c1 = (t + 1.0f) * (t - 1.0f) * (t - 2.0f) * 0.5f;
            const float c2 = -(t + 1.0f) * t * (t - 2.0f) * 0.5f;
            const float c3 = (t + 1.0f) * t * (t - 1.0f) * (1.0f / 6.0f);

            const float fb = c0 * x0 + c1 * x1 + c2 * x2 + c3 * x3;

            c.lp = (1.0f - c.a) * fb + c.a * c.lp;
            const float hp = c.hpC * (c.lp - c.hpX1) + c.hpR * c.hpY1;
            c.hpX1 = c.lp;
            c.hpY1 = hp;

            const float y = x * c.inScale + c.g * hp;

            buf[n] = y;
            c.writePos = (n + 1) & delayMask;
            return y;
        }

        inline float processBank (int ch, float x) noexcept
        {
            auto& b = bank[ch];

            const float xd = x - b.x2;       // 零点 z = ±1
            b.x2 = b.x1;
            b.x1 = x;

            float sum = 0.0f;
            for (int k = 0; k < b.count; ++k)
            {
                const float y = b.b0[k] * xd + b.a1[k] * b.y1[k] - b.a2[k] * b.y2[k];
                b.y2[k] = b.y1[k];
                b.y1[k] = y;
                sum += y;
            }
            return sum;
        }

        // テスト用
        float combFeedback (int ch) const { return comb[ch].g; }
        int   bankCount (int ch) const    { return bank[ch].count; }

    private:
        // tube = true のときは 1 周の長さを半周期にしてフィードバックの符号を反転する。
        // するとループの位相条件が奇数倍音でだけ揃い、1, 3, 5... 倍音だけが共鳴する (片側が閉じた管)
        void updateComb (int ch, double f0, const Settings& s, double fs, bool tube)
        {
            auto& c = comb[ch];

            const double f = juce::jlimit (20.0, fs / 8.0, f0);
            const double period = fs / f;
            const double loopLen = tube ? period * 0.5 : period;
            const double w = juce::MathConstants<double>::twoPi * f / fs;
            const double twoPiOverFs = juce::MathConstants<double>::twoPi / fs;

            // ループ内フィルタは音程に追従させる (固定周波数だと低い音で基音ごと削れてしまう)
            //   ローパス: bright=0 で基音の 2.8 倍、bright=1 で 256 倍 (上限 0.45fs)
            //   ハイパス: 基音の 1/48。0Hz 付近のモードを殺す (これが無いと暗い設定で DC 側が鳴る)。
            //   位相補正は基音でしか合わないので、上の倍音はハイパスの分だけ少し低くなる
            //   (1/16 だと最大 -17 セント、1/48 で約 -6 セント。これ以上下げると暗い設定で余韻が頭打ちになる)
            const double lpFc = juce::jmin (f * std::pow (2.0, 1.5 + 6.5 * s.bright), fs * 0.45);
            const double a = std::exp (-lpFc * twoPiOverFs);
            // Tube は負帰還なので 0Hz 側にモードができない → ハイパス不要 (R = 1, hpC = 1 で素通し)
            const double R = tube ? 1.0 : std::exp (-(f / 48.0) * twoPiOverFs);
            const double hpC = tube ? 1.0 : (1.0 + R) * 0.5;

            using cd = std::complex<double>;
            auto filters = [&] (double wx)
            {
                const cd zi = std::polar (1.0, -wx);
                const cd lp = (1.0 - a) / (1.0 - a * zi);
                return tube ? lp : lp * (hpC * (1.0 - zi) / (1.0 - R * zi));
            };

            // フィルタの位相遅れ (サンプル、ハイパスの分は負になる) を差し引いて音程を合わせる
            const double filterDelay = -std::arg (filters (w)) / w;
            const double D = juce::jmax (2.0, loopLen - filterDelay);
            c.dInt = (int) std::floor (D);
            c.frac = (float) (D - c.dInt);
            c.a = (float) a;
            c.hpR = (float) R;
            c.hpC = (float) hpC;

            // ラグランジュ補間の振幅特性 (高音域で 1 周期ごとに少しずつ削れる分)
            const double t = c.frac;
            const double lc[4] = { -t * (t - 1) * (t - 2) / 6.0, (t + 1) * (t - 1) * (t - 2) / 2.0,
                                   -(t + 1) * t * (t - 2) / 2.0, (t + 1) * t * (t - 1) / 6.0 };
            auto response = [&] (double wx)
            {
                cd interp = 0.0;
                for (int i = 0; i < 4; ++i)
                    interp += lc[i] * std::polar (1.0, -wx * (i - 1 - t));   // 分数遅延ぶんの位相は除く
                return filters (wx) * std::abs (interp);
            };
            const cd H = response (w);

            // 発振しないように、各モードでのループ利得の最大値を見ておく。
            // String のモードは各倍音と、0Hz と基音の間に 1 つ (ループの位相がちょうど 0 になる周波数)。
            // Tube は奇数倍音だけ (0Hz 側は位相が π に届かないのでモードができない)
            double maxMag = std::abs (H);
            if (! tube)
            {
                auto loopPhase = [&] (double wx) { return std::arg (filters (wx)) - wx * D; };
                double lo = w * 1.0e-4, hi = w;     // loopPhase(lo) > 0, loopPhase(hi) = -2pi
                for (int i = 0; i < 40; ++i)
                {
                    const double mid = 0.5 * (lo + hi);
                    (loopPhase (mid) > 0.0 ? lo : hi) = mid;
                }
                maxMag = juce::jmax (maxMag, std::abs (response (lo)));
            }

            const int step = tube ? 2 : 1;
            const int nHarm = juce::jmax (1, (int) (0.5 * fs / f));   // ナイキストまでの倍音番号
            const int kMax = juce::jmin (64, nHarm);
            std::array<double, 65> harmMag {};
            int lastK = 1;
            harmMag[1] = std::abs (H);
            for (int k = 1 + step; k <= kMax; k += step)
            {
                harmMag[(size_t) k] = std::abs (response (w * k));
                maxMag = juce::jmax (maxMag, harmMag[(size_t) k]);
                lastK = k;
            }

            // 基音での 1 周あたりのループ利得を T60 = decay になる値に合わせる。
            // ただしどの周波数でもループ利得が 0.99995 を超えないようにする
            // (暗い設定・長い Decay では、ここで頭打ちになって実際の余韻が Decay より短くなる)
            const double perLoop = std::pow (10.0, -3.0 * loopLen / (s.decaySec * fs));
            const double g = juce::jmin (perLoop / std::abs (H), 0.99995 / maxMag);
            c.g = (float) (tube ? -g : g);

            // ホワイトノイズの電力利得は、各モードのループ利得 G_k について 1/(1-G_k^2) の平均。
            // その逆数の平方根を入力に掛けて Bank と同じく雑音利得 1 に揃える。
            // 計算していない高い倍音は、最後に計算した倍音と同じ利得とみなす (暗い設定ではどうせ小さい)
            double sum = 0.0;
            int counted = 0;
            for (int k = 1; k <= kMax; k += step)
            {
                const double G = g * harmMag[(size_t) k];
                sum += 1.0 / (1.0 - G * G);
                ++counted;
            }
            const int nModes = tube ? (nHarm + 1) / 2 : nHarm;
            if (nModes > counted)
            {
                const double G = g * harmMag[(size_t) lastK];
                sum += (nModes - counted) / (1.0 - G * G);
            }
            c.inScale = (float) std::sqrt (juce::jmax (counted, nModes) / sum);
        }

        void updateBank (int ch, double f0, const Settings& s, double fs)
        {
            auto& b = bank[ch];
            const double tilt = 1.0 - s.bright;
            const int nh = juce::jlimit (1, maxHarmonics, s.harmonics);

            std::array<double, maxHarmonics> amp {};
            double ampSq = 0.0;
            int count = 0;

            for (int k = 1; k <= nh; ++k)
            {
                const double fk = f0 * std::pow ((double) k, 1.0 + (double) s.stretch);
                if (fk < 20.0 || fk >= 0.45 * fs)
                    continue;

                // 高い倍音ほど小さく、短く (bright=1 なら全倍音同じ)
                const double ak   = std::pow ((double) k, -1.5 * tilt);
                const double t60k = s.decaySec * std::pow ((double) k, -tilt);
                const double r    = std::pow (10.0, -3.0 / (t60k * fs));
                const double w    = juce::MathConstants<double>::twoPi * fk / fs;

                b.a1[count] = (float) (2.0 * r * std::cos (w));
                b.a2[count] = (float) (r * r);
                // (1 - z^-2) / (1 - a1 z^-1 + a2 z^-2) の雑音電力利得は 2 / (1 - r^2)
                b.b0[count] = (float) (ak * std::sqrt ((1.0 - r * r) * 0.5));

                amp[count] = ak;
                ampSq += ak * ak;
                ++count;
            }

            // 倍音全体で雑音利得が 1 になるように
            if (ampSq > 0.0)
            {
                const float norm = (float) (1.0 / std::sqrt (ampSq));
                for (int k = 0; k < count; ++k)
                    b.b0[k] *= norm;
            }

            // 倍音数が減ったぶんの状態は捨てる
            for (int k = count; k < maxHarmonics; ++k)
                b.y1[k] = b.y2[k] = 0.0f;

            b.count = count;
        }

        struct CombState
        {
            int   writePos = 0;
            int   dInt = 2;
            float frac = 0.0f;
            float a = 0.0f, g = 0.0f, inScale = 1.0f;
            float hpR = 0.0f, hpC = 1.0f;
            float lp = 0.0f, hpX1 = 0.0f, hpY1 = 0.0f;
        };

        struct BankState
        {
            int count = 0;
            float x1 = 0.0f, x2 = 0.0f;
            std::array<float, maxHarmonics> a1 {}, a2 {}, b0 {}, y1 {}, y2 {};
        };

        int note = 60;
        Settings lastSettings;
        bool coeffsValid = false;
        std::array<std::vector<float>, maxChannels> delay;
        std::array<CombState, maxChannels> comb;
        std::array<BankState, maxChannels> bank;
    };
}
