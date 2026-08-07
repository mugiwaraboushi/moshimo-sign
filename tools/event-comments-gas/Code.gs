/**
 * もしも電光掲示板 — イベントコメントの置き場 (Google Apps Script)
 *
 * スプレッドシートを保存先にして、4つの口を開ける:
 *   GET  ?action=list                     コメント一覧をJSONで返す（管理画面用）
 *   GET  ?action=text                     承認済みだけを1行1件のテキストで返す
 *                                         ← このURLを firmware の COMMENTS_URL にする
 *   POST {"action":"add","text":"..."}    1件足す（参加者の投稿）
 *   POST {"action":"state","id":1,"state":"approved","key":"..."}
 *                                         状態を変える（管理者のみ）
 *
 * 画面: prototypes/event-comments/
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

  if (action === 'list') {
    return json_({ ok: true, comments: rows_() });
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

    return json_({ ok: false, error: 'unknown action' });
  } finally {
    lock.releaseLock();
  }
}
