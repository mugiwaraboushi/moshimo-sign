/*
 * もしも電光掲示板 ファームウェア v0.12
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
 *  - WiFi接続、HTTPポーリングによるコメント取得(15秒毎)
 *  - イベントコメント: playlist.json の "commentsUrl" で取得先を指定できる (v0.10)
 *    → GAS(302リダイレクト)にも届くようになり、URL変更のたびの書き込みが不要になった
 *    → 画面と手順は prototypes/event-comments/ と docs/event-comments.md
 *  - コマ送り: frames[].holdMs によるミリ秒指定 (v2.1先行)
 *  - ArduinoOTA: 初回USB書き込み後はWiFi経由で更新可能 (同一LAN内)
 *  - 自己アップデート: firmware/manifest.json を見て自分で新しい.binを取りに行く (v0.7)
 *    → 設置後もUSB・現地作業なしで更新できる。手順は docs/firmware-release.md
 *    → playlist.json の "fwPing" に新バージョン番号を書けば、周期を待たず即時反映
 *  - 起動時のバージョン表示: 右下に版数を数秒出す (v0.9)
 *    → 実機が今どの版かを、電源を挿し直してパネルを見るだけで確認できる
 *  - 取得の非同期化: playlistもコメントも取得は別タスク、反映だけloop() (v0.12)
 *    → 60秒ごとにスクロールが止まって表示がリセットされて見える問題を解消
 *    → 1本のタスクで playlist(60秒) と コメント(15秒) を回す
 *  - WiFiモデムスリープ停止: 常時給電なのでOTAと応答を優先する (v0.12)
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
#define FW_VERSION 12

// ---- イベントコメント (v0.10) ----
// 取得先は playlist.json の "commentsUrl" でも指定できる。config.h の値は初期値。
// 古い config.h でもビルドが通るように既定値を用意する。
#ifndef COMMENTS_URL
#define COMMENTS_URL ""
#endif

// playlist.json から受け付けるURLの頭。playlist.json は公開リポジトリにあり誰でも書けるので、
// 「実機が任意のホストを叩きにいく」状態にはしない (v2.1 の frames[].src を相対パスだけに
// 限っているのと同じ考え方)。GAS以外の置き場を使うときは config.h の COMMENTS_URL を使う
// = ファームウェア担当の手を通す。
#ifndef COMMENTS_URL_PREFIX
#define COMMENTS_URL_PREFIX "https://script.google.com/macros/"
#endif

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
  // モデムスリープを切る (v0.12)。既定では受信の合間に電波部を落として省電力にするが、
  // この掲示板は常時給電なので節電の必要がない。切っておくと OTA の取りこぼしが減り、
  // playlist/コメント取得の初回応答も安定する (スリープ復帰待ちが挟まらなくなる)。
  WiFi.setSleep(false);
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
static String plCommentsUrl = COMMENTS_URL;   // commentsUrl で差し替え可能 (v0.10)
// plCommentsUrl と受け渡し用バッファは取得タスクと共有する。
// 詳細は「ネットワーク取得タスク」の節。
static SemaphoreHandle_t fetchMutex = nullptr;

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

// いま流している本文。中身が変わったときだけ組み直すための控え (v0.10)
static String marqueeText;

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
  // 中身が同じなら何もしない (v0.10)。組み直すと scrollX が右端に戻るため、
  // 60秒ごとのplaylist取得・15秒ごとのコメント取得のたびに流れが頭から再開し、
  // 一周が取得間隔より長い文面は末尾が永久に出てこない。
  if (t == marqueeText) return;
  marqueeText = t;
  marqueeLen = decodeUtf8("　" + t, marqueeCps, 1024);
  marqueeW = textWidth(marqueeCps, marqueeLen);
  scrollX = PANEL_W;
}

// ---------------- HTTP取得 (http/https両対応) ----------------
// TLSは1本あたり数十KBのヒープを食う。v0.10でコメント取得が別タスクになり、
// playlist取得・自己アップデートと同時に走りうるようになったので、
// **通信は同時に1本まで**に制限する。取れなかったときは code = -2 を返して見送る
// (loop()側から呼ぶときは待たない。待つとスクロールが止まるため)。
static SemaphoreHandle_t netMutex = nullptr;

// httpGetString を通さない通信 (自己アップデートの .bin 取得) 用。
// 途中でreturnしても必ず放すように、スコープを抜けるときに解放する。
// **httpGetString の中と入れ子にしないこと** (再帰不可のmutexなので固まる)。
struct NetLock {
  bool held;
  NetLock() { held = netMutex && xSemaphoreTake(netMutex, portMAX_DELAY) == pdTRUE; }
  ~NetLock() { if (held) xSemaphoreGive(netMutex); }
};

static String httpGetString(const String &url, int &code, bool wait = false) {
  HTTPClient http;
  String out;
  code = -1;
  if (netMutex && xSemaphoreTake(netMutex, wait ? portMAX_DELAY : 0) != pdTRUE) {
    code = -2;   // 別の取得が進行中
    return out;
  }
  http.setTimeout(5000);
  // リダイレクト追従 (v0.10)。GASの /exec は script.googleusercontent.com への302を返すので、
  // 追わないと本文が取れない。302(FOUND)はSTRICTでも追われるのでこれで足りる。
  // リダイレクト先が別ホストでも、プロトコルが同じ(https→https)なら HTTPClient::setURL が通す。
  if (url.startsWith("https")) {
    NetworkClientSecure client;
    client.setInsecure();  // GitHub Pages等の証明書検証を省略 (表示内容のみなので許容)
    if (http.begin(client, url)) {
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      code = http.GET();
      if (code == 200) out = http.getString();
      http.end();
    }
  } else {
    if (http.begin(url)) {
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      code = http.GET();
      if (code == 200) out = http.getString();
      http.end();
    }
  }
  if (netMutex) xSemaphoreGive(netMutex);
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
  // fwPing 経由 (ignoreQuiet=true) は「人がいま押した」指示なので、取得タスクと重なっても
  // 見送らずに順番を待つ (v0.12)。待たないと -2 で空振りし、反応が次のplaylist取得まで
  // 最大1分遅れる。逆に6時間ごとの定期確認 (ignoreQuiet=false) は待たない —— ここで待つと
  // 6時間に一度スクロールが数秒止まることになり、シームレス化の趣旨に反する。
  // 定期確認は空振りしても次の周期で拾えばよい。
  String body = httpGetString(String(SELFUPDATE_MANIFEST_URL) + "?t=" + String(millis()),
                              code, ignoreQuiet);
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

  // 書き込み中は他の通信を止める。コメント取得タスクが同時にTLSを張ると
  // ヒープを奪い合って書き込みが失敗しうるため (v0.10)。
  NetLock netlock;

  // 書き込み中は描画も playlist取得も止まる (数十秒)。失敗しても現行のまま動き続ける。
  NetworkClientSecure client;
  HTTPClient http;
  http.setTimeout(20000);
  // manifest と同じくキャッシュバスターを付ける。PagesのCDNが古い .bin を返すと
  // manifest の新しいmd5と合わず失敗する (md5検証で弾かれるので壊れはしないが、
  // 次の周期まで更新が進まない)。
  String burl = String(binUrl);
  burl += (burl.indexOf('?') >= 0 ? "&t=" : "?t=") + String(millis());
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

// playlist取得は「取りに行く」と「反映する」に分かれている (v0.12)。
//
//   fetchPlaylistBody()  … HTTPだけ。取得タスク側で走る (TLSで1〜2秒止まる)
//   applyPlaylistBody()  … JSONパースと表示への反映。loop()側で走る (速い)
//
// v0.11以前は loop() の中で両方やっていたため、60秒ごとに取得のあいだスクロールが
// 止まり、そこで表示がリセットされたように見えていた。これがv12の主目的。
// 分けたことで、描画を止めるのはパースと反映だけになる。

// HTTPだけ。取れたら true を返し、body に本文を入れる。
// false = 今回は取れなかった (呼び手が間隔を詰めて再挑戦する)
static bool fetchPlaylistBody(String &body, bool wait) {
  if (strlen(PLAYLIST_URL) == 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  // CDNキャッシュ回避のためクエリを付与
  String url = String(PLAYLIST_URL) + "?t=" + String(millis());
  int code;
  String out = httpGetString(url, code, wait);
  if (code != 200 || !out.length()) {
    Serial.printf("[playlist] fetch failed (%d)\n", code);
    return false;   // -2 = 他の取得と重なった。すぐ再挑戦してよい (v0.10)
  }
  body = out;
  return true;
}

// 直前に反映した本文と、そこに書かれていた fwPing (v0.12)。
// 本文まるごと持つのは数KBのヒープを使うが、frames が最大24枚あると
// 「変わったかどうか」をハッシュで代用したときの取りこぼし (更新が永久に無視される)
// のほうが痛い。実機は設置済みで直しに行けないので、確実な一致比較にする。
static String lastAppliedPlaylist;
static int    lastFwPing = 0;

// パースと反映。呼ぶのは loop() 側だけ (表示・フォント・ヒープを触るため)。
// 戻り値は「取り直す意味があるか」ではなく「JSONとして読めたか」。
static bool applyPlaylistBody(const String &body) {
  // **中身が1バイトも変わっていないなら、パースも反映もしない (v0.12)。**
  // 通常運転では60秒ごとに同じ本文が返る。それを毎回パースすると、JSONの読み取りと
  // frames のbase64デコード (最大24枚×342文字) で loop() が数十ms止まる。
  // holdMs:52 のコマ送りでは1〜2コマぶんの引っかかりになって目に見えるため、
  // 変化が無いときは丸ごと省く。取得の非同期化と合わせて、これで60秒周期の
  // 引っかかりが無くなる。
  if (lastAppliedPlaylist.length() && body == lastAppliedPlaylist) {
    // 表示の反映は省くが、fwPing だけは毎回見る。manifest の公開がCDNに行き渡る前に
    // fwPing を拾った場合、ここを省くと次に playlist の中身が変わるまで
    // (最悪6時間の定期確認まで) 更新が始まらなくなるため。v0.11以前と同じ挙動。
    if (lastFwPing > FW_VERSION) {
      Serial.printf("[selfupdate] fwPing=%d (running v%d) → 即時確認\n", lastFwPing, FW_VERSION);
      selfUpdateCheck(true);
    }
    Serial.println("[playlist] no change");
    return true;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("[playlist] JSON parse error");
    return false;   // 壊れたJSONを3秒ごとに取り直しても直らない。次の周期を待つ
  }
  lastAppliedPlaylist = body;
  lastFwPing = doc["fwPing"].is<int>() ? (int)doc["fwPing"] : 0;
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
  // commentsUrl (v0.10): イベントコメントの取得先。空文字なら取得を止める。
  // 頭が COMMENTS_URL_PREFIX と違うものは無視する (公開ファイル経由で任意のホストを
  // 叩かせないため)。既定は Google Apps Script のみ。
  if (doc["commentsUrl"].is<const char*>()) {
    String u = String((const char*)doc["commentsUrl"]);
    if (u.length() == 0 || u.startsWith(COMMENTS_URL_PREFIX)) {
      if (u != plCommentsUrl) {
        // 取得タスクが同じ String を読んでいるので、書き換えはmutexの中で行う
        if (fetchMutex) xSemaphoreTake(fetchMutex, portMAX_DELAY);
        plCommentsUrl = u;
        if (fetchMutex) xSemaphoreGive(fetchMutex);
        commentCount = 0;   // 取得先が変わったので前の置き場のコメントは持ち越さない
        Serial.printf("[comments] url set (%d文字)\n", (int)u.length());
      }
    } else {
      Serial.println("[comments] commentsUrl rejected (prefix mismatch)");
    }
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
  return true;
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

// ---------------- ネットワーク取得タスク ----------------
// **取得は別タスク (core 0) で行う。** GASの応答は実測で2〜3秒、playlist も TLS込みで
// 1〜2秒かかる。loop() の中で待つと、その間スクロールが止まって見える —— コメントは
// 15秒ごと、playlist は60秒ごとなので、イベント中ずっと目につく。
// そこで「取りに行く」のは別タスク、「表示に反映する」のは loop() 側、と分ける。
//
// **コメントとplaylistを1本のタスクに同居させている (v0.12)。**
// 2本に分けるとTLSバッファぶんのスタックが二重に要る (20KB×2) うえ、どのみち
// netMutex で通信は1本ずつに直列化されるので、並列にしても速くならない。
// 1本にまとめれば取得が自然に順番待ちになり、netMutex で弾かれる空振りも減る。
//
// タスクとloop()の間で受け渡すのは下の受け渡しバッファだけ。
// String は同時に触ると壊れるので、受け渡しは必ず fetchMutex の中で行う。
static unsigned long lastFetch = 0;
static String fetchedBody;                 // コメント本文 (mutexの中でだけ触る)
static volatile bool fetchedReady = false; // 未反映のコメント本文がある
static String fetchedPlBody;               // playlist本文 (mutexの中でだけ触る)
static volatile bool fetchedPlReady = false;// 未反映のplaylist本文がある
static volatile bool fetchAsync = false;   // タスクが動いている (falseならloop()側で取る)

// 受け取った本文を comments[] に展開する。呼ぶのは loop() 側だけ。
static void applyCommentsBody(const String &body) {
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
  // 0件でも反映する (v0.10)。管理画面で全部を見送りに戻したとき、以前は newCount>0 の
  // ときしか差し替えなかったため、下げたはずのコメントが流れ続けた。
  commentCount = newCount;
  rebuildMarquee();   // 中身が同じなら何も起きない (スクロール位置は保たれる)
}

// 1回ぶんの取得。playlistと同じ経路 (httpGetString) を通す (v0.10)。
// v0.9以前は http.begin(url) を直に呼んでいて **リダイレクトを追わなかった** ため、
// 302を返すGASからは本文が取れなかった
// (HTTPClientの _followRedirects の既定は HTTPC_DISABLE_FOLLOW_REDIRECTS)。
// 取得できたら true を返し、body に本文を入れる。
static bool fetchCommentsOnce(String &body) {
  String url;
  if (fetchMutex) xSemaphoreTake(fetchMutex, portMAX_DELAY);
  url = plCommentsUrl;
  if (fetchMutex) xSemaphoreGive(fetchMutex);
  if (url.length() == 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  // キャッシュ回避のクエリもplaylistと同様に付ける
  url += (url.indexOf('?') >= 0 ? "&t=" : "?t=") + String(millis());
  int code;
  // 別タスクなので、他の取得が終わるのを待ってよい (待っても表示は止まらない)
  String out = httpGetString(url, code, fetchAsync);   // 1行1コメントのプレーンテキスト
  if (code != 200) {
    Serial.printf("[comments] fetch failed (%d)\n", code);
    return false;   // 通信断で表示中のコメントが消えないよう、失敗時は前回の内容を残す
  }
  body = out;
  return true;
}

// 取得タスク本体。コメント (15秒) と playlist (60秒 / 失敗時3秒) を1本で回す。
// ここでやるのは **HTTPと受け渡しだけ**。パースも表示も触らない。
static void fetchTask(void *arg) {
  (void)arg;
  // どちらも起動直後に1回目を走らせる (nextXxx = 0)。
  // playlist を先に置くのは、commentsUrl と表示設定がそこから来るため。
  unsigned long nextPlaylist = 0;
  unsigned long nextComments = 0;
  const bool havePlaylistUrl = strlen(PLAYLIST_URL) > 0;

  for (;;) {
    unsigned long ms = millis();

    // ---- playlist ----
    // 比較を符号付きで行い、millis()が一周しても止まらないようにする。
    if (havePlaylistUrl && (long)(ms - nextPlaylist) >= 0) {
      String body;
      bool got = fetchPlaylistBody(body, true);
      if (got) {
        xSemaphoreTake(fetchMutex, portMAX_DELAY);
        fetchedPlBody = body;
        fetchedPlReady = true;
        xSemaphoreGive(fetchMutex);
      }
      // 取れなかったときは3秒後に再挑戦する (v0.10の意味論を維持)。
      // 取れた場合は、loop()側でJSONが壊れていても取り直さない —— 壊れたJSONを
      // 3秒ごとに取り直しても直らないので、次の60秒周期に任せる。
      nextPlaylist = millis() + (got ? PLAYLIST_INTERVAL_MS : 3000);
    }

    // ---- コメント ----
    // plCommentsUrl が空のときは通信しない。**これが緊急停止弁**: 万一この経路で
    // 不具合が出ても、playlist.json の commentsUrl を "" にすれば取得が止まる
    // (実機は設置済みで、USBでの復旧が難しいため残してある)。
    // v0.11以前は「commentsUrlが入るまでタスクごと起こさない」だったが、v0.12では
    // playlist取得も同じタスクが担うので、タスク自体は起動時から回す。
    if ((long)(ms - nextComments) >= 0) {
      String body;
      if (fetchCommentsOnce(body)) {
        xSemaphoreTake(fetchMutex, portMAX_DELAY);
        fetchedBody = body;
        fetchedReady = true;
        xSemaphoreGive(fetchMutex);
      }
      nextComments = millis() + FETCH_INTERVAL_MS;
    }

    vTaskDelay(pdMS_TO_TICKS(100));   // 次の予定を見に行くだけの間隔
  }
}

// loop() から呼ぶ。タスクが持ってきた本文があれば表示に反映する (待たない)。
static void applyFetchedComments() {
  if (!fetchedReady) return;
  String body;
  xSemaphoreTake(fetchMutex, portMAX_DELAY);
  body = fetchedBody;
  fetchedBody = "";      // 持ち回らない (最大20行ぶんのヒープを解放する)
  fetchedReady = false;
  xSemaphoreGive(fetchMutex);
  applyCommentsBody(body);
}

// loop() から呼ぶ。タスクが持ってきた playlist があればパースして反映する (待たない)。
// fwPing → selfUpdateCheck(true) もこの中 (= loop()側) から呼ばれる。
static void applyFetchedPlaylist() {
  if (!fetchedPlReady) return;
  String body;
  xSemaphoreTake(fetchMutex, portMAX_DELAY);
  body = fetchedPlBody;
  fetchedPlBody = "";    // 持ち回らない (playlist本文は数KBある)
  fetchedPlReady = false;
  xSemaphoreGive(fetchMutex);
  applyPlaylistBody(body);
}

// タスクを作れなかったときの退路。従来どおり loop() の中で取りに行く
// (2〜3秒止まるが、表示が更新できないよりはよい)。
static void fetchCommentsBlocking() {
  String body;
  if (fetchCommentsOnce(body)) applyCommentsBody(body);
}

// 同上。戻り値は「次を60秒後にしてよいか」(false なら3秒後に再挑戦)。
static bool fetchPlaylistBlocking() {
  if (strlen(PLAYLIST_URL) == 0) return true;
  String body;
  if (!fetchPlaylistBody(body, false)) return false;
  applyPlaylistBody(body);
  return true;   // 取得はできている。JSONが壊れていても取り直しは次の周期で
}

// setup()から呼ぶ。ここでは入れ物だけ作る (タスクは fetchTaskStart で起こす)。
static void fetchSetup() {
  netMutex   = xSemaphoreCreateMutex();      // 通信は同時に1本まで (httpGetString)
  fetchMutex = xSemaphoreCreateMutex();      // タスクとloop()の受け渡しバッファを守る
  if (!fetchMutex) Serial.println("[fetch] mutex作成に失敗。loop()側で取得する");
}

// 取得タスクを起こす。setup() から1回だけ呼ぶ。
static bool fetchTaskTried = false;
static void fetchTaskStart() {
  if (fetchTaskTried) return;
  fetchTaskTried = true;
  if (!fetchMutex) return;   // 入れ物が無い。loop()側の従来経路で取る
  // スタックはTLSハンドシェイクぶんに余裕を見て20KB。WiFiと同じcore 0、
  // 優先度は loopTask と同じ1にして描画を邪魔しない。
  BaseType_t ok = xTaskCreatePinnedToCore(fetchTask, "fetch", 20480, nullptr, 1, nullptr, 0);
  if (ok != pdPASS) {
    Serial.println("[fetch] タスク作成に失敗。loop()側で取得する");
    return;
  }
  fetchAsync = true;
  Serial.println("[fetch] 取得タスクを開始 (core 0 / playlist+コメント)");
}

// ---------------- 起動時のバージョン表示 (v0.9) ----------------
// 実機が今どの版かを「電源を挿し直してパネルを見る」だけで確認できるようにする。
// 遠隔からバージョンを問い合わせる手段が無いため (v8の検証で分かった)。
// 右下に控えめな色で「v9」と出す。通常表示に入ると loop() 側の描画で自然に消える。
#define BOOT_VERSION_MIN_MS 3000   // 電波が良いと起動が数秒で終わるので、読める時間を確保する

static void drawBootVersion() {
  char buf[16];
  snprintf(buf, sizeof(buf), "v%d", FW_VERSION);
  uint16_t cps[8];
  int n = decodeUtf8(String(buf), cps, 8);
  int w = textWidth(cps, n);
  drawText(cps, n, PANEL_W - w, PANEL_H - 16, hexToColor565("505050"));
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);

  HUB75_I2S_CFG mxconfig(PANEL_W, PANEL_H, 1);
  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  display->setBrightness8(BRIGHTNESS);
  display->clearScreen();

  drawBootVersion();
  unsigned long bootShownAt = millis();
  Serial.printf("[boot] moshimo-sign FW_VERSION=%d\n", FW_VERSION);

  wifiSetup();

  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org");  // JST

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  if (strlen(OTA_PASSWORD)) ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  fetchSetup();       // 受け渡しの入れ物を作る (v0.10 / 名前と役割を v0.12 で整理)
  fetchTaskStart();   // playlist+コメントの取得タスクを起こす (v0.12)

  rebuildMarquee();

  // WiFiがすぐ繋がると起動処理が数秒で終わり、版数を読む前に通常表示へ移ってしまう。
  // 目視確認が目的なので、最低 BOOT_VERSION_MIN_MS は出したままにする。
  while (millis() - bootShownAt < BOOT_VERSION_MIN_MS) delay(50);
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

  // コメントもplaylistも、取得は別タスク (v0.12)。ここでは持ってきた本文を
  // 反映するだけなので待たない = スクロールが止まらない。
  // タスクを作れなかったときだけ、従来どおりここで取りに行く。
  if (fetchAsync) {
    applyFetchedComments();
    applyFetchedPlaylist();
  } else {
    if (plCommentsUrl.length() && ms - lastFetch > FETCH_INTERVAL_MS) {
      lastFetch = ms;
      fetchCommentsBlocking();
    }
    // 取れなかったときは3秒後に再挑戦する (v0.10)。他の取得と重なって見送った場合に
    // 60秒待つと、「実機に1〜2分で反映」が守れなくなるため。
    // 比較を符号付きで行い、millis()が一周しても止まらないようにする。
    static unsigned long nextPlaylistAt = 0;
    if ((long)(ms - nextPlaylistAt) >= 0) {
      bool ok = fetchPlaylistBlocking();
      nextPlaylistAt = ms + (ok ? PLAYLIST_INTERVAL_MS : 3000);
    }
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
