// 公開RSSから見出しを取ってきて data/news.json に保存する。
// GitHub Actions (.github/workflows/news.yml) から30分ごとに実行される。
// ブラウザから直接RSSを読むとCORSで弾かれるため、ここで取って同一オリジンのJSONにしておく。
import { writeFileSync, mkdirSync, readFileSync, existsSync } from "node:fs";

const FEEDS = [
  { id: "nhk0",  label: "NHK 主要",       src: "NHK",    url: "https://www.nhk.or.jp/rss/news/cat0.xml" },
  { id: "nhk1",  label: "NHK 社会",       src: "NHK",    url: "https://www.nhk.or.jp/rss/news/cat1.xml" },
  { id: "nhk3",  label: "NHK 科学・文化", src: "NHK",    url: "https://www.nhk.or.jp/rss/news/cat3.xml" },
  { id: "nhk6",  label: "NHK 国際",       src: "NHK",    url: "https://www.nhk.or.jp/rss/news/cat6.xml" },
  { id: "yahoo", label: "Yahoo!トピックス", src: "Yahoo!", url: "https://news.yahoo.co.jp/rss/topics/top-picks.xml" },
];
const MAX_ITEMS = 15;
const OUT = "data/news.json";

const unescapeXml = s => s
  .replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, "$1")
  .replace(/&lt;/g, "<").replace(/&gt;/g, ">")
  .replace(/&quot;/g, '"').replace(/&#0?39;|&apos;/g, "'")
  .replace(/&amp;/g, "&")
  .replace(/\s+/g, " ")
  .trim();

const tag = (block, name) => {
  const m = block.match(new RegExp(`<${name}[^>]*>([\\s\\S]*?)</${name}>`));
  return m ? unescapeXml(m[1]) : "";
};

function parseItems(xml) {
  const blocks = [...xml.matchAll(/<(item|entry)\b[\s\S]*?<\/\1>/g)].map(m => m[0]);
  return blocks.map(b => ({
    title: tag(b, "title"),
    link: tag(b, "link") || (b.match(/<link[^>]*href="([^"]+)"/)?.[1] ?? ""),
    pubDate: tag(b, "pubDate") || tag(b, "updated") || tag(b, "published"),
  })).filter(it => it.title);
}

async function fetchFeed(feed) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 20000);
  try {
    const res = await fetch(feed.url, {
      signal: ctrl.signal,
      headers: { "user-agent": "moshimo-sign/1.0 (+https://github.com/mugiwaraboushi/moshimo-sign)" },
    });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const items = parseItems(await res.text()).slice(0, MAX_ITEMS);
    if (!items.length) throw new Error("見出しが取れませんでした");
    return items;
  } finally {
    clearTimeout(timer);
  }
}

// 取得に失敗したフィードは前回の内容を残す（全滅させない）
let prev = { feeds: {} };
if (existsSync(OUT)) {
  try { prev = JSON.parse(readFileSync(OUT, "utf8")); } catch { /* 壊れていたら無視 */ }
}

const feeds = {};
let ok = 0;
for (const f of FEEDS) {
  try {
    const items = await fetchFeed(f);
    feeds[f.id] = { label: f.label, src: f.src, url: f.url, items };
    ok++;
    console.log(`OK   ${f.id} ${items.length}件  ${items[0].title.slice(0, 30)}`);
  } catch (e) {
    const old = prev.feeds?.[f.id];
    feeds[f.id] = old
      ? { ...old, stale: true }
      : { label: f.label, src: f.src, url: f.url, items: [] };
    console.log(`NG   ${f.id} ${e.message}${old ? "（前回分を維持）" : ""}`);
  }
}

if (!ok) {
  console.error("すべてのフィードが取得できませんでした。前回のJSONを残して終了します。");
  process.exit(1);
}

mkdirSync("data", { recursive: true });
writeFileSync(OUT, JSON.stringify({
  updatedAt: new Date().toISOString(),
  note: "公開RSSの見出し。scripts/fetch-news.mjs が自動生成（手で編集しない）",
  feeds,
}, null, 1) + "\n", "utf8");
console.log(`書き出し: ${OUT}（${ok}/${FEEDS.length} フィード成功）`);
