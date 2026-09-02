// 営業カレンダー (data/hours.json) を見て、playlist.json の topText を
// OPEN / CLOSED に切り替える。GitHub Actions (.github/workflows/open-closed.yml) から
// 定期実行される。判定はすべて日本時間。
//
// 大事な約束: topText が OPEN / CLOSED **以外** のときは何もしない。
// イベント中などで手動で別の文言 (例「イベント中」) を入れているあいだは自動更新が黙る。
// 自動に戻したくなったら topText を OPEN か CLOSED に書き戻せばよい。
import { readFileSync, writeFileSync } from "node:fs";

const HOURS = "data/hours.json";
const PLAYLIST = "playlist.json";

const hours = JSON.parse(readFileSync(HOURS, "utf8"));
const tz = hours.tz || "Asia/Tokyo";
const openText = hours.openText || "OPEN";
const closedText = hours.closedText || "CLOSED";

// --- いまの日本時間を取り出す -------------------------------------------
const parts = Object.fromEntries(
  new Intl.DateTimeFormat("en-US", {
    timeZone: tz,
    year: "numeric", month: "2-digit", day: "2-digit",
    hour: "2-digit", minute: "2-digit", hour12: false,
    weekday: "short",
  }).formatToParts(new Date()).map(p => [p.type, p.value])
);
const dateKey = `${parts.year}-${parts.month}-${parts.day}`;
const dow = parts.weekday.toLowerCase();            // "sun" … "sat"
const nowMin = (Number(parts.hour) % 24) * 60 + Number(parts.minute);

// --- 今日の営業時間を決める ---------------------------------------------
// dates に日付キーがあれば (値が null でも) それが優先。無ければ曜日の既定。
const spec = Object.prototype.hasOwnProperty.call(hours.dates ?? {}, dateKey)
  ? hours.dates[dateKey]
  : (hours.weekly ?? {})[dow];

const toMin = hhmm => {
  const m = /^(\d{1,2}):(\d{2})$/.exec(hhmm.trim());
  if (!m) throw new Error(`時刻の書き方が不正です: "${hhmm}"`);
  return Number(m[1]) * 60 + Number(m[2]);
};
const ranges = (spec ?? "")
  .split(",")
  .map(s => s.trim())
  .filter(Boolean)
  .map(s => {
    const [from, to] = s.split("-");
    if (!from || !to) throw new Error(`時間帯の書き方が不正です: "${s}"`);
    return [toMin(from), toMin(to)];
  });

// 開店時刻は含み、閉店時刻は含まない (19:00 ちょうどは CLOSED)
const isOpen = ranges.some(([from, to]) => nowMin >= from && nowMin < to);
const want = isOpen ? openText : closedText;

// --- playlist.json を更新する -------------------------------------------
const raw = readFileSync(PLAYLIST, "utf8");
const playlist = JSON.parse(raw);
const current = playlist.topText;

const managed = [openText, closedText];
if (!managed.includes(current)) {
  console.log(`topText は "${current}" (手動設定) なので触りません。`);
  process.exit(0);
}
if (current === want) {
  console.log(`${dateKey} ${parts.hour}:${parts.minute} JST — topText は "${current}" のままで正しい。`);
  process.exit(0);
}

playlist.topText = want;
const out = JSON.stringify(playlist, null, 2) + "\n";
JSON.parse(out);                                    // 書く前に壊れていないか確かめる
writeFileSync(PLAYLIST, out);
console.log(`${dateKey} ${parts.hour}:${parts.minute} JST — topText を "${current}" → "${want}" に変更しました。`);
