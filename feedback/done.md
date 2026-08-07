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
