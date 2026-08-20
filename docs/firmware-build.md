# ファームウェアのビルド環境構築

`firmware/moshimo_sign/` を **arduino-cli** でビルド検証するための手順。
実機への書き込みはファームウェア担当が行うため、このリポジトリでは「コンパイルが通ること」の確認までを行う。

対象: Windows 11 + arduino-cli 1.5.1 / ESP32 Arduino Core 3.3.11

> Linux でも同じ手順で通る (検証: arduino-cli 1.5.2-rc.1 / Core 3.3.11)。
> `winget` の代わりに公式tarball (`https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Linux_64bit.tar.gz`) を展開し、
> データ置き場は環境変数 `ARDUINO_DIRECTORIES_DATA` / `ARDUINO_DIRECTORIES_USER` で指定する。
> **ディスクは展開後で約5.6GB使う。** 空きが少ないマシンでは、インストール後に
> `staging/` (ダウンロード済みアーカイブ、約1.7GB) を消してよい。

---

## 0. 事前確認: ディスク空き容量

ESP32コアは展開後 **約4GB** を消費する (ツールチェーンが xtensa / riscv32 両方入るため)。
インストール先ドライブに **最低6GB** の空きを用意すること。

```bash
df -h
```

`%LOCALAPPDATA%` のあるドライブが手狭な場合は、手順2でデータ置き場を別ドライブへ逃がす。

---

## 1. arduino-cli のインストール

```powershell
winget install --id ArduinoSA.CLI --source winget --exact --accept-package-agreements
```

`C:\Program Files\Arduino CLI\arduino-cli.exe` に入る。
新しいシェルを開けば PATH が通る (通らない場合はフルパスで叩く)。

```bash
arduino-cli version
```

---

## 2. 設定ファイルの作成とデータ置き場の指定

```bash
arduino-cli config init
```

既定のデータ置き場は `%LOCALAPPDATA%\Arduino15`。
そのドライブに空きがない場合は、別ドライブへ向ける (設定ファイル自体は既定位置に残る):

```bash
arduino-cli config set directories.data      "F:\Users\<user>\Documents\ArduinoCLI"
arduino-cli config set directories.downloads "F:\Users\<user>\Documents\ArduinoCLI\staging"
```

`arduino-cli config dump` で反映を確認する。

> すでに `Arduino15` にコアを入れてしまっている場合は、`packages/` `staging/` `*_index.json*` `inventory.yaml` を
> 新しい場所へ移動すれば再ダウンロード不要。`arduino-cli.yaml` だけは元の場所に残すこと。

---

## 3. ESP32 ボードの追加

```bash
arduino-cli config add board_manager.additional_urls \
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
```

**注意: 合計約1.4GBのダウンロード + 展開で数分〜十数分かかる。**

### Core 3.x が必要な理由

`moshimo_sign.ino` は `#include <NetworkClientSecure.h>` を使っている。
これは ESP32 Arduino Core **3.x** で導入されたヘッダ (2.x では `WiFiClientSecure.h`)。
2.x 系のコアではコンパイルが通らないので、必ず 3.x を入れること。

---

## 4. ライブラリの導入

```bash
arduino-cli lib update-index
arduino-cli lib install "ESP32 HUB75 LED MATRIX PANEL DMA Display"
arduino-cli lib install "ArduinoJson"
arduino-cli lib install "Adafruit GFX Library"
```

| ライブラリ | バージョン | 用途 |
|---|---|---|
| ESP32 HUB75 LED MATRIX PANEL DMA Display | 3.0.14 | HUB75パネル駆動 (`MatrixPanel_I2S_DMA`) |
| ArduinoJson | 7.4.3 | `playlist.json` のパース |
| Adafruit GFX Library | 1.12.6 | 上記HUB75ライブラリの依存 |
| Adafruit BusIO | 1.17.4 | Adafruit GFX の依存 (自動で入る) |

ArduinoJson は **v7 系**が必要 (`.ino` が容量指定なしの `JsonDocument` を使っているため)。

導入先は `arduino-cli config get directories.user` のパス配下 `libraries/`。
`arduino-cli lib list` で確認する。

---

## 5. config.h の作成

`config.h` は WiFiパスワードを含むため **.gitignore 済み**。各自で作る。

```bash
cp firmware/moshimo_sign/config.example.h firmware/moshimo_sign/config.h
```

コンパイル検証だけなら `WIFI_SSID` / `WIFI_PASS` はダミー値のままでよい。
実機に書き込む前に実際の値へ差し替えること。

`PLAYLIST_URL` は `https://mugiwaraboushi.github.io/moshimo-sign/playlist.json` を指定する
(仕様は [`playlist-spec.md`](playlist-spec.md))。

---

## 6. コンパイル

ESP32 WROOM-32 開発ボードの FQBN は `esp32:esp32:esp32`。
**ただしパーティション設定の指定が必須** (理由は下記)。

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs" firmware/moshimo_sign
```

初回はコア側のソースもビルドするため数分かかる。2回目以降はキャッシュが効いて速い。

### パーティションは `min_spiffs` 固定

デフォルトの `PartitionScheme=default` はアプリ領域が 1.31MB しかなく、**このスケッチは入らない**:

```
最大1310720バイトのフラッシュメモリのうち、スケッチが1384223バイト（105%）を使っています。
Error during build: text section exceeds available space in board
```

efont のかな漢字7,444字 (`font16.h`) を丸ごとフラッシュに持っているため 1.38MB になる。

`min_spiffs` (1.9MB APP × 2 with OTA / 128KB SPIFFS) にすると収まる:

| | サイズ | 使用率 |
|---|---|---|
| Flash (アプリ) | 1,422,971 / 1,966,080 B | **72%** |
| RAM (グローバル変数) | 57,752 / 327,680 B | 17% |

(v0.10 時点の実測値。v0.9以前は 1,384,255 B / 70%)

`huge_app` (3MB) だと余裕はもっとあるが **OTA領域が無くなる**。
`.ino` は `ArduinoOTA` を使う前提なので、アプリ領域が2面ある `min_spiffs` を選ぶこと。

> 実機に書き込むときも同じパーティション設定にすること。設定が食い違うと起動しない。

### バイナリを取り出す

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs" \
  --output-dir build/moshimo_sign firmware/moshimo_sign
```

`build/` と `*.bin` は .gitignore 済みなのでコミットされない。

---

## 7. 実機への書き込み (ファームウェア担当向け)

このPCからは実機テストができないため、以下は担当者側での作業。

### USB経由 (初回)

```bash
arduino-cli board list                       # ポート確認
arduino-cli upload -p COM3 --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs" firmware/moshimo_sign
```

### OTA経由 (2回目以降・同一LAN内)

`.ino` が `ArduinoOTA` を有効にしているため、初回のUSB書き込み以降はWiFi経由で更新できる。
ホスト名 `moshimo-sign` / パスワードは `config.h` の `OTA_PASSWORD`。

---

## つまずきやすい点

- **`NetworkClientSecure.h: No such file`** → コアが 2.x。手順3で 3.x を入れ直す。
- **`ESP32-HUB75-MatrixPanel-I2S-DMA.h: No such file`** → 手順4のライブラリ名は長いので引用符で囲む。
- **`Adafruit_GFX.h: No such file`** → HUB75ライブラリの依存。非対話実行だと自動導入されないことがあるので明示的に入れる。
- **展開中に `not enough space on the disk`** → 手順0/2。ダウンロードは成功していても展開で落ちる。
- **`text section exceeds available space in board`** → 手順6。`PartitionScheme=min_spiffs` を付け忘れている。
- **`config.h: No such file`** → 手順5。`config.example.h` はコピー元であってビルドには使われない。

---

## 関連

- 表示内容のリモート更新: [`playlist-spec.md`](playlist-spec.md)
- 表示ロジックの検証: `prototypes/sim/index.html` (ブラウザ上で同じフォント・描画仕様を再現)
