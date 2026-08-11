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
    pending = { x0,y0,x1,y1, pts, ops: args[0].join(",") };
  }
  else if ((fn === OPS.fill || fn === OPS.eoFill) && pending){
    const { x0,y0,x1,y1,pts,ops } = pending;
    const key = ops + "|" + pts.map((v,idx) => (Math.round((v - (idx%2 ? y0 : x0))*2)/2).toFixed(1)).join(",");
    shapes.push({ x:+x0.toFixed(1), y:+y0.toFixed(1), w:+(x1-x0).toFixed(1), h:+(y1-y0).toFixed(1), c:color, key });
    pending = null;
  }
}
const glyphs = shapes.filter(s => s.w < 30 && s.h < 30 && s.w > 1 && s.h > 3);
const byKey = new Map();
for (const g of glyphs) byKey.set(g.key, (byKey.get(g.key) || 0) + 1);
console.log(file, "| 全fill:", shapes.length, "| 文字らしいもの:", glyphs.length, "| 形の種類:", byKey.size);
console.log("上位20種:", [...byKey.entries()].sort((a,b)=>b[1]-a[1]).slice(0,20).map(([,n],i)=>n).join(" "));
console.log("高さの分布:", [...new Set(glyphs.map(g=>Math.round(g.h)))].sort((a,b)=>a-b).join(","));
writeFileSync(file.replace(/\.pdf$/, ".shapes.json"), JSON.stringify(glyphs));
