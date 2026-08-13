/*
 * もしも電光掲示板 ファームウェア v0.7
 * ESP32 (WROOM-32) + HUB75 RGBマトリクスパネル 64x32 (P4)
 *
 * 機能:
 *  - efont Biwidth 16px (かな漢字7,444字) によるUTF-8テキスト描画
 *  - 2段表示: 上段「営業中」/時計(NTP)交互、下段コメントスクロール
 *  - playlist.json 定期取得: 表示文言・色・速度・輝度・モードをリモート更新 (v0.2)
 *    → GitHub Pages上のplaylist.jsonをClaude/メンバーが更新すると実機に反映される
 *  - 虹色スクロール: colorScroll:"rainbow" (v0.4)
 *  - ドット絵の静止画表示: mode:"frames" + frames[] (v0.5)
 *    → prototypes/draw/ で描いた絵のURL (d=...) をそのまま playlist に載せる
 *  - WiFi接続、HTTPポーリングによるコメント取得(15秒毎、URLはconfig.hで設定)
 *  - コマ送り: frames[].holdMs によるミリ秒指定 (v2.1先行)
 *  - ArduinoOTA: 初回USB書き込み後はWiFi経由で更新可能 (同一LAN内)
 *  - 自己アップデート: firmware/manifest.json を見て自分で新しい.binを取りに行く (v0.7)
 *    → 設置後もUSB・現地作業なしで更新できる。手順は docs/firmware-release.md
 *    → playlist.json の "fwPing" に新バージョン番号を書けば、周期を待たず即時反映
 *
 * 配線 (HUB75標準ピン割当はライブラリ既定値を使用):
 *   https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA を参照
 *   電源はパネルに5V別系統で供給すること (最大4A想定、表示内容なら2A程度)
 */

#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <time.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "config.h"
#include "font16.h"

// 2つ目のWiFiは任意。古い config.h でもビルドが通るように既定値を用意する。
#ifndef WIFI_SSID2
#define WIFI_SSID2 ""
#endif
#ifndef WIFI_PASS2
#define WIFI_PASS2 ""
#endif

// frames (v0.5) も同様に、古い config.h でビルドが通るように既定値を用意する。
// v2.1先行対応で 8 → 24 に拡張した。holdMs による高速コマ送りは枚数を食うため。
// MAX_FRAMES はRAM量で決まる実装上の上限であって設置場所ごとの設定ではないので、
// 古い config.h に 8 が残っていても 24 に引き上げる (下げる方向の上書きだけ無視する)。
#ifdef MAX_FRAMES
#if MAX_FRAMES < 24
#undef MAX_FRAMES
#define MAX_FRAMES 24
#endif
#else
#define MAX_FRAMES 24
#endif

// ドット絵は 64x32 固定 (prototypes/draw/ の s=1)。仕様は docs/playlist-spec.md を参照。
#define FRAME_W 64
#define FRAME_H 32
#define FRAME_BYTES ((FRAME_W * FRAME_H) / 8)  // 256バイト = base64url 342文字

// ---- 自己アップデート (v0.7) ----
// このビルドのバージョン。リリースごとに +1 する (手順: docs/firmware-release.md)。
// **公開する .bin はこの値を上げてビルドしたものであること。** manifest の version だけ
// 上げて .bin が古いままだと、実機は「更新したのにまだ古い」を延々繰り返す。
#define FW_VERSION 7

// 古い config.h でもビルドが通るように既定値を用意する。
#ifndef SELFUPDATE_MANIFEST_URL
#define SELFUPDATE_MANIFEST_URL "https://mugiwaraboushi.github.io/moshimo-sign/firmware/manifest.json"
#endif
#ifndef SELFUPDATE_INTERVAL_MS
#define SELFUPDATE_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)  // 6時間
#endif

// ---------------- パネル ----------------
MatrixPanel_I2S_DMA *display = nullptr;

// ---------------- WiFi ----------------
// 登録したAPのうち電波の届く方に自動接続する
static WiFiMulti wifiMulti;

static void wifiSetup() {
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID, WIFI_PASS);
  if (strlen(WIFI_SSID2)) wifiMulti.addAP(WIFI_SSID2, WIFI_PASS2);
  wifiMulti.run(10000);  // 最大10秒待つ
}

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

// ---------------- ビットマップ描画 (v0.5) ----------------
// 1ドット1bitのモノクロ画像を描く。文字描画と並ぶもう一つの「描画の口」。
// ビット順は prototypes/draw/ と同一: 左上から行優先、1バイト8ドット、MSBが左。
// (x,y=左上)。パネル外はクリッピングする。
//
// 点灯ドットだけでなく消灯ドットも黒で塗る (v2.1先行)。こうすると clearScreen() を
// 挟まず1パスで前の絵を置き換えられる。clearScreen してから描くと、その隙間をDMAが
// スキャンしたとき一瞬全消灯が見えるため。hold が秒単位なら気付かないが、holdMs で
// 数十msまで詰めると瞬断としてはっきり出る。
static void drawBitmapOpaque(const uint8_t *bits, int w, int h, int x, int y, uint16_t color) {
  for (int r = 0; r < h; r++) {
    int py = y + r;
    if (py < 0 || py >= PANEL_H) continue;
    for (int c = 0; c < w; c++) {
      int px = x + c;
      if (px < 0 || px >= PANEL_W) continue;
      int i = r * w + c;
      bool on = (bits[i >> 3] >> (7 - (i & 7))) & 1;
      display->drawPixel(px, py, on ? color : 0);
    }
  }
}

// base64url (パディング無し) をデコードする。戻り値=バイト数、不正なら -1。
// 不正なフレームは表示せず捨てるため、例外扱いを呼び出し側に返す。
static int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;
}

static int b64urlDecode(const char *s, uint8_t *out, int outCap) {
  uint32_t acc = 0;
  int bits = 0, n = 0;
  for (; *s; s++) {
    if (*s == '=') break;  // パディング付きで渡されても受け付ける
    int v = b64Value(*s);
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= outCap) return -1;  // 想定より長い = サイズ違いなので捨てる
      out[n++] = (uint8_t)(acc >> bits);
      acc &= (1UL << bits) - 1;
    }
  }
  return n;
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
static bool plRainbow = false;   // colorScroll:"rainbow" で虹色スクロール (v0.4)

// mode: "dual" / "scroll" / "frames" (v0.5)
enum DisplayMode { MODE_DUAL, MODE_SCROLL, MODE_FRAMES };
static DisplayMode plMode = MODE_DUAL;

// frames: ドット絵の静止画 (v0.5)。hold秒ごとに次の絵へ切り替えて先頭に戻る
static uint8_t plFrameData[MAX_FRAMES][FRAME_BYTES];
static uint16_t plFrameColor[MAX_FRAMES];
static uint16_t plFrameHoldMs[MAX_FRAMES];  // ミリ秒。hold(秒)もここへ1000倍して入れる
static int plFrameCount = 0;
static int frameIdx = 0;
static unsigned long frameSince = 0;      // 0 = 未開始 (次の描画で現在時刻を入れる)
static int drawnFrame = -1;               // 今パネルに描いてある絵 (-1 = 要再描画)

// HSV(h:0-359) → RGB565。虹色スクロール用
static uint16_t hsvToColor565(int h) {
  h %= 360; if (h < 0) h += 360;
  int region = h / 60, rem = (h % 60) * 255 / 60;
  int p = 0, q = 255 - rem, t = rem;
  int r, g, b;
  switch (region) {
    case 0: r = 255; g = t;   b = p;   break;
    case 1: r = q;   g = 255; b = p;   break;
    case 2: r = p;   g = 255; b = t;   break;
    case 3: r = p;   g = q;   b = 255; break;
    case 4: r = t;   g = p;   b = 255; break;
    default: r = 255; g = p;  b = q;   break;
  }
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// 1文字ごとに色相を24°ずつ進めた虹色で描画。基準色相は millis()/20 で回る
// (約7.2秒で一周)。v0.4では周期・回転速度のパラメータ化はしない(将来拡張)
static void drawTextRainbow(const uint16_t *cps, int n, int x, int y) {
  int hueBase = (int)((millis() / 20) % 360);
  for (int i = 0; i < n; i++) {
    uint16_t col = hsvToColor565(hueBase + i * 24);
    x += drawGlyph(cps[i], x, y, col);
  }
}

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

// ---------------- 自己アップデート (v0.7) ----------------
// manifest.json を見て、自分より新しいバージョンがあれば取りに行って書き込む。
// 形式: {"version":8, "url":"https://.../moshimo_sign.bin", "md5":"...", "size":1421728,
//        "quietHours":{"from":2,"to":5}}
// 手順は docs/firmware-release.md を参照。
//
// 直前に「このバージョンへ更新する」と決めて書き込んだ番号を RTCメモリに置く。
// ESP.restart() では消えず、電源断で消える。manifest の version だけ上げて .bin が
// 古いままだと更新→再起動→また更新…の無限ループになるので、その検出に使う。
RTC_DATA_ATTR static int rtcUpdatedTo = 0;

// 現在時刻(JST)が quietHours の範囲内か。範囲は from <= 時 < to。
// from > to なら日をまたぐ (例 22時〜5時)。clockOk=false は NTP未同期。
static bool inQuietHours(int from, int to, bool &clockOk) {
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  clockOk = (tmv.tm_year > 100);   // 上段の時計表示と同じ判定 (2000年より前なら未同期)
  if (!clockOk) return false;
  int h = tmv.tm_hour;
  return (from <= to) ? (h >= from && h < to) : (h >= from || h < to);
}

// ignoreQuiet=true で quietHours を無視する (playlist の fwPing 経由 = 人間の明示指示)。
static void selfUpdateCheck(bool ignoreQuiet) {
  if (strlen(SELFUPDATE_MANIFEST_URL) == 0) return;   // 空なら機能ごと無効
  if (WiFi.status() != WL_CONNECTED) return;

  int code;
  String body = httpGetString(String(SELFUPDATE_MANIFEST_URL) + "?t=" + String(millis()), code);
  if (code != 200 || !body.length()) {
    Serial.printf("[selfupdate] manifest fetch failed (%d)\n", code);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("[selfupdate] manifest JSON parse error");
    return;
  }
  int newVer        = doc["version"] | 0;
  const char *binUrl = doc["url"] | "";
  const char *md5    = doc["md5"] | "";
  size_t size       = (size_t)(doc["size"] | 0);

  // ダウングレード禁止。同じバージョンも「何もしない」が正常。
  if (newVer <= FW_VERSION) {
    Serial.printf("[selfupdate] up to date (manifest v%d / running v%d)\n", newVer, FW_VERSION);
    return;
  }
  if (!strlen(binUrl) || !strlen(md5) || size == 0) {
    Serial.println("[selfupdate] manifest incomplete (url/md5/size のいずれかが無い)");
    return;
  }
  // 前回このバージョンへ更新したはずなのに、まだ FW_VERSION が古い
  // = 公開されている .bin の FW_VERSION が manifest と食い違っている。
  if (rtcUpdatedTo == newVer) {
    Serial.printf("[selfupdate] abort: v%d を書き込み済みのはずが FW_VERSION=%d のまま。"
                  ".bin と manifest が不一致\n", newVer, FW_VERSION);
    return;
  }
  if (!ignoreQuiet && doc["quietHours"].is<JsonObject>()) {
    int from = doc["quietHours"]["from"] | 0;
    int to   = doc["quietHours"]["to"] | 0;
    bool clockOk = false;
    bool inRange = inQuietHours(from, to, clockOk);
    if (!clockOk) {
      Serial.println("[selfupdate] postpone: NTP未同期で時刻が信用できない");
      return;
    }
    if (!inRange) {
      Serial.printf("[selfupdate] postpone: quietHours %d-%d時の外\n", from, to);
      return;
    }
  }

  Serial.printf("[selfupdate] start: v%d -> v%d (%u bytes)\n", FW_VERSION, newVer, (unsigned)size);

  // 書き込み中は描画も playlist取得も止まる (数十秒)。失敗しても現行のまま動き続ける。
  NetworkClientSecure client;
  HTTPClient http;
  http.setTimeout(20000);
  String burl = String(binUrl);
  bool begun;
  if (burl.startsWith("https")) {
    client.setInsecure();   // httpGetString と同じ方針 (MD5で中身は検証する)
    begun = http.begin(client, burl);
  } else {
    begun = http.begin(burl);
  }
  if (!begun) { Serial.println("[selfupdate] http begin failed"); return; }

  int bcode = http.GET();
  if (bcode != 200) {
    Serial.printf("[selfupdate] bin fetch failed (%d)\n", bcode);
    http.end();
    return;
  }
  int len = http.getSize();
  if (len > 0 && (size_t)len != size) {
    Serial.printf("[selfupdate] size mismatch (manifest %u / server %d)\n", (unsigned)size, len);
    http.end();
    return;
  }
  if (!Update.begin(size)) {
    Serial.printf("[selfupdate] Update.begin failed: %s\n", Update.errorString());
    http.end();
    return;
  }
  Update.setMD5(md5);   // 書き込み後に照合される。合わなければ end() が失敗する
  size_t written = Update.writeStream(http.getStream());
  if (written != size) {
    Serial.printf("[selfupdate] write incomplete (%u/%u)\n", (unsigned)written, (unsigned)size);
    Update.abort();
    http.end();
    return;
  }
  if (!Update.end(true)) {
    Serial.printf("[selfupdate] verify failed: %s\n", Update.errorString());
    http.end();
    return;
  }
  http.end();
  rtcUpdatedTo = newVer;
  Serial.printf("[selfupdate] ok: v%d を書き込んだ。再起動する\n", newVer);
  Serial.flush();
  delay(200);
  ESP.restart();
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
  if (doc["mode"].is<const char*>()) {
    const char *m = doc["mode"];
    plMode = (strcmp(m, "scroll") == 0) ? MODE_SCROLL
           : (strcmp(m, "frames") == 0) ? MODE_FRAMES
                                        : MODE_DUAL;
  }
  if (doc["speed"].is<float>())          plSpeed = constrain((float)doc["speed"], 5.0f, 200.0f);
  if (doc["brightness"].is<int>())       display->setBrightness8(constrain((int)doc["brightness"], 8, 255));
  if (doc["colorTop"].is<const char*>())    plColorTop = hexToColor565(doc["colorTop"]);
  if (doc["colorScroll"].is<const char*>()) {
    const char *cs = doc["colorScroll"];
    plRainbow = (strcmp(cs, "rainbow") == 0);
    if (!plRainbow) plColorScroll = hexToColor565(cs);
  }
  if (doc["messages"].is<JsonArray>()) {
    plMsgCount = 0;
    for (JsonVariant v : doc["messages"].as<JsonArray>()) {
      if (plMsgCount >= MAX_PLAYLIST_MSGS) break;
      if (v.is<const char*>()) plMessages[plMsgCount++] = String((const char*)v);
    }
  }
  // frames: 64x32のドット絵 (v0.5)。壊れたフレームはそれだけ捨て、残りは表示する
  if (doc["frames"].is<JsonArray>()) {
    int n = 0;
    for (JsonVariant v : doc["frames"].as<JsonArray>()) {
      if (n >= MAX_FRAMES) break;
      if (!v["d"].is<const char*>()) continue;
      if (b64urlDecode(v["d"], plFrameData[n], FRAME_BYTES) != FRAME_BYTES) {
        Serial.println("[playlist] frame skipped (data size mismatch)");
        continue;
      }
      plFrameColor[n] = v["color"].is<const char*>() ? hexToColor565(v["color"]) : plColorTop;
      // holdMs (ミリ秒) が指定されていればそちらを優先し、無ければ hold (秒) を使う。
      // 下限20msは1コマ50fps相当。これより短い指定はパネルの走査が追いつかない。
      if (v["holdMs"].is<float>())
        plFrameHoldMs[n] = constrain((int)(float)v["holdMs"], 20, 60000);
      else if (v["hold"].is<float>())
        plFrameHoldMs[n] = constrain((int)(float)v["hold"], 1, 60) * 1000;
      else
        plFrameHoldMs[n] = 8000;
      n++;
    }
    plFrameCount = n;
    if (frameIdx >= plFrameCount) frameIdx = 0;
    frameSince = 0;   // 差し替え直後は先頭からhold秒数え直す
    drawnFrame = -1;  // 絵や色が変わっているので描き直す
  }
  rebuildMarquee();
  Serial.println("[playlist] applied");

  // fwPing (v0.7): チャット駆動の即時更新。playlist に自分より新しい番号が書かれていたら
  // 6時間周期を待たずにその場でマニフェストを見に行く。人間の明示的な指示なので
  // quietHours は無視する。実際に更新するかどうかの判断は selfUpdateCheck 側。
  if (doc["fwPing"].is<int>() && (int)doc["fwPing"] > FW_VERSION) {
    Serial.printf("[selfupdate] fwPing=%d (running v%d) → 即時確認\n",
                  (int)doc["fwPing"], FW_VERSION);
    selfUpdateCheck(true);
  }
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

  wifiSetup();

  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org");  // JST

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD)) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  rebuildMarquee();
}

void loop() {
  ArduinoOTA.handle();

  unsigned long ms = millis();

  // 切断時のみ再接続を試みる。もう一方のAPが届いていればそちらへ切り替わる。
  // run() はスキャンのため数秒ブロックするので、繋がっている間は呼ばない。
  static unsigned long lastWifiRetry = 0;
  if (WiFi.status() != WL_CONNECTED && ms - lastWifiRetry > 30000) {
    lastWifiRetry = ms;
    wifiMulti.run(5000);
  }

  if (ms - lastFetch > FETCH_INTERVAL_MS) {
    lastFetch = ms;
    fetchComments();
  }
  if (lastPlaylist == 0 || ms - lastPlaylist > PLAYLIST_INTERVAL_MS) {
    lastPlaylist = ms;
    fetchPlaylist();
  }

  // 自己アップデート (v0.7): 起動60秒後に初回、以後 SELFUPDATE_INTERVAL_MS ごと。
  // 起動直後を避けるのは、WiFi接続とNTP同期が済むのを待つため。
  static unsigned long lastSelfUpdate = 0;
  if (lastSelfUpdate == 0) {
    if (ms > 60000) { lastSelfUpdate = ms; selfUpdateCheck(false); }
  } else if (ms - lastSelfUpdate > SELFUPDATE_INTERVAL_MS) {
    lastSelfUpdate = ms;
    selfUpdateCheck(false);
  }

  // ドット絵モード (v0.5 / holdMs は v2.1先行)。文字は出さず、画をパネル中央に置く。
  // frames が0枚のときは黒画面にせず下の2段表示にフォールバックする。
  //
  // 下の「約30fps」の間引きより前に置いてある。間引いた後だと切り替え判定が33ms刻みに
  // 量子化され、holdMs:52 のような指定が 66ms (33の倍数) に丸められて本来の速さで
  // 動かなくなるため。ここは自前で「絵が変わったときだけ描く」ので毎回通っても軽い。
  if (plMode == MODE_FRAMES && plFrameCount > 0) {
    if (frameSince == 0) frameSince = ms;
    // 溜まった遅れぶんコマを進める。1周分を超えたら追いつくのを諦めて現在時刻に合わせる
    // (WiFi取得などで数百ms止まったあと、早送りで一気に流れるのを防ぐ)。
    unsigned int guard = 0;
    while (ms - frameSince >= (unsigned long)plFrameHoldMs[frameIdx]) {
      frameSince += plFrameHoldMs[frameIdx];
      frameIdx = (frameIdx + 1) % plFrameCount;
      if (++guard >= (unsigned int)plFrameCount) { frameSince = ms; break; }
    }
    // 絵が変わったときだけ描く。消灯ドットも塗る版を使うので clearScreen は挟まない
    // (挟むと高速なコマ送りで瞬断が見える)。
    if (drawnFrame != frameIdx) {
      drawnFrame = frameIdx;
      drawBitmapOpaque(plFrameData[frameIdx], FRAME_W, FRAME_H,
                       (PANEL_W - FRAME_W) / 2, (PANEL_H - FRAME_H) / 2,
                       plFrameColor[frameIdx]);
    }
    return;
  }
  drawnFrame = -1;  // 文字表示に戻った = パネルの内容が流れるので、次回frames時は描き直す

  // 文字表示のフレーム描画 (約30fps)
  static unsigned long lastFrame = 0;
  if (ms - lastFrame < 33) return;
  float dt = (ms - lastFrame) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  lastFrame = ms;

  scrollX -= plSpeed * dt;
  if (scrollX < -marqueeW) scrollX = PANEL_W;

  display->clearScreen();
  if (plMode != MODE_SCROLL && PANEL_H >= 32) {
    drawTopLine();
    if (plRainbow) drawTextRainbow(marqueeCps, marqueeLen, (int)scrollX, 16);
    else           drawText(marqueeCps, marqueeLen, (int)scrollX, 16, plColorScroll);
  } else {
    const int y = (PANEL_H - 16) / 2;
    if (plRainbow) drawTextRainbow(marqueeCps, marqueeLen, (int)scrollX, y);
    else           drawText(marqueeCps, marqueeLen, (int)scrollX, y, plColorScroll);
  }
}
