/**
 * もしも電光掲示板 — イベントコメントの置き場 (Google Apps Script)
 *
 * スプレッドシートを保存先にして、次の口を開ける:
 *   GET  ?action=list&key=…               コメント一覧をJSONで返す（管理画面用・鍵が要る）
 *   GET  ?action=public                   承認済みだけをJSONで返す（参加者画面用・鍵は不要）
 *   GET  ?action=text                     承認済みだけを1行1件のテキストで返す
 *                                         ← このURLを playlist.json の commentsUrl にする
 *   GET  ?action=settings                 参加者画面の文言と色をJSONで返す（鍵は不要）
 *   GET  ?action=events&key=…             保存済みイベントの一覧（鍵が要る）
 *   POST {"action":"add","text":"..."}    1件足す（参加者の投稿）
 *   POST {"action":"state","id":1,"state":"approved","key":"..."}
 *                                         状態を変える（管理者のみ）
 *   POST {"action":"settings","key":"...","title":"...","lead":"...","color":"...","placeholder":"..."}
 *                                         参加者画面の文言と色を保存（管理者のみ）
 *   POST {"action":"event_save","key":"...","name":"..."}   いまの設定をその名前で保存（同名は上書き）
 *   POST {"action":"event_load","key":"...","name":"..."}   その名前の設定をいまの設定にする
 *   POST {"action":"event_start","key":"...","name":"..."}  comments を archive_<日付>_<名前> に退避して空にし、
 *                                         その名前の設定を読み込む（保存が無ければ設定はそのまま）
 *
 * 画面: prototypes/event-comments/（参加者） / prototypes/event-comments-admin/（管理）
 * 手順: docs/event-comments.md
 *
 * ※ ADMIN_KEY はこのファイルに直接書かず、スクリプトプロパティに入れること。
 *    Apps Script の「プロジェクトの設定」→「スクリプト プロパティ」で
 *    ADMIN_KEY という名前で保存する。このリポジトリは公開されているため、
 *    ここに書くと誰でも承認操作ができてしまう。
 */

var SHEET_NAME = 'comments';
var MAX_TEXT = 100;        // 1件の最大文字数（実機は64dot幅なので長文は流れきらない）
var MAX_TO_DEVICE = 20;    // firmware の MAX_COMMENTS と合わせる

// 参加者画面の文言と色。settings シートが空のとき・項目が空のときはこれを返す
var SETTINGS_SHEET = 'settings';
var EVENTS_SHEET = 'events';
var SETTING_KEYS = ['title', 'lead', 'color', 'placeholder'];
var SETTING_DEFAULTS = {
  title: 'コメントを送る',
  lead: 'いま思ったことを送ってください。運営が確認したものが電光掲示板に流れます。',
  color: 'FF9C00',
  placeholder: '質問・コメント・つぶやき、なんでも'
};
var SETTING_MAX = { title: 40, lead: 200, color: 6, placeholder: 60 };
var MAX_EVENT_NAME = 40;

function adminKey_() {
  return PropertiesService.getScriptProperties().getProperty('ADMIN_KEY') || '';
}

function sheet_() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sh = ss.getSheetByName(SHEET_NAME);
  if (!sh) {
    sh = ss.insertSheet(SHEET_NAME);
    sh.appendRow(['id', 'ts', 'text', 'state']);
  }
  return sh;
}

function rows_() {
  var sh = sheet_();
  var last = sh.getLastRow();
  if (last < 2) return [];
  return sh.getRange(2, 1, last - 1, 4).getValues().map(function (r) {
    return { id: Number(r[0]), ts: String(r[1]), text: String(r[2]), state: String(r[3] || 'pending') };
  });
}

function isAdmin_(k) {
  var key = adminKey_();
  return !!key && String(k || '') === key;
}

// 管理者が入れる文字列は「文字列のまま」書く。書式を書式なしテキストにしておかないと、
// "000000" が数値の 0 に、"=..." が数式になってしまう
function writeText_(range, values) {
  range.setNumberFormat('@').setValues(values);
}

function settingsSheet_() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sh = ss.getSheetByName(SETTINGS_SHEET);
  if (!sh) {
    sh = ss.insertSheet(SETTINGS_SHEET);
    sh.appendRow(['key', 'value']);
  }
  return sh;
}

// 保存されている値だけ（空や未保存は入らない）
function storedSettings_() {
  var sh = settingsSheet_();
  var last = sh.getLastRow();
  var out = {};
  if (last < 2) return out;
  sh.getRange(2, 1, last - 1, 2).getValues().forEach(function (r) {
    var k = String(r[0]);
    if (SETTING_KEYS.indexOf(k) >= 0 && String(r[1]) !== '') out[k] = String(r[1]);
  });
  return out;
}

// 参加者画面に返す値（空の項目は既定値で埋める）
function settings_() {
  var stored = storedSettings_();
  var out = {};
  SETTING_KEYS.forEach(function (k) { out[k] = stored[k] != null ? stored[k] : SETTING_DEFAULTS[k]; });
  return out;
}

// 色は "FF9C00" / "#ff9c00" のどちらでも受け、"FF9C00" の形にそろえる。空は既定に戻す
function cleanSettings_(src) {
  var out = {};
  for (var i = 0; i < SETTING_KEYS.length; i++) {
    var k = SETTING_KEYS[i];
    if (src[k] == null) continue;                 // 送られてこなかった項目は変えない
    var v = String(src[k]).trim();
    if (k === 'color' && v !== '') {
      v = v.replace(/^#/, '').toUpperCase();
      if (!/^[0-9A-F]{6}$/.test(v)) return { error: 'bad color' };
    }
    if (v.length > SETTING_MAX[k]) v = v.slice(0, SETTING_MAX[k]);
    out[k] = v;
  }
  return { values: out };
}

function writeSettings_(values) {
  var sh = settingsSheet_();
  var last = sh.getLastRow();
  var keys = last < 2 ? [] : sh.getRange(2, 1, last - 1, 1).getValues().map(function (r) { return String(r[0]); });
  Object.keys(values).forEach(function (k) {
    var idx = keys.indexOf(k);
    if (idx >= 0) {
      writeText_(sh.getRange(idx + 2, 2), [[values[k]]]);
    } else {
      var row = sh.getLastRow() + 1;
      writeText_(sh.getRange(row, 1, 1, 2), [[k, values[k]]]);
      keys.push(k);
    }
  });
}

function eventsSheet_() {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sh = ss.getSheetByName(EVENTS_SHEET);
  if (!sh) {
    sh = ss.insertSheet(EVENTS_SHEET);
    sh.appendRow(['name', 'title', 'lead', 'color', 'placeholder', 'savedAt']);
  }
  return sh;
}

function events_() {
  var sh = eventsSheet_();
  var last = sh.getLastRow();
  if (last < 2) return [];
  return sh.getRange(2, 1, last - 1, 6).getValues().map(function (r) {
    var savedAt = r[5] instanceof Date ? r[5].toISOString() : String(r[5]);
    return { name: String(r[0]), title: String(r[1]), lead: String(r[2]),
             color: String(r[3]), placeholder: String(r[4]), savedAt: savedAt };
  }).filter(function (ev) { return ev.name !== ''; });
}

function eventName_(body) {
  var name = String(body.name == null ? '' : body.name).trim();
  if (name.length > MAX_EVENT_NAME) name = name.slice(0, MAX_EVENT_NAME);
  return name;
}

// 保存済みイベントの設定をいまの設定にする。無ければ false
function loadEvent_(name) {
  var list = events_();
  for (var i = 0; i < list.length; i++) {
    if (list[i].name === name) {
      var ev = list[i], values = {};
      SETTING_KEYS.forEach(function (k) { values[k] = ev[k]; });
      writeSettings_(values);
      return true;
    }
  }
  return false;
}

// comments を archive_<yyyyMMdd>_<名前> シートに写して、comments を空にする
function archiveComments_(name) {
  var ss = SpreadsheetApp.getActiveSpreadsheet();
  var sh = sheet_();
  var moved = Math.max(0, sh.getLastRow() - 1);
  var date = Utilities.formatDate(new Date(), 'Asia/Tokyo', 'yyyyMMdd');
  // シート名に使えない文字を落とす。同名があれば _2, _3 … を付ける
  var base = ('archive_' + date + '_' + name.replace(/[\[\]\*\?\/\\:']/g, '_')).slice(0, 90);
  var title = base, n = 2;
  while (ss.getSheetByName(title)) title = base + '_' + (n++);
  sh.copyTo(ss).setName(title);
  if (moved > 0) sh.deleteRows(2, moved);
  return { sheet: title, moved: moved };
}

function json_(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
function plain_(s) {
  return ContentService.createTextOutput(s)
    .setMimeType(ContentService.MimeType.TEXT);
}

function doGet(e) {
  var action = (e && e.parameter && e.parameter.action) || 'text';

  if (action === 'text') {
    // 実機が15秒ごとに取りにくる口。承認済みだけを新しい順に MAX_TO_DEVICE 件。
    var approved = rows_().filter(function (c) { return c.state === 'approved'; });
    var tail = approved.slice(Math.max(0, approved.length - MAX_TO_DEVICE));
    return plain_(tail.map(function (c) { return c.text; }).join('\n'));
  }

  if (action === 'public') {
    // 参加者画面が見る口。**承認済みだけ**を返す。鍵は要らない。
    // 未確認・見送りは参加者に見せない（見送った言葉が会場の全員に見えてしまうため）。
    var pub = rows_().filter(function (c) { return c.state === 'approved'; });
    return json_({ ok: true, comments: pub.slice(Math.max(0, pub.length - MAX_TO_DEVICE)) });
  }

  if (action === 'list') {
    // 管理画面が見る口。未確認・見送りを含む全件なので鍵で守る。
    // 参加者用URLにはGASのURLが入っているため、鍵なしで開けると誰でも全件読めてしまう。
    var key = adminKey_();
    if (!key || String((e && e.parameter && e.parameter.key) || '') !== key) {
      return json_({ ok: false, error: 'forbidden' });
    }
    return json_({ ok: true, comments: rows_() });
  }

  if (action === 'settings') {
    // 参加者画面が開いたときに読む。公開してよい文言だけなので鍵は要らない
    return json_({ ok: true, settings: settings_() });
  }

  if (action === 'events') {
    if (!isAdmin_(e && e.parameter && e.parameter.key)) return json_({ ok: false, error: 'forbidden' });
    return json_({ ok: true, events: events_() });
  }

  return json_({ ok: false, error: 'unknown action' });
}

function doPost(e) {
  var body;
  try {
    body = JSON.parse(e.postData.contents);
  } catch (err) {
    return json_({ ok: false, error: 'bad json' });
  }

  // 追加と状態変更が同時に来ても行がずれないように直列化する
  var lock = LockService.getScriptLock();
  try {
    lock.waitLock(5000);
  } catch (err) {
    return json_({ ok: false, error: 'busy' });
  }

  try {
    if (body.action === 'add') {
      var text = String(body.text == null ? '' : body.text).trim();
      if (!text) return json_({ ok: false, error: 'empty' });
      if (text.length > MAX_TEXT) text = text.slice(0, MAX_TEXT);

      var list = rows_();
      var id = 1;
      for (var i = 0; i < list.length; i++) if (list[i].id >= id) id = list[i].id + 1;
      sheet_().appendRow([id, new Date().toISOString(), text, 'pending']);
      return json_({ ok: true, id: id });
    }

    if (body.action === 'state') {
      // 承認・見送りは管理者だけ。画面のURLは公開されるので鍵で守る
      var key = adminKey_();
      if (!key || String(body.key || '') !== key) return json_({ ok: false, error: 'forbidden' });

      var state = String(body.state || '');
      if (['pending', 'approved', 'rejected'].indexOf(state) < 0) {
        return json_({ ok: false, error: 'bad state' });
      }
      var all = rows_();
      var idx = -1;
      for (var j = 0; j < all.length; j++) if (all[j].id === Number(body.id)) { idx = j; break; }
      if (idx < 0) return json_({ ok: false, error: 'not found' });

      sheet_().getRange(idx + 2, 4).setValue(state);   // +2 = ヘッダ行 + 1始まり
      return json_({ ok: true });
    }

    // ここから下はすべて管理者だけ
    if (['settings', 'event_save', 'event_load', 'event_start'].indexOf(body.action) >= 0 &&
        !isAdmin_(body.key)) {
      return json_({ ok: false, error: 'forbidden' });
    }

    if (body.action === 'settings') {
      var cleaned = cleanSettings_(body);
      if (cleaned.error) return json_({ ok: false, error: cleaned.error });
      writeSettings_(cleaned.values);
      return json_({ ok: true, settings: settings_() });
    }

    if (body.action === 'event_save') {
      var name = eventName_(body);
      if (!name) return json_({ ok: false, error: 'empty name' });
      var cur = settings_();
      var row = [name, cur.title, cur.lead, cur.color, cur.placeholder, new Date().toISOString()];
      var evs = eventsSheet_();
      var names = events_().map(function (ev) { return ev.name; });
      var at = names.indexOf(name);
      // events_() は空行を飛ばすので、行番号はシートから引き直す
      var last = evs.getLastRow(), target = -1;
      if (at >= 0 && last >= 2) {
        var col = evs.getRange(2, 1, last - 1, 1).getValues();
        for (var r = 0; r < col.length; r++) if (String(col[r][0]) === name) { target = r + 2; break; }
      }
      writeText_(evs.getRange(target > 0 ? target : last + 1, 1, 1, 6), [row]);
      return json_({ ok: true, overwritten: target > 0, events: events_() });
    }

    if (body.action === 'event_load') {
      var lname = eventName_(body);
      if (!loadEvent_(lname)) return json_({ ok: false, error: 'not found' });
      return json_({ ok: true, settings: settings_() });
    }

    if (body.action === 'event_start') {
      var sname = eventName_(body);
      if (!sname) return json_({ ok: false, error: 'empty name' });
      var arc = archiveComments_(sname);
      var loaded = loadEvent_(sname);
      return json_({ ok: true, archived: arc.sheet, moved: arc.moved, loaded: loaded, settings: settings_() });
    }

    return json_({ ok: false, error: 'unknown action' });
  } finally {
    lock.releaseLock();
  }
}
