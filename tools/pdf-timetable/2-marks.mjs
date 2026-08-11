// 行先の記号（かな・漢字）をまとめる。
// 字ごとに点の位置がわずかに違うため、厳密一致ではなく「形の指紋」の近さでまとめる。
import { readFileSync, writeFileSync } from "node:fs";
const file = process.argv[2];
const shapes = JSON.parse(readFileSync(file.replace(/\.pdf$/, ".shapes.json")));
const marks = shapes.filter(s => s.h > 3 && s.h < 12 && s.w < 12 && s.y > 140 && s.y < 1000 && s.x > 60 && s.x < 1750);
const cos = (a, b) => { let s = 0; for (let i=0;i<a.length;i++) s += a[i]*b[i];
                        return s / (Math.hypot(...a) * Math.hypot(...b) || 1); };
const clusters = [];
for (const m of marks){
  let best = null, bestScore = 0;
  for (const c of clusters){ const s = cos(m.g, c.center); if (s > bestScore){ bestScore = s; best = c; } }
  if (best && bestScore > 0.90){ best.items.push(m); best.center = best.center.map((v,i) => (v*(best.items.length-1) + m.g[i]) / best.items.length); }
  else clusters.push({ center: m.g.slice(), items: [m] });
}
clusters.sort((a,b) => b.items.length - a.items.length);
writeFileSync(file.replace(/\.pdf$/, ".marks.json"), JSON.stringify(
  clusters.map((c,i) => ({ i, n: c.items.length, items: c.items.map(m => ({ x:m.x, y:m.y, w:m.w, h:m.h })) }))));
writeFileSync(file.replace(/\.pdf$/, ".marksample.json"), JSON.stringify(
  clusters.map((c,i) => ({ i, n: c.items.length, x:c.items[0].x, y:c.items[0].y, w:c.items[0].w, h:c.items[0].h }))));
console.log(file, "記号", marks.length, "個 →", clusters.length, "種:", clusters.map((c,i)=>i+":"+c.items.length).join(" "));
