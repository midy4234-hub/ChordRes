# ChordRes — MIDI でボイシングを指定するレゾネーター (VST3)

コードは Claude Code (Anthropic の AI) が書き、MIDy が仕様を決めて Ableton Live で確認しました。
無保証です。サポート・不具合対応・要望への対応はしません (Issue / Pull Request も受け付けません)。
ライセンスは AGPLv3 (LICENSE)。JUCE (AGPLv3) と VST3 SDK (MIT) を使っています。Copyright (C) 2026 MIDy

Made with Claude Code. Provided as-is, without support. Issues and pull requests are not accepted. Licensed under AGPLv3.

**ダウンロード**: Releases に Mac 版 (Intel / Apple Silicon 両対応) と Windows 版 (x64) の zip があります。

- Mac: `ChordRes.vst3` を `~/Library/Audio/Plug-Ins/VST3/` に入れる。署名していないので、入れたあとターミナルで

  ```
  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/ChordRes.vst3
  ```

- Windows: `ChordRes.vst3` フォルダごと `C:\Program Files\Common Files\VST3\` に入れる

ビルド方法は CLAUDE.md の「ビルド」。

入力音に、MIDI で押さえたノートの音程で共鳴をかけるオーディオエフェクト。
FFT は使わず時間領域だけで処理する。

Live での使い方
1. 共鳴させたい音のトラックに ChordRes を挿す
2. 別に MIDI トラックを作り、MIDI To を「そのトラック」→「ChordRes」にする
3. MIDI トラックをモニター In にして鍵盤で弾くか、MIDI クリップでコードを書く

パラメータ
- Mode (TONE 右上): String (ディレイのフィードバック。倍音が全部鳴る) / Tube (負帰還・半周期のディレイ。奇数倍音だけ鳴る) / Bank (倍音ごとのバンドパスで拾う)
- Note Off: Key (離したら入口を閉じ、溜まった響きが Decay で消える) / Hold (次に弾き直すまで保持)
- ENVELOPE
  - Attack: 鳴り始めのフェードイン
  - Decay: 余韻 (T60)。String で Bright が低いと、発振防止のため実際の余韻が短くなる場合がある
  - Amount: 共鳴の音量 (Dry は変わらない)。左端で無音。ベロシティは使わない
- TONE
  - Bright: 高い倍音の大きさと余韻
  - Harmonics / Stretch: Bank のみ。倍音の数と、倍音の間隔の伸び縮み
  - Drive: 共鳴の後段で和音全体をまとめて歪ませる (tanh、ADAA で折り返し低減)。0〜48dB。
    手前で -24dB 下げているので 0dB ではほぼ素通し。-6dBFS のピークはどの Drive でも -6dBFS に揃う。
    大きい音ほど早く歪む (共鳴のピーク -5dBFS なら 24dB あたりから、+7dBFS なら 12dB あたりから)
  - Transpose / Fine: 共鳴させる音程をずらす (Dry の音程は変わらない)
- FILTER (共鳴音にだけかかる。Drive の後、Mix の前)
  - Off / LP / HP / Notch: 12dB/oct の状態変数フィルター。Freq と Reso
  - Formant: F1〜F3 の並列バンドパス。Vowel で a-e-i-o-u を連続的に移動、Reso でフォルマントの鋭さ
  - 画面の倍音の線の高さにフィルターの効きを反映し、カーブも薄く表示
- MASTER
  - Width: L/R を逆方向にデチューン
  - Mix / Output
  - Safety: -2dBFS 付近から丸めて 0dBFS を超えさせない
    (音程に一致した持続音を入れると +20dB 以上持ち上がることがある)

ビルド
  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
  → ~/Library/Audio/Plug-Ins/VST3/ChordRes.vst3 にコピーされる

検証 (DAW 無し)
  cmake -B build-test -DCHORDRES_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
  cmake --build build-test --target ChordResTest && ./build-test/ChordResTest_artefacts/Release/ChordResTest
