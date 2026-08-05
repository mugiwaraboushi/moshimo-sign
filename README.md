# もしも電光掲示板 (moshimo-sign)

「もしも まちと未来の実験室」の電光掲示板プロジェクト。
ESP32 + HUB75 RGBマトリクスパネル(64×32, P4)で、イベント時のコメント表示や営業中サインを自作する。

- 学術と社会・近隣コミュニティの双方向コミュニケーションのツールを自ら作る実践
- ハードの電子工作はせず、ソフトウェアに開発を集中する方針
- 「開かれていること」を重視: 開発の過程はDiscordで公開、プロトタイプはURLで誰でも触れる

## ディレクトリ

| パス | 内容 |
|---|---|
| `prototypes/` | Webプロトタイプ集 (GitHub Pagesで公開) |
| `prototypes/sim/` | 掲示板シミュレータ (かな漢字7,444字のビットマップフォント入り) |
| `firmware/moshimo_sign/` | ESP32ファームウェア (2段表示・コメント取得・OTA対応) |
| `docs/` | セットアップ手順・設計メモ |
| `CLAUDE.md` | Claude(AI)がこのリポジトリで作業するときの動作指針 |

## セットアップ (初回のみ)

1. ~~このフォルダをGitHubにpushする~~ (完了: https://github.com/mugiwaraboushi/moshimo-sign)
2. GitHub → リポジトリ Settings → Pages → Source: `Deploy from a branch` → Branch: `main` / `(root)` → Save
3. 数分後、`https://mugiwaraboushi.github.io/moshimo-sign/` でポータルが公開される
4. Discord連携は `docs/discord-bot-setup.md` を参照

> **privateリポジトリの場合**: GitHub PagesはFreeプランのprivateリポジトリでは使えない。
> 学生は [GitHub Education](https://education.github.com/) でProが無料になり、private + Pagesが可能。
> なお、Pagesで公開されるページのURL自体は誰でも見られる(コードだけが非公開)。

## ファームウェアのビルド

arduino-cli / Arduino IDE + ESP32コア3.3.11 + 以下のライブラリ:
ESP32-HUB75-MatrixPanel-DMA, Adafruit GFX, Adafruit BusIO

```bash
cp firmware/moshimo_sign/config.example.h firmware/moshimo_sign/config.h
# config.h にWiFi情報を記入 (config.h はgit管理外)
arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs" firmware/moshimo_sign
```

初回のみUSB書き込み。以後はArduinoOTAでWiFi経由更新できる。

## クレジット

フォント: efont Biwidth 16px © 2000-2003 /efont/ The Electronic Font Open Laboratory
