# ChordRes — Claude Code 向けの仕様書

MIDI で押さえたノートの音程で、入力音に共鳴をかけるオーディオエフェクト (VST3、JUCE 8.0.15)。
作者は MIDy (音楽プロデューサー)。Claude Code が作り、MIDy が Ableton Live で鳴らして判断している。
ユーザー向けの説明は README.md。このファイルは、別のマシン (Windows を含む) の Claude Code が
作業を引き継げるように、仕様・決定事項・ビルド方法をまとめたもの。

## 前提と作業の約束

- 音を聴いて良し悪しを判断するのはユーザー。Claude は数値で壊れていないこと (音程・余韻・発振・レベル・CPU) を確かめる
- パラメータ ID・PLUGIN_CODE (ChRs)・PLUGIN_MANUFACTURER_CODE (MIDy)・Mode の番号は変えない。
  変えると、保存済みの Live セットで設定が読めなくなる。Mac 版と Windows 版も同じ ID なので、セットを持ち回せる
- 名前は仮名 (ユーザーが後で改名するかもしれない)
- コミットの身元は `git config user.name` = midy4234-hub、`user.email` = 331112920+midy4234-hub@users.noreply.github.com。
  本名やホスト名由来のアドレス、個人の Gmail をコミットに入れない。コミット前に `git config --show-origin --get-regexp "^user\."` で確認する

## 信号の流れ

入力 → 共鳴 (Mode) → Drive → FILTER → Mix (Dry と混ぜる) → Output → Safety

- 共鳴は時間領域だけで処理する (FFT 不使用)。最大 12 ボイス
- Live での使い方: 共鳴させたいトラックに挿し、別の MIDI トラックの MIDI To を「そのトラック → ChordRes」にする (NEEDS_MIDI_INPUT)
- ベロシティは一切使わない (ユーザー判断)

## パラメータ (ID / 表示名 / 範囲 / 既定)

| ID | 表示名 | 範囲 | 既定 | 中身 |
|---|---|---|---|---|
| mode | Mode | 0 String / 1 Bank / 2 Tube | Bank | 番号は保存互換のため。UI の並びは String / Tube / Bank |
| hold | Note Off | 0 Key / 1 Hold | Key | Key: 離したら入口を閉じ Decay で自然減衰。Hold: 次に弾き直すまで保持 |
| attack | Attack | 0.5〜2000 ms (中心 50) | 5 ms | 鳴り始めのフェードイン |
| decay | Decay | 0.05〜20 s (中心 2) | 2 s | 余韻 T60 |
| amount | Amount | 左端 -inf 〜 +12 dB | 0 dB | 共鳴の音量 (Dry は変わらない) |
| bright | Bright | 0〜1 | 0.6 | 高い倍音の大きさと余韻 |
| harm | Harmonics | 1〜24 | 12 | Bank のみ。倍音の数 |
| stretch | Stretch | -0.3〜0.3 | 0 | Bank のみ。倍音間隔の伸び縮み |
| sat | Drive | 0〜48 dB | 0 | 共鳴の後段の歪み (tanh + ADAA) |
| transpose | Transpose | -24〜24 半音 | 0 | 共鳴の音程だけずらす |
| fine | Fine | -100〜100 ct | 0 | 同上 |
| ftype | Filter | 0 Off / 1 LP / 2 HP / 3 Notch / 4 Formant | Off | 共鳴音にだけかかる |
| fcut | Freq | 20 Hz〜20 kHz (対数) | 2 kHz | |
| freso | Reso | 0〜1 | 0.2 | |
| vowel | Vowel | 0〜4 (a-e-i-o-u) | 0 | Formant のみ |
| width | Width | 0〜50 ct | 6 ct | L/R を逆方向にデチューン |
| mix | Mix | 0〜1 | 0.5 | |
| out | Output | -24〜12 dB | 0 | |
| safety | Safety | on/off | on | -2 dBFS 付近から丸めて 0 dBFS を超えさせない |

## モードの中身 (Source/Resonator.h)

- String (内部名 comb): ディレイのフィードバック。倍音が全部鳴る
  - ループ内の LP・HP は音程に追従させる。固定周波数 LP だと低音で基音が削れ、0 Hz 側のモードが鳴って音程が最大 177 ct ずれた
  - ループ内 HP は f0/48 (f0/16 だと上の倍音が最大 -17 ct ずれた。f0/48 で約 -4 ct)
  - 発振防止: 各倍音と「0 Hz と基音の間の 1 モード (二分法で求める)」でのループ利得を 0.99995 以下に。Bright が低いと実際の余韻が短くなるのはこのため
  - ラグランジュ補間の振幅損失を補正しないと、高音で余韻が縮む
  - 入力正規化は倍音ごとの 1/(1-G_k²) の平均
- Tube: 半周期のディレイ + 負帰還。奇数倍音だけ。0 Hz 側のモードが無いのでループ内 HP は不要
- Bank: 倍音ごとのバンドパス。Harmonics / Stretch が効く
- 音程が一致した持続音を入れると +19〜26 dB 持ち上がる。だから Safety は既定 ON

## Drive (param id は sat)

- ユーザーは「Drive = 歪み」と理解している。つまみ名と中身をずらさない
- 共鳴の段階で和音 + 音程一致だとピーク +7 dBFS 程度になり得るので、手前で -24 dB 下げてから tanh、後で戻す (ユーザーが選んだ方式)
- 自動のレベル合わせは不採用 (強い音ほど早く歪む性質を残す)。メイクアップは -6 dBFS ピーク基準

## FILTER (Source/WetFilter.h)

- 共鳴音のみ。Drive の後、Mix の前 (UI のパネル順 = 信号順)
- LP / HP / Notch は 12 dB/oct の状態変数フィルター。Formant は Peterson & Barney 系の F1〜F3 並列バンドパス、makeup +9 dB

## UI (Source/PluginEditor.cpp、900 x 480)

- Ableton 純正寄りの見た目 (ユーザーが 4 案から選んだ)
- パネルは左から ENVELOPE (Attack Decay Amount) / TONE (Bright Harmonics Stretch Drive Transpose Fine) / FILTER / MASTER (Width Mix Output Safety)、幅の比 2:3:3:2
- Note Off スイッチは ENVELOPE 右上、Mode スイッチは TONE 右上 (juce::ParameterAttachment で並んだボタン)
- 鍵盤表示: 明るさ = ノートごとの共鳴レベル、鍵盤上の縦線 = 倍音の位置 (Resonator.h の displayPartials)。線の高さにフィルターの効きを反映
- IN / WET / OUT メーター (WET はピークホールド、0 dBFS 超は赤)
- Stretch / Transpose / Fine は 0 から弧を描く

## 決定済みで直さないこと

- Transpose とフィルター種類の切り替えの瞬間にクリック (+27〜30 dB) が出る。オートメーションしない前提で直さない (ユーザー判断 2026-09-26)。耐久テストからも除外している

## 検証の結果 (Mac、2026-09-25〜26)

- 音程は全域 ±0.5 ct 以内、T60 は設定値どおり、60 秒ノイズで発振なし、ノイズ入力で両モードの出力が入力比 ±3 dB 程度
- CPU 12 ボイスで String 0.5 % / Bank 1.4 %
- 耐久テスト (robust) 全 PASS。Bank のサンプルレート依存 (+3 dB) は修正済み (48 kHz の結果は不変)

## ビルド

JUCE は `../_deps/JUCE` があればそれを使い、無ければ CMake が GitHub から 8.0.15 を取ってくる (初回は数分かかる)。
`lab/` は PluginLab (MIDy の非公開の作業場) の共通ヘッダの写し。隣に `../PluginLab` があればそちらが優先される。

### Windows

必要なもの: Visual Studio 2022 以降 (「C++ によるデスクトップ開発」ワークロード。CMake も同梱)、Git。

```
cmake -B build -A x64
cmake --build build --config Release --target ChordRes_VST3
```

- できるもの: `build\ChordRes_artefacts\Release\VST3\ChordRes.vst3` (フォルダ)
- フォルダごと `C:\Program Files\Common Files\VST3\` にコピーする (管理者権限が要る)。
  管理者のシェルなら `-DLAB_INSTALL=ON` を付けて configure すればビルド後に自動でコピーされる (既定は OFF。権限が無いとビルドが失敗するため)
- Live: 環境設定 → Plug-ins → 「VST3 プラグイン システムフォルダ」をオンにして再スキャン
- ソースは UTF-8 の日本語コメント入り。CMakeLists で MSVC に `/utf-8` を渡している (外すと C4819 や誤コンパイル)

### Mac

```
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```
ユニバーサル (x86_64 + arm64)。`~/Library/Audio/Plug-Ins/VST3/` に自動でコピーされる。

### GitHub Actions

`.github/workflows/windows.yml`: main への push (と手動実行) で Windows x64 の VST3 をビルドし、
Actions の実行ページの Artifacts に `ChordRes-windows-x64` として置く。同時に検証ドライバの耐久テストも走らせる。

## 検証ドライバ (DAW 無し)

```
cmake -B build-test -DCHORDRES_BUILD_TESTS=ON
cmake --build build-test --config Release --target ChordResTest
build-test/ChordResTest_artefacts/Release/ChordResTest            # 固有テスト (音程・余韻・安定性・CPU)
build-test/ChordResTest_artefacts/Release/ChordResTest robust     # 標準の耐久テスト 12 項目 (lab/LabRobust.h)
```
変更したら最低でも固有テストと robust を通す。
