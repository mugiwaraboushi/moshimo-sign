# 対応済みフィードバック

## 2026-08-07 / mugi21 / 虹スクロール(1文字ずつ色が変わる)を実機で流したい
- 状態: **実装済み（`colorScroll:"rainbow"`）** — 2026-08-07 ファームウェア v0.4 / OTA反映済み
- 原文:
  > ↑実機で流してほしいです （`prototypes/rainbow/` を指して）
- 文脈: Discordでの依頼。Webプロトタイプ `prototypes/rainbow/` は実装済み
  （mode=char / flow / wave の3種）。
- 対応内容:
  - ファームウェアに `drawTextRainbow()` を追加。1文字ごとに色相を24°ずつ進め、
    基準色相は約7.2秒で一周する。下段スクロールのみが対象（上段 `colorTop` は対象外）。
  - `playlist.json` は既存 `colorScroll` の特殊値 `"rainbow"` で指定する。
    当初案の `colorMode` + `rainbowCycle` / `rainbowSpin` は採用せず、
    **仕様の追加面積を最小にする**ため既存フィールドの特殊値とした。
  - 周期・回転速度はv0.4では固定。細かい調整はビューア `prototypes/rainbow/` で行う。
- 仕様: `docs/playlist-spec.md`「レインボースクロール (v0.4〜)」
- 補足: 予告どおりパネルはフルカラーなのでハードの制約ではなく、ソフトのみで実現できた。
  暫定対応だった2色化（黄×水）はこれで役目を終えた。

## 2026-10-24 / wenqi021124 / 塗り分けたさんまの絵を実機に流したい
- 状態: **対応済み** — 2026-10-24 ファームウェア v21 (複数色) を公開
- 原文:
  > これを流してみて〜 (prototypes/draw-color/ の3色の絵のURL)
- 文脈: Discordでの依頼。今の実機 (v19) は layers を読まないので、流すと青1色になる。
  別の方が v20 のテストを dual 表示で実施中だったため、確認したうえで「B: 塗り分け対応ファームの公開まで待って3色で流す」に決まった (@mugi21 の返答)。
- 用意済み: `data/frames/sanma-color.json` (下地 3B7BFF + layers FFFFFF / 27E0FF)。
  受け取ったURLの白の層が344文字だった (末尾に0のバイトが2つ余分) ので342文字に詰めてある。絵は変わらない。
- やること: 塗り分け対応版 (feat/multicolor) が公開されたら、`mode:"frames"`、`frames` をこのファイルの中身にして流す。
  その前に v20 テストの片付け (sayEnabled / eggWords / eggFrames / interlude を戻す) が済んでいるか確かめる。
- 対応内容: v21 で `frames[].layers` (塗り分け) に対応。いまは通常表示 (`mode:"dual"`) のまま、
  文字が1周するたびにこの絵を下半分で右から左へ泳がせている (`interlude`、`data/frames/sanma-color-interlude.json`)。
  全画面で泳ぐ版は `data/frames/sanma-color-swim.json`、止まった1枚は `data/frames/sanma-color.json`。
