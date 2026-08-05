/*
 * もしも電光掲示板 ファームウェア v0.2
 * ESP32 (WROOM-32) + HUB75 RGBマトリクスパネル 64x32 (P4)
 *
 * 機能:
 *  - efont Biwidth 16px (かな漢字7,444字) によるUTF-8テキスト描画
 *  - 2段表示: 上段「営業中」/時計(NTP)交互、下段コメントスクロール
 *  - playlist.json 定期取得: 表示文言・色・速度・輝度・モードをリモート更新 (v0.2)
 *    → GitHub Pages上のplaylist.jsonをClaude/メンバーが更新すると実機に反映される
 *  - WiFi接続、HTTPポーリングによるコメント取得(15秒毎、URLはconfig.hで設定)
 *  - ArduinoOTA: 初回USB書き込み後はWiFi経由で更新可能 (同一LAN内)
 *
 * 配線 (HUB75標準ピン割当はライブラリ既定値を使用):
 *   https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA を参照
 *   電源はパネルに5V別系統で供給すること (最大4A想定、表示内容なら2A程度)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "config.h"
#include "font16.h"

// ---------------- パネル ----------------
MatrixPanel_I2S_DMA *display = nullptr;

// ---------------- フォント ----------------
// FONT16_CPS を二分探索して FONT16_DATA (stride 33) を引く
static int glyphIndex(uint16_t cp) {
  int lo = 0, hi = FONT16_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint16_t v = pgm_read_word(&FONT16_CPS[mid]);
    if (v == cp) return mid;
    if (v < cp) lo = mid + 1; else hi = mid - 1;
  }
  return -1;
}

// UTF-8 → コードポイント列
static int decodeUtf8(const String &s, uint16_t *out, int maxLen) {
  int n = 0;
  const char *p = s.c_str();
  while (*p && n < maxLen) {
    uint8_t c = *p;
    uint32_t cp = 0; int ext = 0;
    if (c < 0x80) { cp = c; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; ext = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; ext = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; ext = 3; }
    else { p++; continue; }
    p++;
    for (int i = 0; i < ext; i++) {
      if ((*p & 0xC0) != 0x80) break;
      cp = (cp << 6) | (*p & 0x3F);
      p++;
    }
    if (cp <= 0xFFFF) out[n++] = (uint16_t)cp;
  }
  return n;
}

// テキストのピクセル幅
static int textWidth(const uint16_t *cps, int n) {
  int w = 0;
  for (int i = 0; i < n; i++) {
    int gi = glyphIndex(cps[i]);
    w += (gi < 0) ? 16 : pgm_read_byte(&FONT16_DATA[gi * 33]);
  }
  return w;
}

// 1文字描画 (x,y=左上)。クリッピングあり。戻り値=文字幅
static int drawGlyph(uint16_t cp, int x, int y, uint16_t color) {
  int gi = glyphIndex(cp);
  int w = 16;
  if (gi >= 0) w = pgm_read_byte(&FONT16_DATA[gi * 33]);
  if (x + w < 0 || x >= PANEL_W) return w;
  for (int r = 0; r < 16; r++) {
    int py = y + r;
    if (py < 0 || py >= PANEL_H) continue;
    uint16_t bits;
    if (gi >= 0) {
      bits = ((uint16_t)pgm_read_byte(&FONT16_DATA[gi * 33 + 1 + r * 2]) << 8)
           |  pgm_read_byte(&FONT16_DATA[gi * 33 + 2 + r * 2]);
    } else {
      bits = (r == 1 || r == 14) ? 0x7FFE : (r > 1 && r < 14) ? 0x4002 : 0; // 豆腐
    }
    for (int i = 0; i < w; i++) {
      if ((bits >> (15 - i)) & 1) {
        int px = x + i;
        if (px >= 0 && px < PANEL_W) display->drawPixel(px, py, color);
      }
    }
  }
  return w;
}

static void drawText(const uint16_t *cps, int n, int x, int y, uint16_t color) {
  for (int i = 0; i < n; i++) x += drawGlyph(cps[i], x, y, color);
}

// ---------------- 表示状態 ----------------
static uint16_t marqueeCps[1024];
static int marqueeLen = 0;
static int marqueeW = 0;
static float scrollX = PANEL_W;
static String comments[MAX_COMMENTS];
static int commentCount = 0;

// playlist.json で上書きされる実行時設定 (初期値はconfig.h)
static String plTopText = TOP_MESSAGE;
static String plMessages[MAX_PLAYLIST_MSGS];
static int plMsgCount = 0;
static float plSpeed = SCROLL_SPEED;
static uint16_t plColorTop = COLOR_TOP;
static uint16_t plColorScroll = COLOR_BOTTOM;
static bool plDualMode = true;

static void rebuildMarquee() {
  String t;
  // playlistのメッセージ + コメントを ◆ で連結して流す
  for (int i = 0; i < plMsgCount; i++) {
    if (t.length()) t += "　◆　";
    t += plMessages[i];
  }
  for (int i = 0; i < commentCount; i++) {
    if (t.length()) t += "　◆　";
    t += comments[i];
  }
  if (!t.length()) t = DEFAULT_MESSAGE;
  marqueeLen = decodeUtf8("　" + t, marqueeCps, 1024);
  marqueeW = textWidth(marqueeCps, marqueeLen);
  scrollX = PANEL_W;
}

// ---------------- HTTP取得 (http/https両対応) ----------------
static String httpGetString(const String &url, int &code) {
  HTTPClient http;
  String out;
  code = -1;
  http.setTimeout(5000);
  if (url.startsWith("https")) {
    NetworkClientSecure client;
    client.setInsecure();  // GitHub Pages等の証明書検証を省略 (表示内容のみなので許容)
    if (http.begin(client, url)) {
      code = http.GET();
      if (code == 200) out = http.getString();
      http.end();
    }
  } else {
    if (http.begin(url)) {
      code = http.GET();
      if (code == 200) out = http.getString();
      http.end();
    }
  }
  return out;
}

// ---------------- playlist.json ----------------
// 仕様は docs/playlist-spec.md を参照。例:
// { "topText":"営業中", "mode":"dual", "brightness":96, "speed":45,
//   "colorTop":"FF9C00", "colorScroll":"FF9C00",
//   "messages":["ようこそ「もしも」へ","次回イベントは8/20"] }
static uint16_t hexToColor565(const char *hex) {
  uint32_t v = strtoul(hex, nullptr, 16);
  uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static unsigned long lastPlaylist = 0;
static void fetchPlaylist() {
  if (strlen(PLAYLIST_URL) == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  // CDNキャッシュ回避のためクエリを付与
  String url = String(PLAYLIST_URL) + "?t=" + String(millis());
  int code;
  String body = httpGetString(url, code);
  if (code != 200 || !body.length()) {
    Serial.printf("[playlist] fetch failed (%d)\n", code);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("[playlist] JSON parse error");
    return;
  }
  if (doc["topText"].is<const char*>())  plTopText = String((const char*)doc["topText"]);
  if (doc["mode"].is<const char*>())     plDualMode = strcmp(doc["mode"], "scroll") != 0;
  if (doc["speed"].is<float>())          plSpeed = constrain((float)doc["speed"], 5.0f, 200.0f);
  if (doc["brightness"].is<int>())       display->setBrightness8(constrain((int)doc["brightness"], 8, 255));
  if (doc["colorTop"].is<const char*>())    plColorTop = hexToColor565(doc["colorTop"]);
  if (doc["colorScroll"].is<const char*>()) plColorScroll = hexToColor565(doc["colorScroll"]);
  if (doc["messages"].is<JsonArray>()) {
    plMsgCount = 0;
    for (JsonVariant v : doc["messages"].as<JsonArray>()) {
      if (plMsgCount >= MAX_PLAYLIST_MSGS) break;
      if (v.is<const char*>()) plMessages[plMsgCount++] = String((const char*)v);
    }
  }
  rebuildMarquee();
  Serial.println("[playlist] applied");
}

// 上段: 営業中/時計 交互
static void drawTopLine() {
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char buf[32];
  bool clockPhase = ((tmv.tm_sec / 6) % 2) == 1;
  if (clockPhase && tmv.tm_year > 100) {
    snprintf(buf, sizeof(buf), "%02d%s%02d", tmv.tm_hour, (tmv.tm_sec % 2) ? "：" : "　", tmv.tm_min);
  } else {
    snprintf(buf, sizeof(buf), "%s", plTopText.c_str());
  }
  static uint16_t cps[16];
  int n = decodeUtf8(String(buf), cps, 16);
  int w = textWidth(cps, n);
  drawText(cps, n, (PANEL_W - w) / 2, 0, plColorTop);
}

// ---------------- コメント取得 ----------------
static unsigned long lastFetch = 0;
static void fetchComments() {
  if (strlen(COMMENTS_URL) == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setTimeout(4000);
  http.begin(COMMENTS_URL);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();  // 1行1コメントのプレーンテキスト
    int newCount = 0;
    int from = 0;
    while (from < (int)body.length() && newCount < MAX_COMMENTS) {
      int nl = body.indexOf('\n', from);
      if (nl < 0) nl = body.length();
      String line = body.substring(from, nl);
      line.trim();
      if (line.length()) comments[newCount++] = line;
      from = nl + 1;
    }
    if (newCount > 0) {
      commentCount = newCount;
      rebuildMarquee();
    }
  }
  http.end();
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);

  HUB75_I2S_CFG mxconfig(PANEL_W, PANEL_H, 1);
  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  display->setBrightness8(BRIGHTNESS);
  display->clearScreen();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);

  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org");  // JST

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD)) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  rebuildMarquee();
}

void loop() {
  ArduinoOTA.handle();

  unsigned long ms = millis();
  if (ms - lastFetch > FETCH_INTERVAL_MS) {
    lastFetch = ms;
    fetchComments();
  }
  if (lastPlaylist == 0 || ms - lastPlaylist > PLAYLIST_INTERVAL_MS) {
    lastPlaylist = ms;
    fetchPlaylist();
  }

  // フレーム描画 (約30fps)
  static unsigned long lastFrame = 0;
  if (ms - lastFrame < 33) return;
  float dt = (ms - lastFrame) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  lastFrame = ms;

  scrollX -= plSpeed * dt;
  if (scrollX < -marqueeW) scrollX = PANEL_W;

  display->clearScreen();
  if (plDualMode && PANEL_H >= 32) {
    drawTopLine();
    drawText(marqueeCps, marqueeLen, (int)scrollX, 16, plColorScroll);
  } else {
    drawText(marqueeCps, marqueeLen, (int)scrollX, (PANEL_H - 16) / 2, plColorScroll);
  }
}
