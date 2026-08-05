#pragma once

// ---- パネル ----
#define PANEL_W 64
#define PANEL_H 32
#define BRIGHTNESS 96          // 0-255 (室内なら96程度で十分明るい)

// ---- WiFi ----
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

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

// ---- 表示内容 ----
#define TOP_MESSAGE "営業中"
#define DEFAULT_MESSAGE "ようこそ「もしも」へ　◆　まちと未来の実験室"
#define SCROLL_SPEED 45.0f     // dot/秒 (シミュレータで検証した値をそのまま使う)

// ---- 色 (RGB565) ----
#define COLOR_TOP    0xFCC0    // アンバー
#define COLOR_BOTTOM 0xFCC0
