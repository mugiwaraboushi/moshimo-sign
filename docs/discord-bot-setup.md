# Discord ↔ Claude Code 連携セットアップ手順

「Discordのチャンネルに構想を投げると、Claudeがプロトタイプを作ってURLで返答する」仕組みの構築手順。
Anthropic公式のDiscord連携(Channels機能、2026年3月リリース)を使う。追加費用なし(Claudeサブスクリプションの範囲)。

> 参考: [公式ドキュメント Channels](https://code.claude.com/docs/en/channels) / [Channels reference](https://code.claude.com/docs/en/channels-reference)

## 全体像

```
メンバー: Discordチャンネルに投稿
   ↓
自分のPC: Claude Code (Discordプラグインで常駐)
   ↓ プロトタイプ作成 → git push
GitHub Pages: 自動で公開
   ↓
Claude CodeがDiscordにURLを返答
```

- PCでClaude Codeを起動している間だけ応答する (将来は常時稼働サーバへ移行可能)。
- **フォーラムチャンネルは公式ドキュメントに記載がない**。テキストチャンネル+スレッドは対応確認済みなので、まず専用テキストチャンネル(例: `#掲示板ラボ`)で運用し、話題ごとにスレッドを立てるのがフォーラムに近い使用感でおすすめ。フォーラムチャンネルでも動くかは試してみる価値あり。

## まずは自分だけのサーバーでテストする (推奨)

いきなりもしものサーバーに入れず、テスト用サーバーで一通り動きを確かめる:

1. Discordで自分だけの新規サーバーを作る (サーバー追加 → オリジナルの作成 → 自分と友達のため)
2. 以下の手順1〜4を、**招待先をテストサーバーにして**実施する
3. テストサーバーのチャンネルで実際に依頼を投げてみて、応答・プロトタイプ作成・URL返答まで確認する
4. 運用イメージが固まったら、手順1-5の招待URLをもう一度開いて**もしものサーバーにも招待**すればよい
   (Botは複数サーバーに同時参加できる。Claude Code側の設定はそのまま使える)

テスト中に確認しておくとよいこと:
- 返答の速さ・文体は運用に耐えるか (CLAUDE.mdの指示で調整できる)
- スレッドでの会話が続くか
- allowlist外のアカウント(サブ垢があれば)からのメッセージが無視されるか

## 手順1: Discord側 (Botの作成) — 約10分

1. [Discord Developer Portal](https://discord.com/developers/applications) → **New Application** → 名前(例: もしもラボ)
2. **Bot** セクション → **Reset Token** → トークンをコピー(一度しか表示されない。パスワード同様に扱う)
3. 同じBotページの **Privileged Gateway Intents** → **Message Content Intent** をON
4. **OAuth2 → URL Generator** → スコープ `bot` にチェック → Bot Permissionsで以下をON:
   - View Channels / Send Messages / Send Messages in Threads / Read Message History / Attach Files / Add Reactions
5. 生成されたURLを開き、もしものサーバーにBotを招待

## 手順2: Claude Code側 (PC) — 約10分

Claude Code内で:

```
/plugin marketplace add anthropics/claude-plugins-official
/plugin install discord@claude-plugins-official
/discord:configure <手順1でコピーしたBotトークン>
```

その後、このリポジトリのフォルダで再起動:

```bash
claude --channels plugin:discord@claude-plugins-official
```

## 手順3: ペアリングと安全設定

1. Discordで自分のBotにDMを送る → ペアリングコードが表示される
2. Claude Code側で: `/discord:access pair <コード>`
3. **重要**: `/discord:access policy allowlist` を設定
   (許可した送信者のメッセージだけを受け付ける。公開サーバーでの乗っ取り防止に必須)
4. メンバーを追加したい場合は、各メンバーにも同様にペアリングしてもらう

## 手順4: 常駐化 (PCを閉じない運用)

```bash
# ターミナルを閉じても動かし続ける場合
nohup claude --channels plugin:discord@claude-plugins-official &

# 前回の続きから再開する場合
claude --resume --channels plugin:discord@claude-plugins-official
```

離席中に権限確認で止まらないよう、リポジトリの `.claude/settings.json` で
git push 等の必要コマンドを事前許可しておく(このリポジトリに設定済み)。

## 運用ルール(推奨)

- 応答させるのは専用チャンネルだけに絞る(Botの権限をチャンネル単位で制限)
- allowlist に入れるのは信頼できるメンバーのみ。「見るのは全員、話しかけるのは登録メンバー」から始める
- Botが作ったものはすべてGitHubに履歴が残るので、何かあってもrevertできる

## 将来: 24時間365日化

PCを閉じても応答できるようにするには、同じ仕組みを常時稼働マシンに載せ替える:

- 選択肢A: 小型VPS (月500〜1000円程度、経費化しやすい) にClaude Codeを入れて同じ設定
- 選択肢B: 研究室・施設の常時稼働PC/Raspberry Pi
- 選択肢C: Claude API + Agent SDKで専用Botを自作(応答設計の自由度最大、従量課金)

まずは自分のPCで運用感を確かめてから移行するのがおすすめ。設定はそのまま持ち運べる。
