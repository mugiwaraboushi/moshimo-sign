// 形の同じパス＝同じ数字、という前提で時刻表を組み立てる。
// 数字と形の対応は samples.html で目視確認したもの（tt/digits.json, tt/digits-hour.json）。
import { readFileSync, writeFileSync } from "node:fs";
const file = process.argv[2];
const shapes = JSON.parse(readFileSync(file.replace(/\.pdf$/, ".shapes.json")));
const mapMin  = JSON.parse(readFileSync("tt/digits.json"));
const mapHour = JSON.parse(readFileSync("tt/digits-hour.json"));

const isRed = c => { const [r,g,b] = c.split(",").map(Number); return r > 170 && g < 130 && b < 130; };
const mins  = shapes.filter(s => mapMin[s.key] && s.h >= 14.5 && s.h <= 18.5).map(s => ({ ...s, d: mapMin[s.key] }));
const hrs   = shapes.filter(s => mapHour[s.key] && s.h > 20 && s.h < 32).map(s => ({ ...s, d: mapHour[s.key] }));

// 左右の表に割る: x のまんなか付近でいちばん広い隙間を探す
const xs = [...new Set(mins.map(d => Math.round(d.x)))].sort((a,b)=>a-b);
const lo = xs[0] + (xs[xs.length-1]-xs[0])*0.3, hi = xs[0] + (xs[xs.length-1]-xs[0])*0.7;
let split = (lo+hi)/2, best = 0;
for (let i=1;i<xs.length;i++){
  const mid = (xs[i]+xs[i-1])/2;
  if (mid < lo || mid > hi) continue;
  if (xs[i]-xs[i-1] > best){ best = xs[i]-xs[i-1]; split = mid; }
}
// y でまとめて行にする
function rowsOf(list, tol){
  const rows = [];
  for (const g of [...list].sort((a,b)=>a.y-b.y)){
    const r = rows.find(r => Math.abs(r.y - g.y) < tol);
    if (r){ r.items.push(g); } else rows.push({ y: g.y, items: [g] });
  }
  return rows;
}
// 横に近い数字どうしをつないで数にする（数字の間隔は約12.5、数と数の間はもっと広い）
function numbersOf(items, maxGap = 18){
  const s = [...items].sort((a,b)=>a.x-b.x), out = [];
  let cur = null;
  for (const g of s){
    if (cur && g.x - cur.lastX < maxGap){ cur.txt += g.d; cur.lastX = g.x; cur.red = cur.red || isRed(g.c); }
    else { if (cur) out.push(cur); cur = { txt: g.d, x: g.x, lastX: g.x, red: isRed(g.c) }; }
  }
  if (cur) out.push(cur);
  return out;
}
// 表の本体だけを見る。右端の凡例「時刻表の見方」は本体から離れているので、
// 左から見ていって大きく空いたところで切る。
function bodyRange(list){
  const xs = [...list].map(d => d.x).sort((a,b)=>a-b);
  let end = xs[xs.length-1];
  for (let i=1;i<xs.length;i++) if (xs[i] - xs[i-1] > 120){ end = xs[i-1]; break; }
  return [xs[0], end + 30];
}
const tables = [];
for (const side of [0, 1]){
  const half = d => side ? d.x >= split : d.x < split;
  const [bx0, bx1] = bodyRange(mins.filter(half));
  const m = mins.filter(d => half(d) && d.x >= bx0 && d.x <= bx1);
  const hx0 = Math.min(...hrs.filter(half).map(d => d.x));
  const h = hrs.filter(d => half(d) && d.x < hx0 + 45);      // 時台の欄は左端だけ
  const hourRows = rowsOf(h, 14).map(r => ({ y: r.y, txt: numbersOf(r.items, 34)[0].txt }));
  const rows = rowsOf(m, 7).map(r => {
    const hr = hourRows.filter(hh => Math.abs(hh.y - r.y) < 22).sort((a,b)=>Math.abs(a.y-r.y)-Math.abs(b.y-r.y))[0];
    return { y: r.y, hour: hr ? hr.txt : null, mins: numbersOf(r.items) };
  });
  tables.push(rows);
}
const label = ["平日", "土休日"];
const problems = [];
tables.forEach((rows, ti) => {
  console.log("--- " + label[ti] + " (" + rows.length + "行) ---");
  rows.forEach(r => {
    const bad = [];
    if (r.hour === null || !/^\d{1,2}$/.test(r.hour) || +r.hour > 24) bad.push("時台?");
    const vals = r.mins.map(n => +n.txt);
    if (r.mins.some(n => !/^\d{1,2}$/.test(n.txt))) bad.push("分の桁数?");
    if (vals.some(v => v > 59)) bad.push(">59");
    for (let i=1;i<vals.length;i++) if (vals[i] <= vals[i-1]) bad.push("順序:" + vals[i-1] + "→" + vals[i]);
    if (bad.length) problems.push(label[ti] + " " + r.hour + "時: " + bad.join(" "));
    console.log(String(r.hour).padStart(2) + ": " + r.mins.map(n => n.txt + (n.red ? "*" : "")).join(" "));
  });
});
console.log(problems.length ? "⚠ 要確認:\n  " + problems.join("\n  ") : "✓ 時台の連番・分の昇順ともに矛盾なし");
writeFileSync(file.replace(/\.pdf$/, ".rows.json"), JSON.stringify(tables.map(rows =>
  rows.map(r => ({ hour: r.hour, mins: r.mins.map(n => ({ m: +n.txt, express: n.red })) })))));
