# HD60 S Linux Driver

[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Kernel](https://img.shields.io/badge/Kernel-7.0+-orange.svg)]()
[![PipeWire](https://img.shields.io/badge/PipeWire-1.6+-yellow.svg)]()
[![Status](https://img.shields.io/badge/Status-Experimental-orange.svg)]()

**Elgato HD60 S (HDMI キャプチャカード) を Linux で動作させる**、USB プロトコル・レジスタマップ・MCU シーケンスを逆解析して作った ユーザーランドドライバ。純正ドライバは Windows / macOS のみ、それ以前の Linux 実装も存在しなかった (Doug Brown が 2024/09 に "no Linux drivers exist" と明言) ため、実質的に **公開されている唯一の HD60 S Linux 実装** です (2026-07 時点、私達の把握範囲で)。

`English version follows below.`

![HD60 S captured Splatoon 3 gameplay](docs/screenshot.png)

*iso_capture が `/dev/video42` に流し込んだ映像 (1080p60 YUYV、Splatoon 3 ロビー画面)*

---

## 何ができる

`./hd60s live` 一発で:

- ✅ **1080p60 YUYV 映像キャプチャ** → `/dev/video42` (v4l2loopback) → OBS Studio / mpv / ffmpeg などから利用
- ✅ **96kHz mono 音声** (SEP payload → downmix + libsamplerate) → `snd-aloop` playback `hw:10,0` / capture `hw:10,1` → PipeWire-Pulse source `hd60s_capture` → OBS
- ✅ **HDMI パススルー** (HD60 S HDMI OUT → TV) を並行して 低遅延で維持 (ゲームプレイに使える)
- ✅ **iso capture と パススルー 同時動作** (Windows Elgato Game Capture ソフトと同じ運用パターンが可能)
- ✅ **USB 切断後の自動再試行** (`run-hd60s-obs.sh` がデバイス再列挙を待って `iso_capture` を再起動)

Nintendo Switch を接続して 動作確認済み。長時間プレイでも安定 (iso packet loss ~5-7%)。

## Windows Elgato Game Capture ソフトとの比較

| 機能 | Windows 純正 | Linux (このドライバ) |
| --- | --- | --- |
| 1080p60 映像キャプチャ | ✅ | ✅ |
| 音声キャプチャ | ✅ 48kHz | ✅ 96kHz mono (hd60s_capture) |
| HDMI パススルー (TV 出力) | ✅ | ✅ |
| iso capture と パススルー同時 | ✅ | ✅ |
| OBS Studio 連携 | ✅ | ✅ (V4L2 + PipeWire-Pulse hd60s_capture) |
| モニタリング遅延 | ~15-20ms | ~20-30ms |

---

## 免責 (Disclaimer)

⚠️ **このソフトウェアは非公式実装です**。Elgato Systems GmbH とは無関係、同社の承認を受けていません。逆解析は プロトコル互換性目的 (interoperability) で行なわれており、一般に interoperability 目的の逆解析は多くの管轄で適法と考えられていますが、これは法的助言ではありません。使用に伴う法的判断は各自の管轄下でお願いします。

本ドライバの使用によって発生した機器破損・データ損失・その他の損害について、作者は一切の責任を負いません。動作は無保証 (as-is)、使用は自己責任でお願いします。

---

## 動作環境

- **OS**: Ubuntu 25.04 以降 (kernel 7.0系, PipeWire 1.6+ 想定)
- **USB**: **USB 3.0 (SuperSpeed) 必須**。1080p60 は 2Gbps 出るので USB 2.0 だと帯域が足りない
- **音声**: **PipeWire** (PulseAudio only 環境では動作しない)。既定経路は ALSA loopback + `pactl load-module module-alsa-source`。`HD60S_AUDIO_PW=1` で iso_capture の PipeWire ネイティブ出力に切替 (その場合 ALSA source は公開しない)
- **v4l2loopback**: 0.15.3 以降 推奨 (`exclusive_caps=0` で運用)
- **対象デバイス**: Elgato Game Capture HD60 S (VID `0fd9` / PID `005e`) — **HD60 S+ は別チップ構成なので動かない**。旧 PID `0074` のストリームとは別物

### 既知の制約 / TODO

- Ubuntu 25.04 以外は未検証 (他ディストロ / 他バージョンでは要調整の可能性)
- Wayland + NVIDIA GPU 環境で mpv `--vo=gpu-next` が MESA ZINK エラーで動かない場合あり → `--vo=wlshm` を強制 (`hd60s live` で対応済)
- 30fps / 720p60 モードは未実装 (1080p60 のみ動作確認)
- HD60 S+ / HD60 X などの後継モデルは非対応
- ホットプラグ API は未使用。`run-hd60s-obs.sh` が USB 再列挙をポーリングして再起動する

---

## リポジトリ構成

```
src/                 キャプチャ本体 (iso_capture.c と audio include)
tools/               実験用ユーティリティ (audio_extract, offline_parser, spi_dump, probe_iso)
analysis/            init / burst 用 TSV (live 既定: init-p2-audio-fast.tsv, poststream-no9a.tsv)
docs/                screenshot.png
wireplumber/         51-hd60s-alsa.lua (snd-aloop の逆方向ノードを隠す)
hd60s                ランチャー
run-hd60s-obs.sh     USB 再接続ループ (supervisor)
70-elgato-hd60s.rules
hd60s.service.in
Makefile
```

ビルド成果物 (`iso_capture` など) はリポジトリ直下に出力される。

---

## Build

依存パッケージをまとめて入れる (推奨):

```bash
./hd60s install-deps
./hd60s doctor
```

手動で入れる場合:

```bash
sudo apt install \
  build-essential pkg-config \
  libusb-1.0-0-dev libasound2-dev libpipewire-0.3-dev libsamplerate0-dev \
  v4l2loopback-dkms v4l2loopback-utils linux-headers-$(uname -r) \
  alsa-utils ffmpeg vlc mpv guvcview tmux obs-studio

make all
```

`make iso_capture` だけでもキャプチャ本体はビルドできる (`src/iso_capture.c` → `./iso_capture`)。`libsamplerate` は音声リサンプルに必須。

任意: `/usr/local` にインストール (udev ルール・WirePlumber 設定・ユーザー systemd unit も配置):

```bash
sudo make install
```

`make install` は実行ファイル、解析用 TSV、`hd60s` ランチャー、`run-hd60s-obs.sh` を `/usr/local/libexec/hd60s` に置き、`/usr/local/bin/hd60s`、ユーザー用 systemd unit、udev ルール (`0fd9:005e` → `uaccess`) を作成する。削除は `sudo make uninstall`。

端末や tmux に依存せず自動再試行を常駐させる場合:

```bash
# v4l2loopback と snd-aloop は unit ではロードしない
sudo modprobe v4l2loopback video_nr=42 card_label=HD60S exclusive_caps=0
sudo modprobe snd-aloop enable=1 index=10 id=hd60s pcm_substreams=1
sudo v4l2loopback-ctl set-caps /dev/video42 "YUYV:1920x1080@60/1"
systemctl --user daemon-reload
systemctl --user enable --now hd60s.service
```

(`./hd60s prep` というコマンドは無い。モジュール込みの起動は `./hd60s live`。)

---

## 使い方

### 一発起動 (推奨)

```bash
./hd60s live
```

tmux セッション `hd60s`:

- **上ペイン**: `run-hd60s-obs.sh` — USB 制御、映像/音声抽出、切断時の再試行。`HD60S_AUDIO_PW=0` なので音声は ALSA loopback → `hd60s_capture`
- **下ペイン**: `mpv --vo=wlshm` — 13 秒後に `/dev/video42` を再生 (`HD60S_NO_MPV=1` で抑制)

**同時に TV には HDMI パススルー経由でゲーム映像が低遅延で出る** (プレイに使う経路)。

操作:

- `Ctrl+B → 矢印` : ペイン切替
- `Ctrl+B → d` : デタッチ (バックグラウンド継続)
- `Ctrl+B → q` : 全終了
- `./hd60s stop` : iso_capture / tmux を止める

### OBS Studio で使う

1. `./hd60s live` でキャプチャを起動 (mpv 不要なら `HD60S_NO_MPV=1 ./hd60s live`、またはプレビューを閉じる)
2. あるいは `./hd60s obs` (iso_capture を tmux で起動してから OBS を立ち上げる)
3. OBS ソース追加:
   - **映像キャプチャデバイス (V4L2)** → `/dev/video42` / 1920x1080 / 60fps / YUYV
   - **音声**: PipeWire-Pulse source `hd60s_capture` (live 既定)。直接 ALSA なら `hw:10,1` (hd60s Loopback)

「音声モニタリング: モニターのみ (出力はミュート)」設定にすれば、プレイ音は PC スピーカーで即時モニタ、録画音は同期して録画される (Windows OBS と同じ挙動)。

### よく使うコマンド

```text
./hd60s live            # tmux: supervisor + mpv
./hd60s obs             # supervisor のあと OBS 起動
./hd60s start [秒]      # フォアグラウンドで iso_capture (既定 600s)
./hd60s view            # VLC で映像+音声
./hd60s view-mpv        # mpv (映像のみ)
./hd60s stop            # 停止
./hd60s doctor          # 依存・モジュール・デバイス診断
./hd60s install-deps    # apt 依存を一括インストール
./hd60s help            # 実験用コマンド含む一覧
```

---

## アーキテクチャ

```
      ┌───────────────┐
      │  Switch etc.  │
      └───────┬───────┘
              ▼
   ┌──────────────────────┐         ┌──────────────┐
   │  Elgato HD60 S       │──HDMI──▶│  TV           │
   │                      │   OUT   │  (passthrough)│
   │  IT6802E HDMI RX     │         │  低遅延        │
   │       ↓              │         └──────────────┘
   │  Nuvoton M031 MCU    │
   │       ↓              │
   │  Altera MAX II CPLD  │
   │       ↓              │
   │  Cypress FX3 USB     │
   │  ── iso EP 0x83 ─────┼──────▶ Linux Host
   │                      │
   │  IT66121 HDMI TX ────┼──▶ (passthrough)
   └──────────────────────┘
              │
              ▼ USB 3.0
   ┌──────────────────────┐
   │  iso_capture (libusb)│
   │  src/iso_capture.c   │
   │  - Init + MCU/CPLD   │
   │  - Frame parser      │
   │  - ALSA audio feed   │
   └───┬──────────────┬───┘
       │              │
       ▼              ▼
  /dev/video42    snd-aloop hw:10,0 → hw:10,1
  (v4l2loopback)   pactl source "hd60s_capture"
       │              │
       ▼              ▼
    OBS / mpv     OBS / speakers
```

---

## 開発体制について (AI 運用)

このプロジェクトは私 (kusq) が主導しつつ、Anthropic の Claude を伴走ツールとして活用して完成させた リバースエンジニアリングプロジェクトです。**Windows/macOS 純正しか存在しないハードウェアに対して 数日で Linux 実装が生まれた** のは、AI との協働ワークフローの効果が大きく、今後同様の RE プロジェクトの参考事例になればと思って明記します。

### 役割分担

- **私 (人間)**: 方針判断、実機テスト、優先度決定、USB pcap の物理取得、動作確認と体感評価
- **Claude Opus 4.7** (メインエージェント / 主に相談する相手): 実装コード生成、日々の相談、コードレビュー、Web 検索、Telegram 経由でのやりとり
- **Claude Opus 4.8** (サブエージェント / 静的解析用): 大規模な pcap / firmware の静的解析、実装方針の戦略相談、詰まった時の高度な推論
- **Claude Fable 5** (サブエージェント / 別視点): 独立した仮説出し・診断、adversarial critique (本人の仮説を厳しく叩く役)
  - 特に大きく貢献した箇所:
    - **音声 100ms cutoff の真犯人**: IT6802 chip の `FIX_ID_023` recovery ロジック を発掘し、Force FS 48kHz recovery loop を実装する道筋を提示
    - **映像表示できない件の真因**: v4l2loopback は正常動作しており、実際は `WAYLAND_DISPLAY` 空 + NVIDIA EGL 破損 が原因と実機テストで特定
    - **HDMI パススルー動作不安定の真犯人**: 私が追加した IT66121 TX 直叩きコマンドが MCU 自律制御への "friendly fire" と指摘、burst TSV から `0x9a` 書き込み削除で解決

Anthropic の Claude を活用させていただきました。

---

## 主な技術発見 (RE ノート抜粋)

### チップ構成
HD60 S は 5 チップ構成: **Cypress FX3** (USB3 SuperSpeed) + **Nuvoton M031** MCU + **IT6802E** (HDMI RX) + **IT66121** (HDMI TX) + **Altera MAX II** CPLD + **W25Q32JV** SPI flash

### USB プロトコル
- `bReq=0xC0, wValue=0x5066` : I2C bridge (`{slave, reg, val}` 3 バイトを送る)
- `bReq=0xC0, wValue=0x509c` : MCU RPC (2 バイトのコマンド)
- `bReq=0xC6, wValue=0x0032, wIndex=0x0101` : 60B の MCU batch (SRAM 書き込みシーケンス)

### I2C スレーブ
- `0x9a/0x9b` : IT66121 TX write/read (**ホスト側から直叩き禁止**、MCU が自律制御している)
- `0x9c/0x9d` : IT6802 RX write/read (但し MCU proxy 経由らしい)
- `0x94/0x95` : IT6802 audio bank (page 2 選択で AUDIO_ON/HBR/Fs レジスタ)
- `0xaa` : MCU RPC (magic `12 34` prefix)
- `0xd4` : CPLD 制御 (routing bit0=RX→TX bit1=RX→FX3)

### 音声フォーマット
- SEP marker (`ff 00 ff 02`) + 8 バイト payload
- 8B payload = **4 個の signed 16-bit little-endian samples** (`L0,R0,L1,R1`)
- ブリッジ側で downmix し、96 kHz mono として `hd60s_capture` に出す
- 既定の live 経路は ALSA loopback。PipeWire ネイティブは `HD60S_AUDIO_PW=1`

### 音声 100ms cutoff の解決 (FIX_ID_023)
IT6802 の audio state machine が HDMI audio channel status を誤検出 → 100ms 前後で HW mute。ITE 公式 SDK の `AudioFsCal()` + `aud_fiforst()` + Force FS 48kHz recovery loop を 100ms 周期で発射することで解消。

### HDMI パススルーの enable シーケンス
- ホストから `0x9a` (IT66121 TX) に直叩きすると MCU 制御を壊す → 信号切断
- 実際に必要なのは MCU (slave `0xaa`) + CPLD (slave `0xd4`) の 6 コマンド:
  ```
  1. wV=0x509c → 0x27 0x00           MCU video gate open
  2. wV=0x5066 → aa 12 34 90 05 00   MCU RPC set-mode
  3. wV=0x5066 → aa 12 34 90 03 00   MCU RPC set-mode
  4. wV=0x5066 → d4 00 04 03         CPLD routing enable
  5. wV=0x5066 → d4 00 2a 6e         CPLD keepalive
  6. wV=0x5066 → d4 00 01 02         CPLD keepalive
  ```
- init TSV / burst TSV から `0x9a` 書き込みを全削除 することで iso capture と passthrough の両立が可能

---

## Credits

### Reverse Engineering Predecessors
- **[Doug Brown](https://www.downtowndougbrown.com/2024/09/fixing-an-elgato-hd60-s-hdmi-capture-device-with-the-help-of-ghidra/)** — Elgato HD60 S firmware Ghidra 分析 blog。CCUVC MCU の存在と役割の最初の突破口
- **[tolga9009/elgato-gchd](https://github.com/tolga9009/elgato-gchd)** — Game Capture HD (先代モデル) の Linux ドライバ実装。iso 転送処理の実装参考
- **[ITE Tech Inc.](https://www.ite.com.tw/en/product/cate1/IT6802)** — IT6802E chip vendor。音声 recovery ロジックの実装 参考

### AI 協働
- **[Anthropic](https://www.anthropic.com/) Claude** (Opus 4.7 main / Opus 4.8 & Fable 5 subagents) — 静的解析・実装・デバッグの伴走

### 特別謝辞
kusq (プロジェクトオーナー) — 実機・USB pcap 取得・体感評価・方針判断

---

## License

[MIT License](LICENSE)

Copyright (c) 2026 kusq

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED. See LICENSE for full text.

---

## Contributing / Roadmap

PR / Issue 歓迎します。特に:

- [ ] 720p60 / 1080p30 モード対応
- [ ] HD60 S+ / HD60 X 対応 (別チップだが構造が近い可能性、要 pcap)
- [x] ユーザー空間 supervisor による USB 切断・再列挙の自動再試行
- [ ] libusb hotplug API によるイベント駆動のホットプラグ
- [ ] 純正 V4L2 カーネルモジュール化 (userspace で安定したら)
- [ ] EDID 制御 (現状は Switch から出た EDID をそのまま TV に流している)

Windows Elgato Game Capture ソフトの USB pcap を提供いただけると、未実装機能の解析が加速します。

---

# English Overview

**Reverse-engineered Linux userspace driver for the Elgato HD60 S HDMI capture card.** As of July 2026, we believe this is the only publicly available Linux implementation for this device (Doug Brown documented in 2024/09 that no Linux drivers exist).

## Features

One-shot launch (`./hd60s live`) delivers:

- ✅ 1080p60 YUYV video via `/dev/video42` (v4l2loopback) — OBS Studio / mpv / ffmpeg compatible
- ✅ 96kHz mono audio via ALSA loopback (`hw:10,0` → `hw:10,1`) published as PipeWire-Pulse source `hd60s_capture`
- ✅ HDMI passthrough (HD60 S HDMI OUT → TV) simultaneously with low latency for gameplay
- ✅ Simultaneous iso capture and passthrough (matches Windows Elgato app usage pattern)
- ✅ Automatic retry after USB disconnect (`run-hd60s-obs.sh`)

## Requirements

- Ubuntu 25.04+ (kernel 7.0+), PipeWire 1.6+
- USB 3.0 (SuperSpeed) — 1080p60 requires ~2 Gbps, USB 2.0 insufficient
- v4l2loopback 0.15.3+ with `exclusive_caps=0`
- Elgato HD60 S (VID `0fd9` / PID `005e`) — **HD60 S+ is a different chipset and not supported**

## Layout

- `src/` — capture binary (`iso_capture.c`)
- `tools/` — lab utilities
- `analysis/` — init/burst TSV sequences
- `hd60s` / `run-hd60s-obs.sh` — launcher and USB retry supervisor

## Build & Run

```bash
./hd60s install-deps
make all
./hd60s doctor
./hd60s live
```

`make install` is optional (udev rule, WirePlumber snippet, user systemd unit under `/usr/local`). There is no `./hd60s prep` command; `./hd60s live` loads `v4l2loopback` and `snd-aloop`. Native PipeWire audio is opt-in with `HD60S_AUDIO_PW=1`.

## About Development

This project was reverse-engineered by kusq (project owner) with Anthropic's Claude assisting as pair-programming tools. Roles were split as follows:
- **Claude Opus 4.7** — main agent for day-to-day implementation, code generation, and consultation
- **Claude Opus 4.8** — subagent invoked for heavy static analysis and strategic hypothesis work
- **Claude Fable 5** — subagent for adversarial critique and independent diagnosis

The AI performed static analysis, code generation, and debugging; the human directed strategy and tested on real hardware. See the "開発体制について (AI 運用)" section in the Japanese content above for detailed role breakdown.

Notable AI-attributed breakthroughs:
- ITE `FIX_ID_023` (audio 100ms cutoff root cause) discovered by Fable through IT6802 documentation search
- MCU/CPLD passthrough enable sequence (0x9a friendly-fire diagnosis) — Fable's blunt correction of an incorrect hypothesis
- v4l2loopback video display failure diagnosis (empty WAYLAND_DISPLAY + broken NVIDIA EGL) — Fable's empirical on-box testing

## Reverse Engineering Credits

Predecessor work this project builds on:
- Doug Brown's HD60 S Ghidra blog — https://www.downtowndougbrown.com/2024/09/
- tolga9009/elgato-gchd — https://github.com/tolga9009/elgato-gchd (prior-gen HD)
- ITE Tech (IT6802 chip vendor) — chip documentation used as reference for audio state machine behavior

## License

MIT — see [LICENSE](LICENSE).

## Disclaimer

Unofficial third-party implementation. Not affiliated with or endorsed by Elgato Systems GmbH. Use at your own risk. Reverse engineering was performed for interoperability purposes only.
