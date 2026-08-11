import { readFileSync, writeFileSync } from "node:fs";
const SRC = [
  { file: "tt/mg06_1.rows.json", line: "目黒線",   to: "目黒" },
  { file: "tt/mg06_2.rows.json", line: "目黒線",   to: "日吉" },
  { file: "tt/om08_1.rows.json", line: "大井町線", to: "大井町" },
  { file: "tt/om08_2.rows.json", line: "大井町線", to: "溝の口" },
];
const pad = n => String(n).padStart(2, "0");
const lines = new Map();
for (const s of SRC){
  const [weekdayRows, holidayRows] = JSON.parse(readFileSync(s.file));
  const conv = rows => rows.flatMap(r => r.mins.map(m => ({
    t: pad(+r.hour) + ":" + pad(m.m), type: m.express ? "急行" : "各停", dest: m.dest || "" })));
  if (!lines.has(s.line)) lines.set(s.line, { name: s.line, dirs: [] });
  lines.get(s.line).dirs.push({ to: s.to, weekday: conv(weekdayRows), holiday: conv(holidayRows) });
}
const out = {
  updated: process.argv[2],
  station: "大岡山",
  source: "東急電鉄「標準時刻表」PDF（2026年3月14日改正）を読み取ったもの",
  note: "種別は時刻の色で判定（赤=急行、それ以外=各停）。行先は時刻の上の記号を読み取ったもの。" +
        "遅延・運休は反映されない。正確な情報は東急の公式案内を確認すること。",
  lines: [...lines.values()],
};
writeFileSync("tt/timetable-ookayama.json", JSON.stringify(out, null, 1) + "\n");
for (const l of out.lines) for (const d of l.dirs)
  console.log(l.name, d.to + "方面:", "平日", d.weekday.length + "本", "(急行" + d.weekday.filter(t=>t.type==="急行").length + ")",
              "土休日", d.holiday.length + "本", "(急行" + d.holiday.filter(t=>t.type==="急行").length + ")",
              "始発", d.weekday[0].t, "終電", d.weekday[d.weekday.length-1].t);
