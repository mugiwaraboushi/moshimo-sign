// 公共交通オープンデータセンター(ODPT)から大岡山の駅時刻表を取ってきて
// data/timetable-ookayama.json に保存する。
// GitHub Actions (.github/workflows/timetable.yml) から1日1回実行される。
//
// 手元で試すとき:
//   ODPT_CONSUMER_KEY=あなたのキー node scripts/fetch-timetable.mjs
//   （チャレンジ用APIを使う場合は ODPT_API_BASE=https://api-challenge.odpt.org/api/v4 も付ける）
//
// 駅や種別のIDは決め打ちにせず、APIから引いた名前で照合する。
// ダイヤ改正やID変更で黙って壊れないように、見つからなければエラーで止める。
import { writeFileSync, mkdirSync } from "node:fs";

const KEY = process.env.ODPT_CONSUMER_KEY;
const BASE = process.env.ODPT_API_BASE || "https://api.odpt.org/api/v4";
const OUT = "data/timetable-ookayama.json";

// 大岡山に来る2路線。name は掲示板に出す表記、railway はODPTの路線ID。
const TARGETS = [
  { name: "目黒線",   railway: "odpt.Railway:Tokyu.Meguro" },
  { name: "大井町線", railway: "odpt.Railway:Tokyu.Oimachi" },
];
const STATION_TITLE = "大岡山";

if (!KEY) {
  console.log("ODPT_CONSUMER_KEY が設定されていません。何もせずに終わります。");
  console.log("（開発者登録 → キーをGitHubのSecretsに ODPT_CONSUMER_KEY として登録してください）");
  process.exit(0);
}

async function api(path, params = {}) {
  const url = new URL(BASE + "/" + path);
  for (const [k, v] of Object.entries(params)) url.searchParams.set(k, v);
  url.searchParams.set("acl:consumerKey", KEY);
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 30000);
  try {
    const res = await fetch(url, { signal: ctrl.signal, headers: { "accept": "application/json" } });
    if (!res.ok) throw new Error(`${path} が ${res.status} ${res.statusText}`);
    return await res.json();
  } finally { clearTimeout(timer); }
}

// ID → 日本語表記 の対応表を作る（dc:title が無いデータもあるので odpt:*Title.ja も見る）
const titleOf = o => o["dc:title"] || o["odpt:stationTitle"]?.ja || o["odpt:railDirectionTitle"]?.ja
                  || o["odpt:trainTypeTitle"]?.ja || o["owl:sameAs"];
const indexById = list => Object.fromEntries(list.map(o => [o["owl:sameAs"], titleOf(o)]));

const HHMM = /^([01]?\d|2[0-9]):([0-5]\d)$/;

async function main() {
  const [stations, directions, trainTypes] = await Promise.all([
    api("odpt:Station", { "odpt:operator": "odpt.Operator:Tokyu" }),
    api("odpt:RailDirection"),
    api("odpt:TrainType", { "odpt:operator": "odpt.Operator:Tokyu" }),
  ]);
  const stationName = indexById(stations);
  const dirName = indexById(directions);
  const typeName = indexById(trainTypes);

  const lines = [];
  for (const target of TARGETS) {
    const st = stations.find(s => s["odpt:railway"] === target.railway && titleOf(s) === STATION_TITLE);
    if (!st) throw new Error(`${target.name} の${STATION_TITLE}駅が見つかりません（railway=${target.railway}）`);

    const tables = await api("odpt:StationTimetable", { "odpt:station": st["owl:sameAs"] });
    if (!tables.length) {
      throw new Error(`${target.name} の駅時刻表が空でした。無料APIに東急の時刻表が含まれていない可能性があります` +
                      `（ODPT_API_BASE をチャレンジ用に変えて試してください）`);
    }

    // 方面（上り／下り）ごとにまとめ、平日と土休日を分けて持つ
    const byDir = new Map();
    for (const tt of tables) {
      const dirId = tt["odpt:railDirection"];
      const cal = String(tt["odpt:calendar"] || "");
      const slot = cal.includes("SaturdayHoliday") || cal.includes("Holiday") ? "holiday"
                 : cal.includes("Weekday") ? "weekday" : null;
      if (!slot) continue;                                  // 特定日ダイヤなどは使わない

      const trains = (tt["odpt:stationTimetableObject"] || []).map(o => {
        const t = o["odpt:departureTime"];
        if (!t || !HHMM.test(t)) return null;
        const destIds = o["odpt:destinationStation"] || [];
        return {
          t,
          type: typeName[o["odpt:trainType"]] || "",
          dest: destIds.map(id => stationName[id]).filter(Boolean).join("・"),
        };
      }).filter(Boolean).sort((a, b) => a.t.localeCompare(b.t));
      if (!trains.length) continue;

      const key = dirId || "unknown";
      if (!byDir.has(key)) byDir.set(key, { to: dirName[dirId] || "", weekday: [], holiday: [] });
      byDir.get(key)[slot].push(...trains);
    }
    if (!byDir.size) throw new Error(`${target.name} で平日・土休日の時刻表が取れませんでした`);

    lines.push({ name: target.name, dirs: [...byDir.values()] });
  }

  const out = {
    updated: new Date().toISOString(),
    station: STATION_TITLE,
    source: "公共交通オープンデータセンター（ODPT） https://www.odpt.org/",
    note: "このデータは公共交通オープンデータセンターにおいて提供されるものです。" +
          "データの正確性・完全性は保証されません。運行情報は各事業者の公式情報を確認してください。",
    lines,
  };
  const counts = lines.map(l => `${l.name} ${l.dirs.map(d => `${d.to || "?"}:平日${d.weekday.length}/土休日${d.holiday.length}`).join(" ")}`);
  mkdirSync("data", { recursive: true });
  writeFileSync(OUT, JSON.stringify(out, null, 1) + "\n");
  console.log("書き出しました:", OUT);
  console.log(counts.join("\n"));
}

main().catch(e => { console.error("失敗:", e.message); process.exit(1); });
