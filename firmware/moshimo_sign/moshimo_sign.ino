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
 *  - 営業カレンダー: data/hours.json を実機が直接読んで OPEN/CLOSED を自分で判定 (v16)
 *    → playlist.json の "hoursUrl" で取得先を指定する。空文字なら従来どおり
 *    → GitHub Actions 側の自動書き換えも残す (二重化。判定ルールは同じなので矛盾しない)
 *  - 認証情報をNVSへ: WiFi/OTAの認証情報を .bin に埋めず Preferences から読む (v17)
 *    → 公開している .bin から SSID/パスワードが取り出せてしまう状態をやめるため
 *    → NVSに無ければ config.h に戻る移行版。投入はUSBのシリアルから (手順: docs/firmware-build.md)
 *  - イベント中の表示設定: commentsUrl の JSON 応答の "settings" で色・速さ・大きさ・段数を上書き (v19)
 *    → mode:"event" のときだけ効く。テキスト応答 (1行1件) は従来どおり読める
 *  - ブース用キーボード入力 (say): sayEnabled のときだけ LAN 内に入力ページを開く (v20)
 *    → 打った文字を 32px 太字でその場に出す。GAS を通さないので待ちもネットも要らない
 *    → eggWords に一致した文字は eggFrames の絵に差し替える (裏ルート)
 *  - インタールード: 流れる文字が一周するたびに絵を挟む (v20、playlist の "interlude")
 *  - playlist の保存: LittleFS に /playlist.json を置き、ネット無しの再起動でも戻す (v20)
 *  - 複数色 (v21): frames[].layers で1枚の絵を色ごとに塗り分ける。
 *    messages の中に {#RRGGBB} を書くと、そこから先の文字の色が変わる ({#} で元の色に戻す)
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
#include "esp_task_wdt.h"
#include <ArduinoJson.h>
#include <Update.h>
#include <Preferences.h>
#include <WebServer.h>
#include <LittleFS.h>
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

// 塗り分けの重ね絵 (v21、frames[].layers)。関数の引数に使うので、Arduino が関数の宣言を
// 自動で差し込む位置 (最初の関数の手前) より前に定義しておく。
#define MAX_FRAME_LAYERS 48          // 1組の合計 (48×256B = 12KB)
#define MAX_LAYERS_PER_FRAME 7       // 下地と合わせて1枚8色まで
struct FrameLayers {
  uint8_t (*data)[FRAME_BYTES] = nullptr;   // malloc。nullptr = 重ね絵なし
  uint16_t color[MAX_FRAME_LAYERS];
  uint8_t  start[MAX_FRAMES];
  uint8_t  count[MAX_FRAMES];               // 0 = 下地だけの絵
};

// ---- 自己アップデート (v0.7) ----
// このビルドのバージョン。リリースごとに +1 する (手順: docs/firmware-release.md)。
// **公開する .bin はこの値を上げてビルドしたものであること。** manifest の version だけ
// 上げて .bin が古いままだと、実機は「更新したのにまだ古い」を延々繰り返す。
#define FW_VERSION 21

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

// ---- 営業カレンダー (v16) ----
// 取得先は playlist.json の "hoursUrl" で指定する。config.h の値は初期値。
// 空文字なら機能ごとOFF (緊急停止弁。そのときは従来どおり playlist の topText をそのまま出す)。
#ifndef HOURS_URL
#define HOURS_URL ""
#endif

// commentsUrl と同じ考え方で、受け付けるURLの頭を絞る (公開ファイル経由で実機に任意の
// ホストを叩かせない)。カレンダーの置き場はこのリポジトリの GitHub Pages なので、
// COMMENTS_URL_PREFIX (Google Apps Script) とは別の頭になる。
#ifndef HOURS_URL_PREFIX
#define HOURS_URL_PREFIX "https://mugiwaraboushi.github.io/moshimo-sign/"
#endif

// カレンダー取得の周期。日付と曜日しか見ないので頻繁に取る必要はない。
#ifndef HOURS_INTERVAL_MS
#define HOURS_INTERVAL_MS (10UL * 60UL * 1000UL)   // 10分
#endif

// 実機側で保持する量の上限。RAMの都合であって仕様上の上限ではない。
// 現行の data/hours.json は1か月ぶん (15件程度) なので余裕がある。
#define HOURS_MAX_DATES  40   // dates の日数
#define HOURS_MAX_RANGES 3    // 1日あたりの時間帯の数 ("11:00-13:00,15:00-19:00" で2)

// 1日ぶんの営業時間。文字列のまま持つとヒープが断片化するので、時間帯は分単位の int に直す。
// count == 0 は「終日CLOSED」(null・空文字・曜日キーが無い場合も同じ扱い)。
// **型の定義がここにあるのは、Arduino が関数プロトタイプを最初の関数の直前に挿し込むため。**
// 状態の実体は下の「表示状態」の節にある。
struct HoursDay {
  int8_t  count;
  int16_t from[HOURS_MAX_RANGES];
  int16_t to[HOURS_MAX_RANGES];
};

// 本文の受け皿 (v20)。String に足していくか、呼び手が持つ char バッファに詰めるか。
// playlist は 17KB を超えることがあり、TLS の接続中に同じ大きさの String を確保すると
// ヒープが割れて取れなくなった (v20 の実機で確認)。そこで playlist だけ静的バッファで受ける。
// str があれば String、無ければ buf。cap は上限 (String なら BODY_MAX_BYTES、buf なら大きさ)。
// 使い方は「HTTP取得」の節。型がここにあるのは HoursDay と同じ理由。
struct BodySink {
  String *str = nullptr;
  char   *buf = nullptr;
  size_t  cap = 0;
  size_t  len = 0;
  size_t size() const { return str ? str->length() : len; }
  bool put(const uint8_t *p, size_t n) {
    if (size() + n > cap) return false;
    if (str) return str->concat((const char *)p, (unsigned int)n);   // false = メモリ不足
    memcpy(buf + len, p, n);
    len += n;
    return true;
  }
};

// playlist の受け渡しバッファ (plBuf) の持ち主 (v20)。説明は「playlist.json」の節。
enum PlBufState { PLBUF_FREE, PLBUF_WRITING, PLBUF_READY };

// 古い config.h でもビルドが通るように既定値を用意する。
#ifndef SELFUPDATE_MANIFEST_URL
#define SELFUPDATE_MANIFEST_URL "https://mugiwaraboushi.github.io/moshimo-sign/firmware/manifest.json"
#endif
#ifndef SELFUPDATE_INTERVAL_MS
#define SELFUPDATE_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)  // 6時間
#endif

// ---------------- パネル ----------------
MatrixPanel_I2S_DMA *display = nullptr;

// ---------------- 認証情報 (v17) ----------------
// WiFi/OTA の認証情報は NVS (Preferences, namespace "cfg") に置き、.bin には埋めない。
// 公開している .bin から SSID/パスワードが読み出せてしまう状態をやめるため。
// NVS に値が無いときだけ config.h の値へ戻る (v16以前と同じ挙動。移行のための後方互換)。
// 投入はUSBで物理接続したシリアルからのみ。値そのものは絶対にログへ出さない
// (出所と文字数だけを出す)。
static Preferences cfgPrefs;
static const char *CFG_NS = "cfg";
static const char *CFG_KEYS[] = {"ssid", "pass", "ssid2", "pass2", "ota"};
static const int CFG_KEY_COUNT = sizeof(CFG_KEYS) / sizeof(CFG_KEYS[0]);

// wifiMulti.addAP() / ArduinoOTA.setPassword() には c_str() を渡すので、一時オブジェクトでは
// なく寿命がプログラム全体の String に持たせる (解放済みの領域を渡して接続に失敗するのを防ぐ)。
static String cfgSsid, cfgPass, cfgSsid2, cfgPass2, cfgOta;

static bool cfgIsKey(const String &k) {
  for (int i = 0; i < CFG_KEY_COUNT; i++) {
    if (k == CFG_KEYS[i]) return true;
  }
  return false;
}

// NVSから1つ読む。namespaceが未作成でもキーが無くても空文字を返す。
static String cfgReadNvs(const char *key) {
  String v;
  if (cfgPrefs.begin(CFG_NS, true)) {   // 読み取り専用。未作成なら false
    v = cfgPrefs.getString(key, "");
    cfgPrefs.end();
  }
  return v;
}

static void cfgLoad() {
  // WiFiの4つはまとめて切り替える。ssidはNVS・passはconfig.hのような混ざり方をすると
  // 繋がらないため、出所は「4つともNVS」か「4つともconfig.h」の二択にする。
  String s = cfgReadNvs("ssid");
  if (s.length()) {
    cfgSsid  = s;
    cfgPass  = cfgReadNvs("pass");
    cfgSsid2 = cfgReadNvs("ssid2");
    cfgPass2 = cfgReadNvs("pass2");
    Serial.println("[cfg] wifi: NVS");
  } else {
    cfgSsid  = WIFI_SSID;
    cfgPass  = WIFI_PASS;
    cfgSsid2 = WIFI_SSID2;
    cfgPass2 = WIFI_PASS2;
    Serial.println("[cfg] wifi: config.h");
  }

  String o = cfgReadNvs("ota");
  if (o.length()) {
    cfgOta = o;
    Serial.println("[cfg] ota: NVS");
  } else {
    cfgOta = OTA_PASSWORD;
    Serial.println("[cfg] ota: config.h");
  }
}

// シリアル (115200) からの投入。USBで物理接続した者しか使えない。
// ネットワーク経由の設定変更は用意しない (playlist等から触れる経路は作らない)。
//   cfg set <key> <value>   key は CFG_KEYS の5つのみ。value は行末まで (空白可)
//   cfg show                設定の有無と文字数だけを返す (値は返さない)
//   cfg clear               5キーを全部消す
//   cfg reboot              再起動して投入した値で繋ぎ直す
// 上記以外の行は黙って捨てる (ログも出さない)。
// loop() から毎回呼ぶが、届いている分を読むだけでブロックしない。
static void cfgSerialPoll() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (line.length() < 192) line += c;   // 長すぎる行は頭だけ残す (コマンドとして落ちる)
      continue;
    }
    String cmd = line;
    line = "";
    if (!cmd.startsWith("cfg ")) continue;
    String rest = cmd.substring(4);

    if (rest.startsWith("set ")) {
      String kv = rest.substring(4);
      int sp = kv.indexOf(' ');
      if (sp <= 0) continue;
      String key = kv.substring(0, sp);
      String val = kv.substring(sp + 1);    // 行末まで = 空白を含むパスワードも通る
      if (!cfgIsKey(key)) continue;
      if (!cfgPrefs.begin(CFG_NS, false)) continue;
      cfgPrefs.putString(key.c_str(), val);
      cfgPrefs.end();
      Serial.printf("[cfg] saved %s (%d文字)\n", key.c_str(), (int)val.length());
    } else if (rest == "show") {
      for (int i = 0; i < CFG_KEY_COUNT; i++) {
        String v = cfgReadNvs(CFG_KEYS[i]);
        if (v.length()) Serial.printf("%s: set (%d)\n", CFG_KEYS[i], (int)v.length());
        else            Serial.printf("%s: unset\n", CFG_KEYS[i]);
      }
    } else if (rest == "clear") {
      if (!cfgPrefs.begin(CFG_NS, false)) continue;
      for (int i = 0; i < CFG_KEY_COUNT; i++) cfgPrefs.remove(CFG_KEYS[i]);
      cfgPrefs.end();
      Serial.println("[cfg] cleared");
    } else if (rest == "reboot") {
      Serial.println("[cfg] reboot");
      Serial.flush();
      ESP.restart();
    }
  }
}

// ---------------- WiFi ----------------
// 登録したAPのうち電波の届く方に自動接続する
static WiFiMulti wifiMulti;

static void wifiSetup() {
  WiFi.mode(WIFI_STA);
  // モデムスリープを切る (v0.12)。既定では受信の合間に電波部を落として省電力にするが、
  // この掲示板は常時給電なので節電の必要がない。切っておくと OTA の取りこぼしが減り、
  // playlist/コメント取得の初回応答も安定する (スリープ復帰待ちが挟まらなくなる)。
  WiFi.setSleep(false);
  wifiMulti.addAP(cfgSsid.c_str(), cfgPass.c_str());
  if (cfgSsid2.length()) wifiMulti.addAP(cfgSsid2.c_str(), cfgPass2.c_str());
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

// 文字の拡大率と太字 (v18)。scale=2 で16pxフォントを縦横2倍 (32px高) に引き伸ばす。
// bold は点灯ドットを右へ1dot太らせる (横線はそのまま・縦線が1dot太くなる)。
// 太らせたぶん隣の文字に食い込まないよう、1文字ごとの送り幅も +1 する。
// 既定 (scale=1, bold=false) の描画結果は v17 以前と1ドットも変わらない。

// 1文字の送り幅 (拡大・太字込み)
static int glyphAdvance(uint16_t cp, int scale, bool bold) {
  int gi = glyphIndex(cp);
  int w = (gi < 0) ? 16 : pgm_read_byte(&FONT16_DATA[gi * 33]);
  return w * scale + (bold ? 1 : 0);
}

// テキストのピクセル幅
static int textWidth(const uint16_t *cps, int n, int scale = 1, bool bold = false) {
  int w = 0;
  for (int i = 0; i < n; i++) w += glyphAdvance(cps[i], scale, bold);
  return w;
}

// 1文字描画 (x,y=左上)。クリッピングあり。戻り値=送り幅
static int drawGlyph(uint16_t cp, int x, int y, uint16_t color, int scale = 1, bool bold = false) {
  int gi = glyphIndex(cp);
  int w = 16;
  if (gi >= 0) w = pgm_read_byte(&FONT16_DATA[gi * 33]);
  const int adv = w * scale + (bold ? 1 : 0);
  if (x + adv < 0 || x >= PANEL_W) return adv;
  // 1ドットを scale×scale の四角で描く。太字なら横幅だけ +1 (右隣と重なってよい)
  const int dotW = scale + (bold ? 1 : 0);
  for (int r = 0; r < 16; r++) {
    uint16_t bits;
    if (gi >= 0) {
      bits = ((uint16_t)pgm_read_byte(&FONT16_DATA[gi * 33 + 1 + r * 2]) << 8)
           |  pgm_read_byte(&FONT16_DATA[gi * 33 + 2 + r * 2]);
    } else {
      bits = (r == 1 || r == 14) ? 0x7FFE : (r > 1 && r < 14) ? 0x4002 : 0; // 豆腐
    }
    if (!bits) continue;
    for (int sy = 0; sy < scale; sy++) {
      int py = y + r * scale + sy;
      if (py < 0 || py >= PANEL_H) continue;
      for (int i = 0; i < w; i++) {
        if (!((bits >> (15 - i)) & 1)) continue;
        int px0 = x + i * scale;
        for (int sx = 0; sx < dotW; sx++) {
          int px = px0 + sx;
          if (px >= 0 && px < PANEL_W) display->drawPixel(px, py, color);
        }
      }
    }
  }
  return adv;
}

static void drawText(const uint16_t *cps, int n, int x, int y, uint16_t color,
                     int scale = 1, bool bold = false) {
  for (int i = 0; i < n; i++) x += drawGlyph(cps[i], x, y, color, scale, bold);
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

// 塗り分けた絵 (v21)。下地と重ね絵を1パスで合成して描く。
// 重ね絵ごとに描き足すと、その間をDMAが走査したとき下地の色が一瞬見えるため、
// ドットごとに「いちばん上で点灯している層の色」を決めてから1回だけ描く。
static void drawBitmapLayered(const uint8_t *base, uint16_t baseColor,
                              const uint8_t (*layers)[FRAME_BYTES], const uint16_t *layerColors,
                              int layerCount, int w, int h, int x, int y) {
  for (int r = 0; r < h; r++) {
    int py = y + r;
    if (py < 0 || py >= PANEL_H) continue;
    for (int c = 0; c < w; c++) {
      int px = x + c;
      if (px < 0 || px >= PANEL_W) continue;
      int i = r * w + c;
      const uint8_t mask = 0x80 >> (i & 7);
      uint16_t col = (base[i >> 3] & mask) ? baseColor : 0;
      for (int L = layerCount - 1; L >= 0; L--) {
        if (layers[L][i >> 3] & mask) { col = layerColors[L]; break; }
      }
      display->drawPixel(px, py, col);
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
// 1文字ごとの色 (v21)。messages の {#RRGGBB} で決まる。
// RGB565 は 0 (黒) もありうるので、「指定あり」は別のビット列で持つ (int32 で -1 を空きにすると 4KB、これで 2KB)。
// ビットが立っていない文字は既定の色 (colorScroll / 虹色) のまま。
static uint16_t marqueeCol[1024];
static uint8_t  marqueeColSet[1024 / 8];
static inline void marqueeSetCol(int i, int32_t col) {
  const uint8_t bit = 1 << (i & 7);
  if (col >= 0) { marqueeCol[i] = (uint16_t)col; marqueeColSet[i >> 3] |= bit; }
  else          marqueeColSet[i >> 3] &= ~bit;
}
static int marqueeLen = 0;
static int marqueeW = 0;
static float scrollX = PANEL_W;
// marqueeW をどの拡大率・太字で測ったかの控え (v18)。-1 = 測り直しが必要
static int  marqueeWScale = -1;
static bool marqueeWBold = false;
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
static int  plScrollScale = 1;   // scrollScale: 1=16px / 2=32px。2 は mode:"scroll" のときだけ効く (v18)
static bool plScrollBold = false; // scrollBold: スクロール文字を太字にする (v18)
// イベント中の表示設定 (v19)。commentsUrl の JSON 応答の "settings" から受け取る。
// plMode == MODE_EVENT のときだけ、指定のあるキーが上の playlist 由来の値を上書きする
// (イベント中の優先順位: GAS settings > playlist.json)。has* = false は「指定なし」。
// pl* 自体は書き換えないので、mode が event 以外になれば何もしなくても playlist の値に戻る。
struct EventSettings {
  bool hasColor = false;  bool rainbow = false;  uint16_t color = 0;
  String colorStr;                         // ログ用 (受け取った文字列そのまま)
  bool hasSpeed = false;  float speed = 0;
  bool hasScale = false;  int scale = 1;
  bool hasBold = false;   bool bold = false;
  bool hasLines = false;  int lines = 2;
  String flow;                             // eventFlow: 保持だけ (v20 で使う)
};
static EventSettings evSet;
static String plCommentsUrl = COMMENTS_URL;   // commentsUrl で差し替え可能 (v0.10)
static String plHoursUrl = HOURS_URL;         // hoursUrl で差し替え可能 (v16)
static volatile bool hoursUrlChanged = false; // 取得先が変わった → 次の取得を待たずに行う
// plCommentsUrl / plHoursUrl と受け渡し用バッファは取得タスクと共有する。
// 詳細は「ネットワーク取得タスク」の節。
static SemaphoreHandle_t fetchMutex = nullptr;

// ---- 営業カレンダー (v16) ----
// data/hours.json を実機が直接読んで OPEN / CLOSED を自分で判定する。
// 判定ルールは scripts/update-open-closed.mjs と同じ (docs/playlist-spec.md 参照)。
static HoursDay hoursWeekly[7];                     // 0=日曜 … 6=土曜 (tm_wday と同じ並び)
static char     hoursDateKey[HOURS_MAX_DATES][11];  // "YYYY-MM-DD"
static HoursDay hoursDates[HOURS_MAX_DATES];
static int      hoursDateCount = 0;
static String   hoursOpenText   = "OPEN";
static String   hoursClosedText = "CLOSED";
static bool     hoursValid = false;   // 一度でもカレンダーを読めたか (false = 判定しない)

// 判定結果で置き換える上段の文言。空 = 置き換えない (plTopText をそのまま出す)。
static String hoursTopText;
// 直前の判定 (-1=未判定 / 0=CLOSED / 1=OPEN / 2=手動文言)。ログを1回だけ出すための控え。
static int hoursLastState = -1;

// mode: "dual" / "scroll" / "frames" (v0.5) / "event" (v15)
// MODE_EVENT の見た目は MODE_DUAL と同じ (上段topText + 下段スクロール)。
// ただし GAS の settings で eventLines:1 が来ていれば MODE_SCROLL と同じ1段になる (v19)。
// 違うのは流す中身だけで、コメントがあるときは plMessages を出さずコメントだけを流す
// (rebuildMarquee を見ること)。
enum DisplayMode { MODE_DUAL, MODE_SCROLL, MODE_FRAMES, MODE_EVENT };
static DisplayMode plMode = MODE_DUAL;

// frames: ドット絵の静止画 (v0.5)。hold秒ごとに次の絵へ切り替えて先頭に戻る
static uint8_t plFrameData[MAX_FRAMES][FRAME_BYTES];
static uint16_t plFrameColor[MAX_FRAMES];
static uint16_t plFrameHoldMs[MAX_FRAMES];  // ミリ秒。hold(秒)もここへ1000倍して入れる
static int plFrameCount = 0;
// 塗り分け (v21): frames[].layers の重ね絵。d/color が下地、layers が上に重ねる色ごとの絵
// (点灯ドットだけを上書き)。layers を知らない v20 以前の実機は下地だけを出す (1色の絵として見える)。
// 重ね絵の置き場は絵の組 (frames / eggFrames / interlude) ごとに、layers の総数ぶんだけ確保する
// (FrameLayers。定義はファイル先頭)。静的に持つと常駐が 12KB 増えて TLS の確保ができなくなるため。
static FrameLayers plFrameLayers;
static int frameIdx = 0;
static unsigned long frameSince = 0;      // 0 = 未開始 (次の描画で現在時刻を入れる)
static int drawnFrame = -1;               // 今パネルに描いてある絵 (-1 = 要再描画)

// ---- ブース用キーボード入力 (say) と裏ルート (egg) (v20) ----
// 実機が LAN 内に小さな入力ページを持ち、打った文字をその場で出す。GAS を経由すると
// 15秒遅れるうえ、ブースのネットは不安定なので、インターネットを通さずに直接受け取る。
// sayEnabled が true のあいだだけ入力口 (WebServer) を開く。仕組みは「ブース入力」の節。
#define SAY_QUEUE_MAX 5
#define SAY_MAX_CHARS 40
#define EGG_MAX_WORDS 8
#define EGG_WORD_MAX_CHARS 16
static bool plSayEnabled = false;
static int  plSayHoldMs = 5000;           // 中央に静止で出す長さ (1000〜20000)
static String plEggWords[EGG_MAX_WORDS];  // 照合用に正規化済み (前後の空白を除き、英字は小文字)
static int  plEggWordCount = 0;
// 絵の置き場は枚数が 1 以上のときだけ確保する (最大 6KB)。v20 の実機で、静的に持つと
// 常駐が増えて TLS の確保ができなくなったため。nullptr = 0 枚。
static uint8_t (*plEggData)[FRAME_BYTES] = nullptr;
static uint16_t plEggColor[MAX_FRAMES];
static uint16_t plEggHoldMs[MAX_FRAMES];
static FrameLayers plEggLayers;
static int  plEggFrameCount = 0;
static int  plEggMs = 4000;               // egg の再生時間 (1000〜20000)

// ---- インタールード (v20) ----
// 流れる文字が左端まで流れ切った回数が every の倍数になったら、絵を1回だけ挟む。
static uint8_t (*plIlData)[FRAME_BYTES] = nullptr;   // eggFrames と同じく必要なときだけ確保
static uint16_t plIlColor[MAX_FRAMES];
static uint16_t plIlHoldMs[MAX_FRAMES];
static FrameLayers plIlLayers;
static int  plIlFrameCount = 0;
static int  plIlEvery = 1;                // 1〜10

// 絵の組の idx 枚目を中央に描く (v21)。重ね絵が無ければ従来の1色描き。
static void drawFrame(const uint8_t *base, uint16_t baseColor, const FrameLayers &L, int idx) {
  const int x = (PANEL_W - FRAME_W) / 2, y = (PANEL_H - FRAME_H) / 2;
  if (!L.data || L.count[idx] == 0)
    drawBitmapOpaque(base, FRAME_W, FRAME_H, x, y, baseColor);
  else
    drawBitmapLayered(base, baseColor, L.data + L.start[idx], L.color + L.start[idx],
                      L.count[idx], FRAME_W, FRAME_H, x, y);
}

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

// 1文字ごとの色つきで描く (v21)。色の指定が無い文字は従来どおり
// color (または虹色) で描くので、{#…} を使っていない本文の見た目は v20 と同じ。
// cols / colSet が nullptr なら全文字が既定の色 (ブース入力の文字など)。
// 虹色は1文字ごとに色相を24°ずつ進め、基準色相は millis()/20 で回る (約7.2秒で一周。v0.4 から同じ)。
static void drawTextColored(const uint16_t *cps, const uint16_t *cols, const uint8_t *colSet,
                            int n, int x, int y,
                            uint16_t color, bool rainbow, int scale = 1, bool bold = false) {
  int hueBase = (int)((millis() / 20) % 360);
  for (int i = 0; i < n; i++) {
    const bool set = colSet && (colSet[i >> 3] & (1 << (i & 7)));
    uint16_t col = set ? cols[i]
                 : rainbow ? hsvToColor565(hueBase + i * 24) : color;
    x += drawGlyph(cps[i], x, y, col, scale, bold);
  }
}

// 本文の一片を marquee に足す (v21)。tags=true のときだけ {#RRGGBB} / {#} を色の指定として読む。
// 色の指定は一片ごとに既定色から始まる (前のメッセージの色を次に持ち越さない)。
// コメント (参加者の投稿) は tags=false で渡すので、書かれた {#…} はそのまま文字として出る。
static void marqueeAppend(const String &s, bool tags) {
  int32_t cur = -1;
  int from = 0;
  const int len = s.length();
  while (from < len && marqueeLen < 1024) {
    int at = tags ? s.indexOf("{#", from) : -1;
    int end = (at < 0) ? len : at;
    if (end > from) {
      int n = decodeUtf8(s.substring(from, end), marqueeCps + marqueeLen, 1024 - marqueeLen);
      for (int i = 0; i < n; i++) marqueeSetCol(marqueeLen + i, cur);
      marqueeLen += n;
    }
    if (at < 0) break;
    int close = s.indexOf('}', at);
    String hex = (close > at) ? s.substring(at + 2, close) : String("x");
    bool isHex = hex.length() == 6;
    for (int i = 0; isHex && i < 6; i++) isHex = isxdigit((unsigned char)hex[i]);
    if (hex.length() == 0) { cur = -1; from = close + 1; }
    else if (isHex) { cur = hexToColor565(hex.c_str()); from = close + 1; }
    else {
      // 色の指定になっていない "{#" はそのまま文字として出す
      if (marqueeLen < 1024) { marqueeCps[marqueeLen] = '{'; marqueeSetCol(marqueeLen++, cur); }
      from = at + 1;
    }
  }
}

// いま流している本文。中身が変わったときだけ組み直すための控え (v0.10)
static String marqueeText;

static void rebuildMarquee() {
  String t;
  if (plMode == MODE_EVENT && commentCount > 0) {
    // イベント中 (v15): コメントだけを流す。通常メッセージは混ぜない。
    for (int i = 0; i < commentCount; i++) {
      if (t.length()) t += "　◆　";
      t += comments[i];
    }
  } else if (plMode == MODE_EVENT) {
    // イベント中でもコメントがまだ0件のときは通常メッセージを流す (パネルを空にしない)
    for (int i = 0; i < plMsgCount; i++) {
      if (t.length()) t += "　◆　";
      t += plMessages[i];
    }
  } else {
    // playlistのメッセージ + コメントを ◆ で連結して流す
    for (int i = 0; i < plMsgCount; i++) {
      if (t.length()) t += "　◆　";
      t += plMessages[i];
    }
    for (int i = 0; i < commentCount; i++) {
      if (t.length()) t += "　◆　";
      t += comments[i];
    }
  }
  if (!t.length()) t = DEFAULT_MESSAGE;
  // 中身が同じなら何もしない (v0.10)。組み直すと scrollX が右端に戻るため、
  // 60秒ごとのplaylist取得・15秒ごとのコメント取得のたびに流れが頭から再開し、
  // 一周が取得間隔より長い文面は末尾が永久に出てこない。
  if (t == marqueeText) return;
  marqueeText = t;
  // 上と同じ並びで、一片ずつ色の指定を読みながら組む (v21)。
  // 色の指定 {#…} を読むのは playlist の messages だけ。コメントは文字のまま
  // (区切りの入れ方も上と同じ: それまでに1文字でもあれば ◆ を挟む)
  marqueeLen = 0;
  marqueeAppend("　", false);
  const bool evComments = (plMode == MODE_EVENT && commentCount > 0);
  const bool withMsgs = !evComments;
  const bool withComments = (plMode != MODE_EVENT) || evComments;
  bool any = false;
  if (withMsgs) for (int i = 0; i < plMsgCount; i++) {
    if (any) marqueeAppend("　◆　", false);
    marqueeAppend(plMessages[i], true);
    any = any || plMessages[i].length();
  }
  if (withComments) for (int i = 0; i < commentCount; i++) {
    if (any) marqueeAppend("　◆　", false);
    marqueeAppend(comments[i], false);
    any = any || comments[i].length();
  }
  if (!any) marqueeAppend(DEFAULT_MESSAGE, false);
  marqueeWScale = -1;   // 幅は loop() 側で今の拡大率・太字に合わせて測る (v18)
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

// ---- 本文読み出し (v15) ----
// Arduinoコアの HTTPClient::getString() は writeToStreamDataBlock() を通るが、
// この関数は **全体の締め切りを持たない** (HTTPClient.cpp:1318 の
// while (connected() && (len > 0 || len == -1)))。さらに Content-Length が
// 分かっているときは available() を見ずに毎周期 delay(0) しか挟まない
// (同 1321 / 1393。無通信時の delay(1) は len < 0 のときしか通らない)。
// delay(0) は vTaskDelay(0) で、同優先度のタスクには譲らない。
// fetchタスクは tskIDLE_PRIORITY の core0 固定なので、TLSの本文が途中で止まると
// IDLE0 が回らず、60秒でタスクWDTが発火して再起動していた (v15で実測)。
// そこで getString() を使わず、締め切り付き + 毎周期 vTaskDelay(1) で自前に読む。
static const unsigned long BODY_TOTAL_TIMEOUT_MS = 20000;   // 本文全体の締め切り
static const unsigned long BODY_STALL_TIMEOUT_MS = 5000;    // 最後に受信してからの無通信
static const size_t        BODY_MAX_BYTES        = 48 * 1024;

// 締め切りを過ぎたか。millis()の巻き返しに耐えるよう符号付きで比べる。
static inline bool bodyExpired(unsigned long tEnd, unsigned long lastData) {
  return (long)(millis() - tEnd) >= 0
      || (long)(millis() - (lastData + BODY_STALL_TIMEOUT_MS)) >= 0;
}

// need バイトを dst に読む。need < 0 = 接続が閉じるまで読む。
// データが無い周回では必ず vTaskDelay(1) して IDLE を回す (yield/delay(0)では足りない)。
static bool bodyReadN(NetworkClient *s, int need, BodySink &dst,
                      unsigned long tEnd, unsigned long &lastData) {
  uint8_t buf[512];
  for (;;) {
    if (need == 0) return true;
    int avail = s->available();
    if (avail > 0) {
      int want = (int)sizeof(buf);
      if (need > 0 && need < want) want = need;
      if (avail < want) want = avail;
      int r = s->read(buf, want);
      if (r > 0) {
        if (!dst.put(buf, (size_t)r)) return false;   // 上限超え / メモリ不足
        lastData = millis();
        if (need > 0) need -= r;
        continue;
      }
    } else if (!s->connected()) {
      return need < 0;   // 閉じるまで読む指定なら、閉じたところで正常終了
    }
    if (bodyExpired(tEnd, lastData)) return false;
    vTaskDelay(1);
  }
}

// 1行読む (CR/LFは含めずに返す)。チャンクのサイズ行と本体直後のCRLFに使う。
static bool bodyReadLine(NetworkClient *s, String &line,
                         unsigned long tEnd, unsigned long &lastData) {
  line = "";
  for (;;) {
    if (s->available() > 0) {
      int c = s->read();
      if (c >= 0) {
        lastData = millis();
        if (c == '\n') return true;
        if (c != '\r' && line.length() < 32) line += (char)c;   // 32文字で頭打ち
        continue;
      }
    } else if (!s->connected()) {
      return false;
    }
    if (bodyExpired(tEnd, lastData)) return false;
    vTaskDelay(1);
  }
}

// http.getString() の代わり。成功なら true。
// 締め切り超過 / Content-Length 未達 / チャンク不正 / 受け皿の上限超え は false。
// out は空の状態で渡すこと (呼び手が用意する)。
static bool readBodyWithDeadline(NetworkClient *s, int total, bool chunked, BodySink &out) {
  if (!s) return false;
  const unsigned long tEnd = millis() + BODY_TOTAL_TIMEOUT_MS;
  unsigned long lastData = millis();

  if (chunked) {
    // 最小限のチャンク復号: 16進のサイズ行 → 本体 → CRLF。サイズ0で終わり。
    for (;;) {
      String sz;
      if (!bodyReadLine(s, sz, tEnd, lastData)) return false;
      if (!sz.length()) continue;                    // 余分な空行は読み飛ばす
      int semi = sz.indexOf(';');                    // チャンク拡張は捨てる
      if (semi >= 0) sz = sz.substring(0, semi);
      char *endp = nullptr;
      long n = strtol(sz.c_str(), &endp, 16);
      if (endp == sz.c_str() || n < 0) return false;             // 16進として読めない
      if (n == 0) return true;                                   // 最終チャンク
      if (out.size() + (unsigned long)n > out.cap) return false;
      if (!bodyReadN(s, (int)n, out, tEnd, lastData)) return false;
      String crlf;
      if (!bodyReadLine(s, crlf, tEnd, lastData)) return false;
      if (crlf.length()) return false;               // 本体の直後がCRLFでない = 不正
    }
  }
  if (total == 0) return true;   // Content-Length: 0 は空本文で成功 (getString と同じ)
  if (total > 0) {
    if ((unsigned long)total > out.cap) return false;
    if (out.str) out.str->reserve(total);
    return bodyReadN(s, total, out, tEnd, lastData);
  }
  return bodyReadN(s, -1, out, tEnd, lastData);   // 長さ不明 = 閉じるまで読む
}

// 200のときの本文取得。失敗時は受け皿を空にして false (呼び手が code = -2 にする)。
static bool readBody(HTTPClient &http, BodySink &out) {
  NetworkClient *s = http.getStreamPtr();
  int total = http.getSize();                        // 不明 / chunked なら -1
  bool chunked = http.header("Transfer-Encoding").indexOf("chunked") >= 0;
  if (readBodyWithDeadline(s, total, chunked, out)) return true;
  Serial.printf("[http] body timeout/incomplete (%d/%d bytes)\n", (int)out.size(), total);
  if (out.str) *out.str = "";
  out.len = 0;
  return false;
}

// 空きヒープ (v20)。合計と、一度に確保できる最大の塊。TLS は後者が足りないと張れない。
static void logMem() {
  Serial.printf("[mem] free=%u largest=%u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

// timeoutMs は**ヘッダが返ってくるまでの待ち時間** (HTTPClient の _tcpTimeout)。
// 本文の締め切りは readBodyWithDeadline 側が別に持っている (v15)。
// 接続の手順は httpGetString (String で受ける) と httpGetToBuf (char バッファで受ける) で共通 (v20)。
static void httpGetInto(const String &url, int &code, bool wait, unsigned long timeoutMs, BodySink &out) {
  logMem();   // 計測 (v20)
  HTTPClient http;
  code = -1;
  if (netMutex && xSemaphoreTake(netMutex, wait ? portMAX_DELAY : 0) != pdTRUE) {
    code = -2;   // 別の取得が進行中
    return;
  }
  http.setTimeout(timeoutMs);
  // Transfer-Encoding は自前の本文読み出し (readBody) がチャンクかどうかを見るのに使う。
  const char *hdrs[] = {"Transfer-Encoding"};
  http.collectHeaders(hdrs, 1);
  // リダイレクト追従 (v0.10)。GASの /exec は script.googleusercontent.com への302を返すので、
  // 追わないと本文が取れない。302(FOUND)はSTRICTでも追われるのでこれで足りる。
  // リダイレクト先が別ホストでも、プロトコルが同じ(https→https)なら HTTPClient::setURL が通す。
  if (url.startsWith("https")) {
    NetworkClientSecure client;
    client.setInsecure();  // GitHub Pages等の証明書検証を省略 (表示内容のみなので許容)
    client.setHandshakeTimeout(10);
    client.setTimeout(5);   // 本文読み出しが止まったまま吊られないよう5秒で諦める (v14)
    if (http.begin(client, url)) {
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      code = http.GET();
      if (code == 200 && !readBody(http, out)) code = -2;   // 本文が読み切れなかった
      http.end();
    }
  } else {
    if (http.begin(url)) {
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      code = http.GET();
      if (code == 200 && !readBody(http, out)) code = -2;   // 本文が読み切れなかった
      http.end();
    }
  }
  if (netMutex) xSemaphoreGive(netMutex);
  if (code != 200) logMem();   // 計測 (v20)
}

static String httpGetString(const String &url, int &code, bool wait = false,
                            unsigned long timeoutMs = 5000) {
  String out;
  BodySink sink;
  sink.str = &out;
  sink.cap = BODY_MAX_BYTES;
  httpGetInto(url, code, wait, timeoutMs, sink);
  return out;
}

// 本文を buf に読む (v20、playlist 用)。戻り値は本文のバイト数 (code != 200 なら 0)。
// buf には末尾に '\0' を付けるので、本文は cap - 1 バイトまで。超えたら失敗 (code = -2)。
static size_t httpGetToBuf(const String &url, char *buf, size_t cap, int &code, bool wait = false,
                           unsigned long timeoutMs = 5000) {
  BodySink sink;
  sink.buf = buf;
  sink.cap = cap - 1;
  httpGetInto(url, code, wait, timeoutMs, sink);
  if (code != 200) sink.len = 0;
  buf[sink.len] = '\0';
  return sink.len;
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

// playlist の本文は String ではなく、この静的バッファで受ける (v20)。
// TLS の接続中に本文ぶんの String を確保しないため (ヒープが割れて取れなくなるのを防ぐ)。
// 取得タスクと loop() の受け渡しはこのバッファそのもので行い、コピーしない。
// 持ち主は plBufState で決める (書き換えは fetchMutex の中で行う):
//   FREE    … 誰も使っていない。取得タスクだけが WRITING にできる
//   WRITING … 取得タスクが書いている。loop() は読まない
//   READY   … 新しい本文がある (plBufLen バイト)。loop() が反映し終えたら FREE に戻す
// playlist の上限はこの大きさ - 1 になる (コメント等の String 経路は BODY_MAX_BYTES のまま)。
static char plBuf[24 * 1024];
static size_t plBufLen = 0;
static volatile PlBufState plBufState = PLBUF_FREE;

static void plBufSetState(PlBufState st) {
  if (fetchMutex) xSemaphoreTake(fetchMutex, portMAX_DELAY);
  plBufState = st;
  if (fetchMutex) xSemaphoreGive(fetchMutex);
}

// HTTPだけ。取れたら true を返し、plBuf に本文 (len バイト) が入って READY になる。
// false = 今回は取れなかった (呼び手が間隔を詰めて再挑戦する)
static bool fetchPlaylistBody(size_t &len, bool wait) {
  if (strlen(PLAYLIST_URL) == 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  // loop() がまだ前の本文を反映していなければ書かない (すぐ再挑戦してよい)
  if (fetchMutex) xSemaphoreTake(fetchMutex, portMAX_DELAY);
  const bool mine = (plBufState == PLBUF_FREE);
  if (mine) plBufState = PLBUF_WRITING;
  if (fetchMutex) xSemaphoreGive(fetchMutex);
  if (!mine) return false;
  // CDNキャッシュ回避のためクエリを付与
  String url = String(PLAYLIST_URL) + "?t=" + String(millis());
  int code;
  size_t n = httpGetToBuf(url, plBuf, sizeof(plBuf), code, wait);
  if (code != 200 || n == 0) {
    plBufSetState(PLBUF_FREE);
    Serial.printf("[playlist] fetch failed (%d)\n", code);
    return false;   // -2 = 他の取得と重なった。すぐ再挑戦してよい (v0.10)
  }
  if (fetchMutex) xSemaphoreTake(fetchMutex, portMAX_DELAY);
  plBufLen = n;
  plBufState = PLBUF_READY;
  if (fetchMutex) xSemaphoreGive(fetchMutex);
  len = n;
  return true;
}

// 直前に反映した本文の指紋 (FNV-1a 32bit + 長さ) と、そこに書かれていた fwPing (v0.12)。
// v19 までは本文まるごと持って一致比較していたが、v20 で playlist が 17KB を超え、
// 常駐ぶんと合わせて TLS の確保ができなくなった (実機で確認)。長さも合わせて見るので、
// 取りこぼし (変わったのに同じと判定される) は 32bit の衝突が起きたときだけになる。
static bool     lastPlValid = false;
static uint32_t lastPlHash = 0;
static size_t   lastPlLen = 0;
static int      lastFwPing = 0;

static uint32_t fnv1a32(const char *p, size_t n) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < n; i++) {
    h ^= (uint8_t)p[i];
    h *= 16777619UL;
  }
  return h;
}

// 重ね絵の置き場を放す (v21)。count も 0 に戻すので、以後は下地だけの絵として描かれる。
static void freeLayers(FrameLayers &L) {
  free(L.data);
  L.data = nullptr;
  memset(L.count, 0, sizeof(L.count));
}

// frames / eggFrames / interlude.frames は同じ形式 (v20 で読み出しを共通化)。
// 壊れたフレームはそれだけ捨て、残りの枚数を返す。color が無いコマは colorTop の色。
// layers (v21): 色ごとの重ね絵。壊れた層はそれだけ捨てる。置き場が尽きたら残りは下地だけ。
// 置き場は layers の総数ぶんだけ確保し、0 なら持たない。確保できなければ layers を捨てて下地だけ出す。
static int parseFrames(JsonArray arr, uint8_t (*data)[FRAME_BYTES],
                       uint16_t *color, uint16_t *holdMs, FrameLayers &layers) {
  freeLayers(layers);   // 前の置き場は先に放す (確保の山を低くする)
  size_t want = 0;
  for (JsonVariant v : arr) {
    if (!v["layers"].is<JsonArray>()) continue;
    size_t k = v["layers"].as<JsonArray>().size();
    want += (k > MAX_LAYERS_PER_FRAME) ? MAX_LAYERS_PER_FRAME : k;
  }
  if (want > MAX_FRAME_LAYERS) want = MAX_FRAME_LAYERS;
  if (want > 0) {
    layers.data = (uint8_t (*)[FRAME_BYTES])malloc(want * FRAME_BYTES);
    if (!layers.data) Serial.println("[playlist] layers skipped (no memory)");
  }
  const int cap = layers.data ? (int)want : 0;
  int layerUsed = 0;
  int n = 0;
  for (JsonVariant v : arr) {
    if (n >= MAX_FRAMES) break;
    if (!v["d"].is<const char*>()) continue;
    if (b64urlDecode(v["d"], data[n], FRAME_BYTES) != FRAME_BYTES) {
      Serial.println("[playlist] frame skipped (data size mismatch)");
      continue;
    }
    color[n] = v["color"].is<const char*>() ? hexToColor565(v["color"]) : plColorTop;
    layers.start[n] = layerUsed;
    layers.count[n] = 0;
    if (layers.data && v["layers"].is<JsonArray>()) {
      for (JsonVariant L : v["layers"].as<JsonArray>()) {
        if (layers.count[n] >= MAX_LAYERS_PER_FRAME || layerUsed >= cap) {
          Serial.println("[playlist] layer skipped (too many layers)");
          break;
        }
        if (!L["d"].is<const char*>() ||
            b64urlDecode(L["d"], layers.data[layerUsed], FRAME_BYTES) != FRAME_BYTES) {
          Serial.println("[playlist] layer skipped (data size mismatch)");
          continue;
        }
        layers.color[layerUsed] = L["color"].is<const char*>() ? hexToColor565(L["color"]) : color[n];
        layerUsed++;
        layers.count[n]++;
      }
    }
    // holdMs (ミリ秒) が指定されていればそちらを優先し、無ければ hold (秒) を使う。
    // 下限20msは1コマ50fps相当。これより短い指定はパネルの走査が追いつかない。
    if (v["holdMs"].is<float>())
      holdMs[n] = constrain((int)(float)v["holdMs"], 20, 60000);
    else if (v["hold"].is<float>())
      holdMs[n] = constrain((int)(float)v["hold"], 1, 60) * 1000;
    else
      holdMs[n] = 8000;
    n++;
  }
  // 壊れた層を捨てたぶん、置き場を実際の数に詰める (0 なら放す)
  if (layerUsed == 0) freeLayers(layers);
  else if (layerUsed < cap) {
    void *p = realloc(layers.data, layerUsed * FRAME_BYTES);
    if (p) layers.data = (uint8_t (*)[FRAME_BYTES])p;
  }
  return n;
}

// 絵の置き場を arr の枚数ぶんだけ確保し直して読む (v20、eggFrames / interlude 用)。
// 前の置き場は先に放す (確保の山を低くする)。0 枚なら何も持たない。
static int parseFramesAlloc(JsonVariant arr, uint8_t (*&data)[FRAME_BYTES],
                            uint16_t *color, uint16_t *holdMs, FrameLayers &layers,
                            const char *name) {
  free(data);
  data = nullptr;
  freeLayers(layers);
  if (!arr.is<JsonArray>()) return 0;
  size_t want = arr.as<JsonArray>().size();
  if (want > MAX_FRAMES) want = MAX_FRAMES;
  if (want == 0) return 0;
  data = (uint8_t (*)[FRAME_BYTES])malloc(want * FRAME_BYTES);
  if (!data) {
    Serial.printf("[playlist] %s skipped (no memory)\n", name);
    return 0;
  }
  int n = parseFrames(arr.as<JsonArray>(), data, color, holdMs, layers);
  if (n == 0) { free(data); data = nullptr; freeLayers(layers); }
  return n;
}

// UTF-8 の文字数 (先頭バイトの数)
static int utf8Count(const String &s) {
  int n = 0;
  for (unsigned int i = 0; i < s.length(); i++) {
    if (((uint8_t)s[i] & 0xC0) != 0x80) n++;
  }
  return n;
}

// 前後の空白を除く。String::trim() は ASCII の空白しか見ないので、全角空白 (U+3000) も落とす
// (日本語入力のまま打つと全角空白が付きやすいため)。
static void trimWide(String &s) {
  for (;;) {
    s.trim();
    if (s.startsWith("　"))    s.remove(0, 3);
    else if (s.endsWith("　")) s.remove(s.length() - 3);
    else return;
  }
}

// eggWords との照合用の形。前後の空白を除き、英字だけ小文字にする (かな漢字は触らない)。
static String eggNormalize(const String &in) {
  String s = in;
  trimWide(s);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') s.setCharAt(i, c - 'A' + 'a');
  }
  return s;
}

static void cacheSavePlaylist(const char *buf, size_t len);   // 「playlist の保存」の節

// パースと反映。呼ぶのは loop() 側だけ (表示・フォント・ヒープを触るため)。
// 例外は setup() の保存済み playlist の読み込み (fromCache = true、取得タスクを起こす前)。
// fromCache のときは fwPing による自己アップデートを起こさず、保存もしない
// (保存した古い版番号で更新に走らないため / 読んだものを書き直す意味は無いため)。
// 本文は plBuf (取得タスクから受け取ったもの / 保存分を読み込んだもの) をそのまま受ける (v20)。
// 戻り値は「取り直す意味があるか」ではなく「JSONとして読めたか」。
static bool applyPlaylistBody(char *buf, size_t len, bool fromCache = false) {
  // **中身が1バイトも変わっていないなら、パースも反映もしない (v0.12)。**
  // 通常運転では60秒ごとに同じ本文が返る。それを毎回パースすると、JSONの読み取りと
  // frames のbase64デコード (最大24枚×342文字) で loop() が数十ms止まる。
  // holdMs:52 のコマ送りでは1〜2コマぶんの引っかかりになって目に見えるため、
  // 変化が無いときは丸ごと省く。取得の非同期化と合わせて、これで60秒周期の
  // 引っかかりが無くなる。
  const uint32_t bodyHash = fnv1a32(buf, len);
  if (lastPlValid && len == lastPlLen && bodyHash == lastPlHash) {
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

  // ArduinoJson 7 にはゼロコピーが無い (7.0 で廃止) ので、文字列は doc 側に写される。
  // buf は書き換わらないので、保存は解析に成功してから行う (壊れた JSON で保存分を潰さない)。
  JsonDocument doc;
  if (deserializeJson(doc, (const char *)buf, len)) {
    Serial.println("[playlist] JSON parse error");
    logMem();   // 計測 (v20)。解析時のメモリ不足 (NoMemory) との切り分け用
    return false;   // 壊れたJSONを3秒ごとに取り直しても直らない。次の周期を待つ
  }
  lastPlValid = true;
  lastPlHash = bodyHash;
  lastPlLen = len;
  lastFwPing = doc["fwPing"].is<int>() ? (int)doc["fwPing"] : 0;
  if (doc["topText"].is<const char*>()) {
    String t = String((const char*)doc["topText"]);
    if (t != plTopText) {
      plTopText = t;
      // 上段の文言が変わった = 営業カレンダーで上書きしてよいかの前提が変わった。
      // 次の1分判定までは plTopText をそのまま出す (v16)。
      hoursTopText = "";
    }
  }
  if (doc["mode"].is<const char*>()) {
    const char *m = doc["mode"];
    plMode = (strcmp(m, "scroll") == 0) ? MODE_SCROLL
           : (strcmp(m, "frames") == 0) ? MODE_FRAMES
           : (strcmp(m, "event")  == 0) ? MODE_EVENT
                                        : MODE_DUAL;
    Serial.printf("[playlist] mode=%s\n", m);
  }
  if (doc["speed"].is<float>())          plSpeed = constrain((float)doc["speed"], 5.0f, 200.0f);
  if (doc["brightness"].is<int>())       display->setBrightness8(constrain((int)doc["brightness"], 8, 255));
  if (doc["colorTop"].is<const char*>())    plColorTop = hexToColor565(doc["colorTop"]);
  if (doc["colorScroll"].is<const char*>()) {
    const char *cs = doc["colorScroll"];
    plRainbow = (strcmp(cs, "rainbow") == 0);
    if (!plRainbow) plColorScroll = hexToColor565(cs);
  }
  // scrollScale / scrollBold (v18)。キーが無いときは既定 (1 / false) に戻す。
  // 他のキーは「省略 = 現在値を維持」だが、この2つはイベント用の一時設定なので、
  // playlist.json から消せば元の見た目に戻るほうが事故が少ない。
  plScrollScale = doc["scrollScale"].is<int>() ? constrain((int)doc["scrollScale"], 1, 2) : 1;
  plScrollBold  = doc["scrollBold"].is<bool>() ? (bool)doc["scrollBold"] : false;
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
        evSet = EventSettings();   // 表示設定も同じ置き場から来たものなので破棄する (v19)
        Serial.printf("[comments] url set (%d文字)\n", (int)u.length());
      }
    } else {
      Serial.println("[comments] commentsUrl rejected (prefix mismatch)");
    }
  }
  // hoursUrl (v16): 営業カレンダーの取得先。commentsUrl と同じ扱いで、
  // 空文字なら機能OFF (緊急停止弁)、頭が HOURS_URL_PREFIX と違うものは無視する。
  if (doc["hoursUrl"].is<const char*>()) {
    String u = String((const char*)doc["hoursUrl"]);
    if (u.length() == 0 || u.startsWith(HOURS_URL_PREFIX)) {
      if (u != plHoursUrl) {
        // 取得タスクが同じ String を読んでいるので、書き換えはmutexの中で行う
        if (fetchMutex) xSemaphoreTake(fetchMutex, portMAX_DELAY);
        plHoursUrl = u;
        hoursUrlChanged = true;   // 10分周期を待たず次の取得を即時にする
        if (fetchMutex) xSemaphoreGive(fetchMutex);
        // 取得先が変わったので前のカレンダーは持ち越さない。
        // 空にしたときはこれで従来どおり plTopText がそのまま出る。
        hoursValid = false;
        hoursTopText = "";
        hoursLastState = -1;
        Serial.printf("[hours] url set (%d文字)\n", (int)u.length());
      }
    } else {
      Serial.println("[hours] hoursUrl rejected (prefix mismatch)");
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
    plFrameCount = parseFrames(doc["frames"].as<JsonArray>(), plFrameData, plFrameColor, plFrameHoldMs, plFrameLayers);
    if (frameIdx >= plFrameCount) frameIdx = 0;
    frameSince = 0;   // 差し替え直後は先頭からhold秒数え直す
    drawnFrame = -1;  // 絵や色が変わっているので描き直す
  }
  // ブース用 (v20)。scrollScale と同じく、キーが無ければ既定に戻す
  // (playlist.json から消せば入力口が閉じ、裏ルートも絵も消える。「通常に戻して」の手順と揃える)。
  plSayEnabled = doc["sayEnabled"].is<bool>() ? (bool)doc["sayEnabled"] : false;
  plSayHoldMs  = doc["sayHoldMs"].is<int>() ? constrain((int)doc["sayHoldMs"], 1000, 20000) : 5000;
  plEggMs      = doc["eggMs"].is<int>() ? constrain((int)doc["eggMs"], 1000, 20000) : 4000;
  plEggWordCount = 0;
  if (doc["eggWords"].is<JsonArray>()) {
    for (JsonVariant v : doc["eggWords"].as<JsonArray>()) {
      if (plEggWordCount >= EGG_MAX_WORDS) break;
      if (!v.is<const char*>()) continue;
      String w = eggNormalize(String((const char*)v));
      if (!w.length() || utf8Count(w) > EGG_WORD_MAX_CHARS) continue;   // 長すぎる語は丸ごと捨てる
      plEggWords[plEggWordCount++] = w;
    }
  }
  // 枚数を先に 0 にしてから置き場を差し替える (描画は同じ loop() 側なので、ここでは割り込まれない)
  plEggFrameCount = 0;
  plEggFrameCount = parseFramesAlloc(doc["eggFrames"], plEggData, plEggColor, plEggHoldMs, plEggLayers, "eggFrames");
  plIlFrameCount = 0;
  plIlFrameCount = parseFramesAlloc(doc["interlude"]["frames"], plIlData, plIlColor, plIlHoldMs, plIlLayers, "interlude");
  plIlEvery = doc["interlude"]["every"].is<int>() ? constrain((int)doc["interlude"]["every"], 1, 10) : 1;
  rebuildMarquee();
  Serial.println("[playlist] applied");
  logMem();   // 計測 (v20)
  if (!fromCache) cacheSavePlaylist(buf, len);   // 本文が前回と変わったときだけここに来る

  // fwPing (v0.7): チャット駆動の即時更新。playlist に自分より新しい番号が書かれていたら
  // 6時間周期を待たずにその場でマニフェストを見に行く。人間の明示的な指示なので
  // quietHours は無視する。実際に更新するかどうかの判断は selfUpdateCheck 側。
  // 保存済みの playlist (fromCache) では起こさない。ネットから同じ本文が取れたら
  // 上の「no change」の経路で改めて見る。
  if (!fromCache && doc["fwPing"].is<int>() && (int)doc["fwPing"] > FW_VERSION) {
    Serial.printf("[selfupdate] fwPing=%d (running v%d) → 即時確認\n",
                  (int)doc["fwPing"], FW_VERSION);
    selfUpdateCheck(true);
  }
  return true;
}

// ---------------- 営業カレンダー (v16) ----------------
// data/hours.json を読んで OPEN / CLOSED を実機が自分で決める。
// **判定ルールは scripts/update-open-closed.mjs と完全に同じにすること。**
// GitHub Actions 側も同じカレンダーを見て playlist.json を書き換え続けるので、
// ルールがずれると「実機の表示」と「playlist の topText」が食い違う。
//
// 取得先は playlist.json の "hoursUrl"。取れなかったときは前回の内容を保持する
// (通信断で表示が CLOSED に倒れないように)。

// "13:00" → 780。書き方が不正なら false。
// mjs 側の /^(\d{1,2}):(\d{2})$/ (trim後) と同じ条件にしてある。
static bool hoursParseHhmm(const char *s, int len, int &outMin) {
  int i = 0;
  while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
  int h = 0, hd = 0;
  while (i < len && s[i] >= '0' && s[i] <= '9' && hd < 2) { h = h * 10 + (s[i] - '0'); i++; hd++; }
  if (hd == 0 || i >= len || s[i] != ':') return false;
  i++;
  int m = 0, md = 0;
  while (i < len && s[i] >= '0' && s[i] <= '9' && md < 2) { m = m * 10 + (s[i] - '0'); i++; md++; }
  if (md != 2) return false;
  while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i != len) return false;
  outMin = h * 60 + m;
  return true;
}

// "13:00-17:00,18:00-19:00" を分単位の区間に分解する。
// mjs は書き方が不正だと例外で止まるが、実機は止められないので**その区間だけ捨てて進む**。
// (壊れた1行のせいで終日 CLOSED になるより、読めた区間で動くほうが害が小さい)
static void hoursParseSpec(const char *spec, HoursDay &day) {
  day.count = 0;
  if (!spec) return;                       // null = 終日CLOSED
  const char *p = spec;
  while (*p && day.count < HOURS_MAX_RANGES) {
    const char *comma  = strchr(p, ',');
    const char *segEnd = comma ? comma : p + strlen(p);
    const char *dash   = (const char *)memchr(p, '-', segEnd - p);
    int from, to;
    if (dash && hoursParseHhmm(p, dash - p, from)
             && hoursParseHhmm(dash + 1, segEnd - dash - 1, to)) {
      day.from[day.count] = (int16_t)from;
      day.to[day.count]   = (int16_t)to;
      day.count++;
    }
    if (!comma) break;
    p = comma + 1;
  }
}

// 本文をパースして保持する。呼ぶのは loop() 側だけ (ヒープを触るため)。
static bool applyHoursBody(const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("[hours] parse error");
    return false;   // 前回の内容をそのまま使い続ける
  }

  // "tz" は見ない。実機の時計は configTime で JST 固定なので localtime_r がそのまま日本時間。
  // "datesUntil" "_note" "_howto" も実機には要らない。
  const char *ot = doc["openText"]   | "";
  const char *ct = doc["closedText"] | "";
  hoursOpenText   = strlen(ot) ? String(ot) : String("OPEN");
  hoursClosedText = strlen(ct) ? String(ct) : String("CLOSED");

  // weekly: 曜日キーが無い / null は終日CLOSED (mjs の (spec ?? "") と同じ)
  static const char *const dowKeys[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
  for (int i = 0; i < 7; i++) {
    hoursWeekly[i].count = 0;
    JsonVariant v = doc["weekly"][dowKeys[i]];
    if (v.is<const char *>()) hoursParseSpec(v.as<const char *>(), hoursWeekly[i]);
  }

  // dates: 日付キーが**存在すれば**(値が null でも) weekly より優先される。
  // 過ぎた日付は判定に効かないので入れない。HOURS_MAX_DATES を超えるぶんは捨てる
  // (現行のカレンダーは1か月ぶんなので届かない。溢れた日は weekly で判定される)。
  char today[11] = "";
  {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_year > 100)
      snprintf(today, sizeof(today), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  }
  hoursDateCount = 0;
  for (JsonPair kv : doc["dates"].as<JsonObject>()) {
    if (hoursDateCount >= HOURS_MAX_DATES) break;
    const char *k = kv.key().c_str();
    if (strlen(k) != 10) continue;                  // "YYYY-MM-DD" 以外は無視
    if (today[0] && strcmp(k, today) < 0) continue; // 過去の日付は持たない
    strncpy(hoursDateKey[hoursDateCount], k, 10);
    hoursDateKey[hoursDateCount][10] = '\0';
    HoursDay &d = hoursDates[hoursDateCount];
    d.count = 0;
    if (kv.value().is<const char *>()) hoursParseSpec(kv.value().as<const char *>(), d);
    hoursDateCount++;
  }

  hoursValid = true;
  Serial.printf("[hours] applied (dates=%d)\n", hoursDateCount);
  return true;
}

// いま営業中か。ok=false は「判定できない」(カレンダー未取得 / NTP未同期)。
// ルールは mjs と同じ: dates にキーがあればそれ、無ければ曜日。
// **開店時刻は含み、閉店時刻は含まない** (19:00 ちょうどは CLOSED)。
static bool hoursIsOpenNow(bool &ok) {
  ok = false;
  if (!hoursValid) return false;
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  if (tmv.tm_year <= 100) return false;   // 上段の時計表示と同じ判定 (2000年より前 = NTP未同期)

  char key[11];
  snprintf(key, sizeof(key), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
  const HoursDay *day = nullptr;
  for (int i = 0; i < hoursDateCount; i++) {
    if (strcmp(hoursDateKey[i], key) == 0) { day = &hoursDates[i]; break; }
  }
  if (!day) day = &hoursWeekly[tmv.tm_wday];

  int nowMin = tmv.tm_hour * 60 + tmv.tm_min;
  ok = true;
  for (int i = 0; i < day->count; i++) {
    if (nowMin >= day->from[i] && nowMin < day->to[i]) return true;
  }
  return false;
}

// 判定して、上段に出す文言を決める。呼ぶのは loop() 側だけ (1分ごと + 本文の適用直後)。
static void hoursUpdateDecision() {
  bool ok = false;
  bool open = hoursIsOpenNow(ok);
  if (!ok) { hoursTopText = ""; return; }   // 未取得 / NTP未同期 → plTopText をそのまま出す

  // **自動管理の文言のときだけ置き換える。** plTopText が openText / closedText 以外
  // (イベント中に「イベント中」を出しているときなど) は一切触らない。
  // update-open-closed.mjs の「OPEN/CLOSED 以外は黙る」と同じ約束。
  if (plTopText != hoursOpenText && plTopText != hoursClosedText) {
    hoursTopText = "";
    if (hoursLastState != 2) {
      hoursLastState = 2;
      Serial.println("[hours] manual topText, skip");
    }
    return;
  }
  hoursTopText = open ? hoursOpenText : hoursClosedText;
  int state = open ? 1 : 0;
  if (state != hoursLastState) {
    hoursLastState = state;
    Serial.println(open ? "[hours] OPEN" : "[hours] CLOSED");
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
    // hoursTopText は営業カレンダーの判定結果 (v16)。空なら playlist の topText をそのまま出す。
    const String &top = hoursTopText.length() ? hoursTopText : plTopText;
    snprintf(buf, sizeof(buf), "%s", top.c_str());
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
// playlist の本文は plBuf / plBufState で受け渡す (v20、「playlist.json」の節)
static String fetchedHoursBody;            // 営業カレンダー本文 (mutexの中でだけ触る) (v16)
static volatile bool fetchedHoursReady = false; // 未反映のカレンダー本文がある
static volatile bool fetchAsync = false;   // タスクが動いている (falseならloop()側で取る)

// イベント中の実効値 (v19)。描画側はここを通して読む。
static bool evActive() { return plMode == MODE_EVENT; }
static float effSpeed()     { return (evActive() && evSet.hasSpeed) ? evSet.speed : plSpeed; }
static bool effRainbow()    { return (evActive() && evSet.hasColor) ? evSet.rainbow : plRainbow; }
static uint16_t effColor()  { return (evActive() && evSet.hasColor) ? evSet.color : plColorScroll; }
static int  effScale()      { return (evActive() && evSet.hasScale) ? evSet.scale : plScrollScale; }
static bool effBold()       { return (evActive() && evSet.hasBold)  ? evSet.bold  : plScrollBold; }
static int  effEventLines() { return (evActive() && evSet.hasLines) ? evSet.lines : 2; }

// "settings" を evSet に読み込む (v19)。無いキーは「指定なし」、範囲外は丸める。
// 受け取るたびに丸ごと置き換える (管理画面でキーを消せば playlist の値に戻る)。
static void applyEventSettings(JsonObject s) {
  EventSettings n;
  if (s["colorScroll"].is<const char*>()) {
    const char *cs = s["colorScroll"];
    n.hasColor = true;
    n.colorStr = cs;
    n.rainbow = (strcmp(cs, "rainbow") == 0);
    if (!n.rainbow) n.color = hexToColor565(cs);
  }
  if (s["speed"].is<float>())    { n.hasSpeed = true; n.speed = constrain((float)s["speed"], 5.0f, 200.0f); }
  if (s["scrollScale"].is<int>()) { n.hasScale = true; n.scale = constrain((int)s["scrollScale"], 1, 2); }
  if (s["scrollBold"].is<bool>()) { n.hasBold = true;  n.bold = (bool)s["scrollBold"]; }
  if (s["eventLines"].is<int>())  { n.hasLines = true; n.lines = constrain((int)s["eventLines"], 1, 2); }
  if (s["eventFlow"].is<const char*>()) n.flow = (const char*)s["eventFlow"];
  evSet = n;
  if (evActive()) {
    Serial.printf("[comments] settings: lines=%d scale=%d bold=%d speed=%d color=%s flow=%s\n",
                  effEventLines(), effScale(), (int)effBold(), (int)effSpeed(),
                  n.hasColor ? n.colorStr.c_str() : "-", n.flow.length() ? n.flow.c_str() : "-");
  }
}

// JSON 形式の応答 (v19): {"settings":{...},"comments":[{"id":12,"text":"…"} or "…", …]}
// 壊れていたら何も変えない (前回のコメント・設定を保持)。
// "settings" が無ければ前回の設定を保持、"comments" が無ければ前回のコメントを保持。
static void applyCommentsJson(const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("[comments] parse error");
    return;
  }
  if (doc["settings"].is<JsonObject>()) applyEventSettings(doc["settings"].as<JsonObject>());
  if (doc["comments"].is<JsonArray>()) {
    int newCount = 0;
    for (JsonVariant v : doc["comments"].as<JsonArray>()) {
      if (newCount >= MAX_COMMENTS) break;
      const char *t = v.is<const char*>() ? v.as<const char*>()
                    : v["text"].is<const char*>() ? v["text"].as<const char*>() : nullptr;
      if (!t) continue;
      String line(t);
      line.trim();
      if (line.length()) comments[newCount++] = line;
    }
    commentCount = newCount;   // 0件でも反映する (テキスト形式と同じ)
  }
  rebuildMarquee();
}

// 受け取った本文を comments[] に展開する。呼ぶのは loop() 側だけ。
// 先頭が '{' なら JSON 形式 (v19)、それ以外は従来どおり 1 行 1 件のテキスト。
static void applyCommentsBody(const String &body) {
  if (body.length() && body[0] == '{') {
    applyCommentsJson(body);
    return;
  }
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
  // ヘッダ待ちは10秒 (v16)。GASはコールドスタートだと応答が5秒を超えることがあり、
  // 既定の5秒だとヘッダが返る前に打ち切って -11 になるのを実機で確認した。
  // 本文の締め切り (readBodyWithDeadline) は据え置き。
  String out = httpGetString(url, code, fetchAsync, 10000);   // 1行1コメントのプレーンテキスト
  if (code != 200) {
    Serial.printf("[comments] fetch failed (%d)\n", code);
    return false;   // 通信断で表示中のコメントが消えないよう、失敗時は前回の内容を残す
  }
  body = out;
  return true;
}

// 営業カレンダーの取得 (v16)。コメントと同じ経路・同じ作法。
// 取得できたら true を返し、body に本文を入れる。
static bool fetchHoursOnce(String &body) {
  String url;
  if (fetchMutex) xSemaphoreTake(fetchMutex, portMAX_DELAY);
  url = plHoursUrl;
  if (fetchMutex) xSemaphoreGive(fetchMutex);
  if (url.length() == 0) return false;   // 機能OFF (緊急停止弁)
  if (WiFi.status() != WL_CONNECTED) return false;
  // GitHub Pages のCDNが古い内容を返さないようキャッシュ回避のクエリを付ける
  url += (url.indexOf('?') >= 0 ? "&t=" : "?t=") + String(millis());
  int code;
  String out = httpGetString(url, code, fetchAsync);
  if (code != 200 || !out.length()) {
    Serial.printf("[hours] fetch failed (%d)\n", code);
    return false;   // 通信断で CLOSED に倒れないよう、失敗時は前回の内容を保持する
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
  unsigned long nextHours = 0;
  const bool havePlaylistUrl = strlen(PLAYLIST_URL) > 0;

  for (;;) {
    unsigned long ms = millis();

    // ---- playlist ----
    // 比較を符号付きで行い、millis()が一周しても止まらないようにする。
    if (havePlaylistUrl && (long)(ms - nextPlaylist) >= 0) {
      size_t len;
      bool got = fetchPlaylistBody(len, true);   // 取れたら plBuf が READY になる (コピーしない)
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

    // ---- 営業カレンダー (v16) ----
    // plHoursUrl が空のときは通信しない (commentsUrl と同じ緊急停止弁)。
    // 日付と曜日しか見ないので10分おきで足り、切り替わりの判定は loop() 側が1分ごとに行う。
    if (hoursUrlChanged || (long)(ms - nextHours) >= 0) {
      hoursUrlChanged = false;   // 取得先が変わった直後は周期を待たずに取りに行く
      String body;
      if (fetchHoursOnce(body)) {
        xSemaphoreTake(fetchMutex, portMAX_DELAY);
        fetchedHoursBody = body;
        fetchedHoursReady = true;
        xSemaphoreGive(fetchMutex);
      }
      nextHours = millis() + HOURS_INTERVAL_MS;
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
// plBuf を直接読む。READY のあいだ取得タスクは書かないので、反映中に中身は変わらない (v20)。
static void applyFetchedPlaylist() {
  if (plBufState != PLBUF_READY) return;
  xSemaphoreTake(fetchMutex, portMAX_DELAY);
  size_t len = plBufLen;
  xSemaphoreGive(fetchMutex);
  applyPlaylistBody(plBuf, len);
  plBufSetState(PLBUF_FREE);   // 反映し終えた。次の取得で書いてよい
}

// loop() から呼ぶ。タスクが持ってきた営業カレンダーがあればパースして反映する (v16)。
static void applyFetchedHours() {
  if (!fetchedHoursReady) return;
  String body;
  xSemaphoreTake(fetchMutex, portMAX_DELAY);
  body = fetchedHoursBody;
  fetchedHoursBody = "";   // 持ち回らない (カレンダー本文は1〜2KBある)
  fetchedHoursReady = false;
  xSemaphoreGive(fetchMutex);
  if (applyHoursBody(body)) hoursUpdateDecision();   // 適用した直後に判定する
}

// タスクを作れなかったときの退路。従来どおり loop() の中で取りに行く
// (2〜3秒止まるが、表示が更新できないよりはよい)。
static void fetchCommentsBlocking() {
  String body;
  if (fetchCommentsOnce(body)) applyCommentsBody(body);
}

// 同上 (v16)。
static void fetchHoursBlocking() {
  String body;
  if (fetchHoursOnce(body) && applyHoursBody(body)) hoursUpdateDecision();
}

// 同上。戻り値は「次を60秒後にしてよいか」(false なら3秒後に再挑戦)。
static bool fetchPlaylistBlocking() {
  if (strlen(PLAYLIST_URL) == 0) return true;
  size_t len;
  if (!fetchPlaylistBody(len, false)) return false;
  applyPlaylistBody(plBuf, len);
  plBufSetState(PLBUF_FREE);
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
  // 優先度は IDLE にして、core 0 の WiFi/TCP スタックを絶対に待たせない (v14)。
  BaseType_t ok = xTaskCreatePinnedToCore(fetchTask, "fetch", 20480, nullptr, tskIDLE_PRIORITY, nullptr, 0);
  if (ok != pdPASS) {
    Serial.println("[fetch] タスク作成に失敗。loop()側で取得する");
    return;
  }
  fetchAsync = true;
  Serial.println("[fetch] 取得タスクを開始 (core 0 / playlist+コメント)");
}

// ---------------- playlist の保存 (v20) ----------------
// 最後に反映した playlist.json を LittleFS (min_spiffs の spiffs 領域、128KB) に置く。
// ブースのようにネットが無い場所で再起動しても、前回の表示 (sayEnabled も含む) で立ち上がる。
// ネットが取れた時点で最新に置き換わる。本文は plBuf (24KB) 以内なので、
// 書きかけの一時ファイルと合わせても領域に収まる。
static bool cacheOk = false;
static const char *CACHE_PATH = "/playlist.json";
static const char *CACHE_TMP  = "/playlist.tmp";

// setup() から1回だけ。マウントできなければ1回だけフォーマットして再挑戦する
// (初回起動や、SPIFFS など別の形式が残っている領域)。それでも駄目なら保存機能ごと止める。
static void cacheBegin() {
  if (!LittleFS.begin(false)) {
    LittleFS.format();
    if (!LittleFS.begin(false)) {
      Serial.println("[cache] disabled");
      return;
    }
  }
  cacheOk = true;
}

// 書き込みは数十ms。applyPlaylistBody (loop() 側) で本文が変わったときだけ呼ばれる。
// 一時ファイルに書いてから付け替えるので、書いている最中に電源が落ちても前の版が残る。
static void cacheSavePlaylist(const char *buf, size_t len) {
  if (!cacheOk) return;
  File f = LittleFS.open(CACHE_TMP, "w");
  if (!f) { Serial.println("[cache] save failed"); return; }
  size_t w = f.write((const uint8_t *)buf, len);
  f.close();
  if (w != len) {
    LittleFS.remove(CACHE_TMP);
    Serial.println("[cache] save failed");
    return;
  }
  if (!LittleFS.rename(CACHE_TMP, CACHE_PATH)) {
    // 付け替え先が既にあると失敗する実装に備えて、消してからもう一度
    LittleFS.remove(CACHE_PATH);
    if (!LittleFS.rename(CACHE_TMP, CACHE_PATH)) {
      LittleFS.remove(CACHE_TMP);
      Serial.println("[cache] save failed");
      return;
    }
  }
  Serial.printf("[cache] playlist saved (%d bytes)\n", (int)len);
}

// setup() で WiFi 接続の前に呼ぶ。fwPing は見ない (applyPlaylistBody の fromCache)。
// commentsUrl / hoursUrl はここで入れてよい (取得タスクが後で使う)。
// 取得タスクを起こす前なので、plBuf はそのまま使ってよい (受け渡しと同じ経路で反映する)。
static void cacheLoadPlaylist() {
  if (!cacheOk) return;
  File f = LittleFS.open(CACHE_PATH, "r");
  if (!f) return;   // まだ保存したことが無い
  size_t n = f.size();
  if (n == 0 || n > sizeof(plBuf) - 1) { f.close(); return; }
  size_t got = 0;
  while (got < n) {
    int r = f.read((uint8_t *)plBuf + got, n - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  f.close();
  if (got != n) return;   // 読み切れなかったものは使わない
  plBuf[n] = '\0';
  Serial.printf("[cache] playlist loaded (%d bytes)\n", (int)n);
  applyPlaylistBody(plBuf, n, true);
}

// ---------------- ブース入力 (say / egg) (v20) ----------------
// 実機が LAN 内に入力ページを持つ。GET / で入力欄、POST /say で文字を受け取る。
// 認証は無し (LAN 内限定)。そのかわり playlist.json の sayEnabled で開け閉めする。
// handleClient() は loop() から呼ぶので、ハンドラも loop() 側で動く (キューに mutex は要らない)。
// **打たれた文字そのものはログに出さない** (公共の場で打たれた文言をシリアルに残さない)。
static WebServer sayServer(80);
static bool sayServerOn = false;
static bool sayNeedRestart = false;   // 開いているあいだに WiFi が切れた → 繋がり直したら開き直す

static String sayQueue[SAY_QUEUE_MAX];
static int sayQHead = 0, sayQCount = 0;

static const char SAY_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>もしも掲示板</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:16px;font-family:sans-serif;background:#111;color:#ff9c00}
#h{margin:0 0 12px;font-size:24px;line-height:1.4}
form{display:flex;flex-direction:column;gap:12px}
input{width:100%;font-size:44px;height:1.6em;padding:0 12px;border-radius:16px;border:6px solid #ff9c00;background:#000;color:#ff9c00}
button{width:100%;font-size:32px;height:calc(44px * 1.6);border-radius:16px;border:0;background:#ff9c00;color:#000;font-weight:bold}
#s{margin-top:12px;min-height:1.4em;font-size:24px}
</style></head><body>
<p id="h">ここに ことばを かいて おくる を おしてね</p>
<form id="f"><input id="t" name="text" maxlength="40" autocomplete="off" autofocus><button>おくる</button></form>
<div id="s"></div>
<script>
var f=document.getElementById('f'),t=document.getElementById('t'),s=document.getElementById('s');
f.addEventListener('submit',function(e){
  e.preventDefault();
  var v=t.value;
  t.value='';t.focus();
  if(!v.trim())return;
  fetch('/say',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'text='+encodeURIComponent(v)})
    .then(function(r){s.textContent=r.status==200?'おくったよ':r.status==429?'じゅんばんまち いっぱい。すこし まってね':'おくれなかった';})
    .catch(function(){s.textContent='おくれなかった';});
});
</script></body></html>
)rawliteral";

static void sayHandleRoot() {
  sayServer.send_P(200, "text/html; charset=utf-8", SAY_HTML);
}

// trim → 制御文字を除く → 40文字で切る。空なら 400、キューが満杯なら 429。
static void sayHandlePost() {
  String raw = sayServer.arg("text");
  trimWide(raw);
  String t;
  t.reserve(raw.length());
  for (unsigned int i = 0; i < raw.length(); i++) {
    uint8_t c = (uint8_t)raw[i];
    if (c < 0x20 || c == 0x7F) continue;   // 改行・タブなども含めて落とす
    t += (char)c;
  }
  // 40文字目までで切る (UTF-8 の途中で切らないよう、41文字目の先頭バイトの手前で切る)
  int chars = 0;
  for (unsigned int i = 0; i < t.length(); i++) {
    if (((uint8_t)t[i] & 0xC0) != 0x80) {
      if (chars == SAY_MAX_CHARS) { t.remove(i); break; }
      chars++;
    }
  }
  trimWide(t);   // 切った端に空白が残ったぶん
  if (!t.length()) { sayServer.send(400, "text/plain", "empty"); return; }
  if (sayQCount >= SAY_QUEUE_MAX) { sayServer.send(429, "text/plain", "busy"); return; }
  sayQueue[(sayQHead + sayQCount) % SAY_QUEUE_MAX] = t;
  sayQCount++;
  Serial.printf("[say] queued (%d文字)\n", utf8Count(t));
  sayServer.send(200, "text/plain", "ok");
}

static void sayHandleNotFound() {
  sayServer.send(404, "text/plain", "not found");
}

// setup() から1回。開くのは sayServerSync() (sayEnabled かつ WiFi 接続中のとき)。
static void saySetup() {
  sayServer.on("/", HTTP_GET, sayHandleRoot);
  sayServer.on("/say", HTTP_POST, sayHandlePost);
  sayServer.onNotFound(sayHandleNotFound);
}

// loop() から毎回。sayEnabled と WiFi の状態に合わせて入力口を開け閉めする。
static void sayServerSync() {
  if (!plSayEnabled) {
    if (sayServerOn) {
      sayServer.stop();
      sayServerOn = false;
      sayNeedRestart = false;
      sayQCount = 0;   // 閉じたら、まだ出していないぶんも出さない
      Serial.println("[say] server off");
    }
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (sayServerOn) sayNeedRestart = true;
    return;
  }
  if (sayServerOn && !sayNeedRestart) return;
  if (sayServerOn) sayServer.stop();   // WiFi が繋がり直した。待ち受けを張り直す
  sayServer.begin();
  sayServerOn = true;
  sayNeedRestart = false;
  Serial.printf("[say] server on http://%s/\n", WiFi.localIP().toString().c_str());
}

// ---- 表示 ----
// say / egg の表示中は、元の mode の描画 (frames / dual / scroll / event、インタールードも)
// を止めて差し替える。終わったら止めていた時間ぶん各タイマーをずらし、続きから再開する。
enum SayState { SAY_IDLE, SAY_STATIC, SAY_SCROLL, SAY_EGG };
static SayState sayState = SAY_IDLE;
static uint16_t sayCps[SAY_MAX_CHARS + 1];
static int sayLen = 0, sayW = 0;
static float sayX = 0;
static unsigned long sayStartMs = 0;      // 表示を始めた時刻 (止めていた時間の計算にも使う)
static unsigned long sayLastFrame = 0;
static int eggIdx = 0, eggDrawn = -1;
static unsigned long eggSince = 0;

// インタールードの状態 (v20)
static bool ilActive = false;
static int  ilIdx = 0, ilDrawn = -1;
static unsigned long ilSince = 0;
static long ilDrawnSec = -1;              // 2段のとき、上段の時計を描いた秒
static unsigned int ilPasses = 0;         // 本文が流れ切った回数

// いまの mode で文字を2段 (上段 + 下段スクロール) にするか。
// eventLines:1 のときは上段を出さず、MODE_SCROLL と同じ1行の経路で流す (v19)。
static bool marqueeTwoRows() {
  const bool oneRow = (plMode == MODE_SCROLL) || (plMode == MODE_EVENT && effEventLines() == 1);
  return !oneRow && PANEL_H >= 32;
}

// キューの先頭を取り出して表示を始める。
static void sayStartNext(unsigned long ms) {
  String t = sayQueue[sayQHead];
  sayQueue[sayQHead] = "";
  sayQHead = (sayQHead + 1) % SAY_QUEUE_MAX;
  sayQCount--;
  sayStartMs = ms;
  sayLastFrame = ms - 33;   // 最初のコマはすぐ描く

  if (plEggFrameCount > 0) {
    String key = eggNormalize(t);
    for (int i = 0; i < plEggWordCount; i++) {
      if (key != plEggWords[i]) continue;
      // 出すのは playlist に書いてある語のほう (公開されている文字列なので残してよい)
      Serial.printf("[say] egg: %s\n", plEggWords[i].c_str());
      sayState = SAY_EGG;
      eggIdx = 0;
      eggDrawn = -1;
      eggSince = ms;
      return;
    }
  }
  sayLen = decodeUtf8(t, sayCps, SAY_MAX_CHARS + 1);
  sayW = textWidth(sayCps, sayLen, 2, true);
  if (sayW <= PANEL_W) {
    sayState = SAY_STATIC;
  } else {
    sayState = SAY_SCROLL;
    sayX = PANEL_W;
  }
}

// say / egg が終わった。止めていたあいだの時間ぶんタイマーをずらして元の表示に戻す。
static void sayFinish(unsigned long ms) {
  unsigned long paused = ms - sayStartMs;
  sayState = SAY_IDLE;
  if (frameSince != 0) frameSince += paused;   // frameIdx と残りの hold を保ったまま再開
  drawnFrame = -1;                              // パネルは上書きされているので描き直す
  if (ilActive) { ilSince += paused; ilDrawn = -1; ilDrawnSec = -1; }
}

// say / egg を1回ぶん進める。表示中なら true (= loop() は元の mode を描かない)。
static bool sayTick(unsigned long ms) {
  if (sayState == SAY_IDLE) {
    if (sayQCount == 0) return false;
    sayStartNext(ms);
  }

  if (sayState == SAY_EGG) {
    if (plEggFrameCount == 0 || (long)(ms - (sayStartMs + plEggMs)) >= 0) {
      sayFinish(ms);
      return sayQCount > 0 ? sayTick(ms) : false;
    }
    if (eggIdx >= plEggFrameCount) { eggIdx = 0; eggDrawn = -1; }   // 再生中に playlist で減った
    // frames モードと同じ進め方 (溜まった遅れは1周ぶんまで追い、それ以上は諦める)
    unsigned int guard = 0;
    while (ms - eggSince >= (unsigned long)plEggHoldMs[eggIdx]) {
      eggSince += plEggHoldMs[eggIdx];
      eggIdx = (eggIdx + 1) % plEggFrameCount;
      if (++guard >= (unsigned int)plEggFrameCount) { eggSince = ms; break; }
    }
    if (eggDrawn != eggIdx) {
      eggDrawn = eggIdx;
      drawFrame(plEggData[eggIdx], plEggColor[eggIdx], plEggLayers, eggIdx);
    }
    return true;
  }

  // 文字 (32px 太字)。描画は通常の文字表示と同じく約30fps
  if (ms - sayLastFrame < 33) return true;
  float dt = (ms - sayLastFrame) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  sayLastFrame = ms;

  if (sayState == SAY_STATIC) {
    if ((long)(ms - (sayStartMs + plSayHoldMs)) >= 0) {
      sayFinish(ms);
      return sayQCount > 0 ? sayTick(ms) : false;
    }
  } else {
    sayX -= max(plSpeed, 40.0f) * dt;
    if (sayX < -sayW) {   // 右端から左端へ1回だけ流した
      sayFinish(ms);
      return sayQCount > 0 ? sayTick(ms) : false;
    }
  }
  const int x = (sayState == SAY_STATIC) ? (PANEL_W - sayW) / 2 : (int)sayX;
  const int y = (PANEL_H - 32) / 2;
  display->clearScreen();
  drawTextColored(sayCps, nullptr, nullptr, sayLen, x, y, effColor(), effRainbow(), 2, true);
  return true;
}

// 本文が左端まで流れ切ったところで呼ぶ。every 回に1回、インタールードを始める。
static void interludeOnPass(unsigned long ms) {
  if (plIlFrameCount == 0 || plMode == MODE_FRAMES) return;
  ilPasses++;
  if (ilPasses % (unsigned int)plIlEvery != 0) return;
  ilActive = true;
  ilIdx = 0;
  ilDrawn = -1;
  ilDrawnSec = -1;
  ilSince = ms;
}

// インタールードを1回ぶん進める。再生中なら true。
// 各コマ holdMs で1回だけ流し、最後のコマが終わったら本文を右端から再開する。
static bool interludeTick(unsigned long ms) {
  if (!ilActive) return false;
  if (plIlFrameCount == 0 || plMode == MODE_FRAMES || ilIdx >= plIlFrameCount) {
    ilActive = false;   // 再生中に playlist で消えた / 減った / frames モードになった
    return false;
  }
  if (ms - ilSince >= (unsigned long)plIlHoldMs[ilIdx]) {
    ilSince += plIlHoldMs[ilIdx];
    if (ms - ilSince >= (unsigned long)plIlHoldMs[ilIdx]) ilSince = ms;   // 大きく止まったあとは追わない
    if (++ilIdx >= plIlFrameCount) {
      ilActive = false;
      return false;
    }
  }
  // 2段のときは、コマの上に上段 (文言・時計) を描き直す。絵は下半分に寄せたものを使う前提。
  // 時計の点滅に追従するため、コマが変わらなくても秒が変わったら描き直す。
  const bool twoRows = marqueeTwoRows();
  const long sec = twoRows ? (long)time(nullptr) : -1;
  if (ilDrawn != ilIdx || sec != ilDrawnSec) {
    ilDrawn = ilIdx;
    ilDrawnSec = sec;
    drawFrame(plIlData[ilIdx], plIlColor[ilIdx], plIlLayers, ilIdx);
    if (twoRows) drawTopLine();
  }
  return true;
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

  {
    esp_task_wdt_config_t wdtcfg;
    wdtcfg.timeout_ms     = 60000;
    wdtcfg.idle_core_mask = (1 << 0);
    wdtcfg.trigger_panic  = true;
    esp_err_t r = esp_task_wdt_reconfigure(&wdtcfg);
    if (r != ESP_OK) r = esp_task_wdt_init(&wdtcfg);
    Serial.printf("[wdt] task watchdog timeout=60s (%s)\n", r == ESP_OK ? "ok" : "FAILED");
  }

  HUB75_I2S_CFG mxconfig(PANEL_W, PANEL_H, 1);
  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  display->setBrightness8(BRIGHTNESS);
  display->clearScreen();

  drawBootVersion();
  unsigned long bootShownAt = millis();
  Serial.printf("[boot] moshimo-sign FW_VERSION=%d\n", FW_VERSION);

  cfgLoad();          // 認証情報の出所を決める (v17)。WiFi接続より前に読む
  // 保存済みの playlist (v20)。ネットが無くても前回の表示で立ち上がるよう、WiFi接続より前に読む
  cacheBegin();
  cacheLoadPlaylist();
  saySetup();         // 入力口の道案内だけ。開くのは loop() の sayServerSync() (v20)
  wifiSetup();

  configTime(9 * 3600, 0, "ntp.nict.jp", "pool.ntp.org");  // JST

  // パスワードが無いときは OTA を上げない (v17)。認証情報を .bin に入れない構成では
  // config.h の OTA_PASSWORD が "" になるため、NVS に ota が入っていない実機が
  // 「同一LANの誰でもパスワード無しで書き換えられる」状態になってしまうのを防ぐ。
  // ArduinoOTA.handle() は begin() を呼んでいなければ即 return するので loop() 側は変更不要。
  if (cfgOta.length()) {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(cfgOta.c_str());
    ArduinoOTA.begin();
  } else {
    Serial.println("[ota] disabled (no password)");
  }

  fetchSetup();       // 受け渡しの入れ物を作る (v0.10 / 名前と役割を v0.12 で整理)
  fetchTaskStart();   // playlist+コメントの取得タスクを起こす (v0.12)

  rebuildMarquee();

  // WiFiがすぐ繋がると起動処理が数秒で終わり、版数を読む前に通常表示へ移ってしまう。
  // 目視確認が目的なので、最低 BOOT_VERSION_MIN_MS は出したままにする。
  while (millis() - bootShownAt < BOOT_VERSION_MIN_MS) delay(50);
}

void loop() {
  ArduinoOTA.handle();
  cfgSerialPoll();    // 認証情報の投入 (v17)。届いている分を読むだけで待たない
  if (sayServerOn) sayServer.handleClient();   // ブース入力 (v20)

  unsigned long ms = millis();

  // 切断時のみ再接続を試みる。もう一方のAPが届いていればそちらへ切り替わる。
  // run() はスキャンのため数秒ブロックするので、繋がっている間は呼ばない。
  static unsigned long lastWifiRetry = 0;
  if (WiFi.status() != WL_CONNECTED && ms - lastWifiRetry > 30000) {
    lastWifiRetry = ms;
    wifiMulti.run(5000);
  }
  sayServerSync();    // sayEnabled と WiFi の状態に合わせて入力口を開け閉めする (v20)

  // コメントもplaylistも、取得は別タスク (v0.12)。ここでは持ってきた本文を
  // 反映するだけなので待たない = スクロールが止まらない。
  // タスクを作れなかったときだけ、従来どおりここで取りに行く。
  if (fetchAsync) {
    applyFetchedComments();
    applyFetchedPlaylist();
    applyFetchedHours();
  } else {
    if (plCommentsUrl.length() && ms - lastFetch > FETCH_INTERVAL_MS) {
      lastFetch = ms;
      fetchCommentsBlocking();
    }
    // 営業カレンダー (v16)。取得タスクが無いときはここで取りに行く (10分ごとなので影響は小さい)
    static unsigned long nextHoursAt = 0;
    if (plHoursUrl.length() && (long)(ms - nextHoursAt) >= 0) {
      nextHoursAt = ms + HOURS_INTERVAL_MS;
      fetchHoursBlocking();
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

  // 営業カレンダーの判定 (v16): 分が変わったときだけ見る。
  // 取得は10分おきでも、OPEN/CLOSED の切り替わりは分単位で合う。
  {
    static int lastJudgedMin = -1;
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_min != lastJudgedMin) {
      lastJudgedMin = tmv.tm_min;
      hoursUpdateDecision();
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
  // ブース入力 (v20)。say / egg の表示中、インタールードの再生中は元の mode を描かない
  if (sayTick(ms)) return;
  if (interludeTick(ms)) return;

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
      drawFrame(plFrameData[frameIdx], plFrameColor[frameIdx], plFrameLayers, frameIdx);
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

  // 2段表示の下段は16px高しかないので、拡大は1行スクロールのときだけ効かせる (v18)。
  // 太字は高さが変わらないのでどちらでも効く。
  // イベント中は GAS の settings が playlist の値を上書きする (v19、eff* を参照)。
  // eventLines:1 のときは上段を出さず、MODE_SCROLL と同じ1行の経路で流す。
  const bool twoRows = marqueeTwoRows();
  const int effScaleNow = effScale();
  int scale = (!twoRows && 16 * effScaleNow <= PANEL_H) ? effScaleNow : 1;
  const bool bold = effBold();
  const bool rainbow = effRainbow();
  const uint16_t color = effColor();
  // 本文・拡大率・太字のどれかが変わったときだけ幅を測り直す
  // (拡大・太字は playlist 取得で変わりうるが、rebuildMarquee は本文が変わったときしか走らない)。
  if (scale != marqueeWScale || bold != marqueeWBold) {
    marqueeW = textWidth(marqueeCps, marqueeLen, scale, bold);
    marqueeWScale = scale;
    marqueeWBold = bold;
  }

  scrollX -= effSpeed() * dt;
  if (scrollX < -marqueeW) {
    scrollX = PANEL_W;
    // インタールード (v20): 流れ切った回数が every の倍数なら、右端から再開する前に絵を挟む
    interludeOnPass(ms);
    if (interludeTick(ms)) return;
  }

  display->clearScreen();
  if (twoRows) {
    drawTopLine();
    drawTextColored(marqueeCps, marqueeCol, marqueeColSet, marqueeLen, (int)scrollX, 16, color, rainbow, 1, bold);
  } else {
    const int y = (PANEL_H - 16 * scale) / 2;
    drawTextColored(marqueeCps, marqueeCol, marqueeColSet, marqueeLen, (int)scrollX, y, color, rainbow, scale, bold);
  }
}
