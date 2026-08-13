#pragma once

// ---- パネル ----
#define PANEL_W 64
#define PANEL_H 32
#define BRIGHTNESS 96          // 0-255 (室内なら96程度で十分明るい)

// ---- WiFi ----
// 2つまで登録でき、電波の届く方に自動で繋がる (WiFiMulti)。
// 2つ目は任意。使わないなら SSID2 を空文字のままにしておく。
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define WIFI_SSID2 ""          // 例: もしもの会場WiFi (空なら登録しない)
#define WIFI_PASS2 ""

// ---- OTA (WiFi経由アップデート) ----
#define OTA_HOSTNAME "moshimo-sign"
#define OTA_PASSWORD "moshimo"     // 空文字列でパスワード無し

// ---- コメント取得 ----
// 1行1コメントのプレーンテキストを返すURL (空なら取得しない)
// 例: Google Apps Script / 小さなWebサーバ / GitHub Gist raw など
#define COMMENTS_URL ""
#define FETCH_INTERVAL_MS 15000
#define MAX_COMMENTS 20

// ---- playlist取得 (v0.2) ----
// GitHub Pages上の playlist.json のURL (空なら取得しない)
// 例: "https://<USERNAME>.github.io/moshimo-sign/playlist.json"
#define PLAYLIST_URL ""
#define PLAYLIST_INTERVAL_MS 60000
#define MAX_PLAYLIST_MSGS 10

// ---- ドット絵 frames (v0.5 / holdMs は v2.1先行) ----
// 1枚あたり256バイトをRAMに常駐させる (24枚で6KB)
// 24未満を指定しても .ino 側で24に引き上げられる (holdMsのコマ送りに枚数が要るため)
#define MAX_FRAMES 24

// ---- 自己アップデート (v0.7) ----
// 実機が自分で新しいファームウェアを取りに行くための設定。手順は docs/firmware-release.md。
// URLを空文字列 "" にすると自己アップデート機能そのものが無効になる。
#define SELFUPDATE_MANIFEST_URL "https://mugiwaraboushi.github.io/moshimo-sign/firmware/manifest.json"
#define SELFUPDATE_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)   // 6時間ごとに確認

// ---- 表示内容 ----
#define TOP_MESSAGE "営業中"
#define DEFAULT_MESSAGE "ようこそ「もしも」へ　◆　まちと未来の実験室"
#define SCROLL_SPEED 45.0f     // dot/秒 (シミュレータで検証した値をそのまま使う)

// ---- 色 (RGB565) ----
#define COLOR_TOP    0xFCC0    // アンバー
#define COLOR_BOTTOM 0xFCC0
