import { getDocument, OPS } from "pdfjs-dist/legacy/build/pdf.mjs";
import { readFileSync, writeFileSync } from "node:fs";
const mul = (a,b) => [ a[0]*b[0]+a[2]*b[1], a[1]*b[0]+a[3]*b[1], a[0]*b[2]+a[2]*b[3],
                       a[1]*b[2]+a[3]*b[3], a[0]*b[4]+a[2]*b[5]+a[4], a[1]*b[4]+a[3]*b[5]+a[5] ];
const apply = (m,x,y) => [ m[0]*x + m[2]*y + m[4], m[1]*x + m[3]*y + m[5] ];

const file = process.argv[2];
const doc = await getDocument({ data: new Uint8Array(readFileSync(file)),
  cMapUrl: "./node_modules/pdfjs-dist/cmaps/", cMapPacked: true }).promise;
const page = await doc.getPage(1);
const vp = page.getViewport({ scale: 1 });
const ol = await page.getOperatorList();

let ctm = vp.transform.slice();          // PDF座標 → 画面座標
const stack = [];
let color = "#000", pending = null;
const shapes = [];
for (let i = 0; i < ol.fnArray.length; i++){
  const fn = ol.fnArray[i], args = ol.argsArray[i];
  if (fn === OPS.save) stack.push(ctm.slice());
  else if (fn === OPS.restore) ctm = stack.pop() || ctm;
  else if (fn === OPS.transform) ctm = mul(ctm, args);
  else if (fn === OPS.setFillRGBColor) color = args.slice(0,3).join(",");
  else if (fn === OPS.constructPath){
    const coords = args[1];
    let x0=1e9, y0=1e9, x1=-1e9, y1=-1e9;
    const pts = [];
    for (let k=0; k+1 < coords.length; k+=2){
      const [X,Y] = apply(ctm, coords[k], coords[k+1]);
      pts.push(X, Y);
      if (X<x0) x0=X; if (X>x1) x1=X; if (Y<y0) y0=Y; if (Y>y1) y1=Y;
    }
    // 座標配列に矩形(rectangle)が混ざっていると「幅・高さ」を点と誤読して枠が壊れる。
    // その場合だけ pdf.js が持っている minMax を使う（数字は矩形を含まないので従来どおり）。
    const mm = args[2];
    let bx0, by0, bx1, by1;
    if (args[0].includes(OPS.rectangle) && mm && mm.length === 4){
      const c1 = apply(ctm, mm[0], mm[1]), c2 = apply(ctm, mm[2], mm[3]);
      bx0 = Math.min(c1[0], c2[0]); bx1 = Math.max(c1[0], c2[0]);
      by0 = Math.min(c1[1], c2[1]); by1 = Math.max(c1[1], c2[1]);
    } else { bx0 = x0; by0 = y0; bx1 = x1; by1 = y1; }
    pending = { x0, y0, bx0, by0, bx1, by1, pts, ops: args[0].join(",") };
  }
  else if ((fn === OPS.fill || fn === OPS.eoFill) && pending){
    const { x0,y0,bx0,by0,bx1,by1,pts,ops } = pending;
    // 形のキーは今までどおり座標から作る（数字の対応表を作り直さなくて済むように）
    const key = ops + "|" + pts.map((v,idx) => (Math.round((v - (idx%2 ? y0 : x0))*2)/2).toFixed(1)).join(",");
    // 記号（かな・漢字）は同じ字でも座標がわずかにずれるので、
    // 枠で正規化して粗く量子化したキーも持つ（こちらは位置と大きさに依存しない）
    const w = Math.max(bx1-bx0, 0.01), h = Math.max(by1-by0, 0.01);
    // 8×8のマスに点を落とした密度。字の形をざっくり表す指紋で、多少座標がぶれても似た値になる。
    const G = 8, grid = new Array(G*G).fill(0);
    for (let k=0; k+1 < pts.length; k+=2){
      const gx = Math.min(G-1, Math.max(0, Math.floor((pts[k]   - bx0) / w * G)));
      const gy = Math.min(G-1, Math.max(0, Math.floor((pts[k+1] - by0) / h * G)));
      grid[gy*G + gx]++;
    }
    const norm = Math.hypot(...grid) || 1;
    shapes.push({ x:+bx0.toFixed(1), y:+by0.toFixed(1), w:+w.toFixed(1), h:+h.toFixed(1), c:color, key,
                  g: grid.map(v => Math.round(v/norm*100)) });
    pending = null;
  }
}
const glyphs = shapes.filter(s => s.w < 40 && s.h < 40);
const byKey = new Map();
for (const g of glyphs) byKey.set(g.key, (byKey.get(g.key) || 0) + 1);
console.log(file, "| 全fill:", shapes.length, "| 文字らしいもの:", glyphs.length, "| 形の種類:", byKey.size);
console.log("上位20種:", [...byKey.entries()].sort((a,b)=>b[1]-a[1]).slice(0,20).map(([,n],i)=>n).join(" "));
console.log("高さの分布:", [...new Set(glyphs.map(g=>Math.round(g.h)))].sort((a,b)=>a-b).join(","));
writeFileSync(file.replace(/\.pdf$/, ".shapes.json"), JSON.stringify(glyphs));
