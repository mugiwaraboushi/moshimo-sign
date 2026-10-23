# イベントコメントの置き場をつくる (GAS)

イベント中に参加者のスマホから届いたコメントを、管理者が選別して電光掲示板に流すための
サーバ側の作り方。コードは [`tools/event-comments-gas/Code.gs`](../tools/event-comments-gas/Code.gs)。

画面は2つに分かれている:

| | URL | 使う人 |
|---|---|---|
| 参加者用 | [`prototypes/event-comments/`](../prototypes/event-comments/)`?gas=<デプロイID>` | QRから開いて1件送るだけ。管理画面へのリンクは無い |
| 管理者用 | [`prototypes/event-comments-admin/`](../prototypes/event-comments-admin/) | 選別・イベント設定・イベントの切り替え・QR表示。`ADMIN_KEY` が要る |

参加者用ページのタイトル・説明文・色・入力欄のヒントは、管理者用ページの「参加者ページの文言と色」
から変えられる (GAS の `settings` シートに入る)。参加者ページは開いたときにこれを読み、
読めなければ既定の文言で出る。

## 全体の流れ

```
参加者のスマホ                 管理者のPC                        実機
  event-comments/  ──add──────▶  スプレッドシート  ◀──list─────  event-comments-admin/
                   ◀─settings──  (GAS Webアプリ)    ──state────▶
                                  comments          ──settings──▶ (文言と色)
                                  settings          ──event_*───▶ (イベントの保存・切替)
                                  events
                                  archive_…
                                       │
                                       └── action=text ──▶ ESP32 が15秒ごとに取得
                                           (承認済みだけ・1行1件・最大20件)
```

- **自動では何も捨てない。** 自動判定はNGワード等に理由を付けて「未確認」に積むだけで、
  掲示板に出す・見送るは必ず人が押す。
- 実機とつなぐのは `playlist.json` の `commentsUrl`（v0.10〜）。ファームウェアの書き込みは不要
  （仕様: [`playlist-spec.md`](playlist-spec.md)）。

## 手順

### 1. スプレッドシートを作る

Googleドライブで新規スプレッドシートを作る。名前は何でもよい（例: `もしも イベントコメント`）。
シートは空のままでよい（`comments` / `settings` / `events` シートは初回アクセス時に自動で作られる）。

### 2. Apps Script を貼る

スプレッドシートのメニューから **拡張機能 → Apps Script**。
既定の `Code.gs` の中身を消して、[`tools/event-comments-gas/Code.gs`](../tools/event-comments-gas/Code.gs)
の中身をそのまま貼り付けて保存する。

### 3. 管理キーを設定する

Apps Script の左メニュー **プロジェクトの設定 → スクリプト プロパティ → プロパティを追加**。

| プロパティ | 値 |
|---|---|
| `ADMIN_KEY` | 適当な長い文字列（例: `moshimo-2026-natsu-8f3a`） |

**このキーはリポジトリに書かないこと。** 承認・見送りの操作はこのキーを知っている人だけができる。
参加者の投稿にはキーは要らない（誰でも投稿できてよいため）。

### 4. ウェブアプリとしてデプロイ

右上の **デプロイ → 新しいデプロイ → 種類を選択（歯車）→ ウェブアプリ**。

| 項目 | 設定 |
|---|---|
| 次のユーザーとして実行 | 自分 |
| アクセスできるユーザー | **全員** |

「全員」にしないと参加者のスマホからも実機からも届かない。
発行された `https://script.google.com/macros/s/..../exec` をコピーする。

> デプロイし直すたびにURLが変わる設定になっていることがある。URLを配ったあとにコードを直す場合は
> 「デプロイを管理」から**既存のデプロイを編集**すればURLは変わらない。

### 5. 管理画面につなぐ

[管理者用ページ](../prototypes/event-comments-admin/) を開き、「つなぎ先（GAS）」に
さきほどのURLと `ADMIN_KEY` を入れて「保存してつなぐ」。
上部の帯が「GASにつながっています」になれば成功。

> **入れるのは `…/exec` で終わるURL。** ブラウザで `/exec` を開くと
> `https://script.googleusercontent.com/macros/echo?user_content_key=…` に転送されるので、
> **アドレスバーからコピーするとこの転送先を拾ってしまう。** これを入れると帯が
> 「つなぎ先に届きません」のままになる (2026-09-08 に実際に起きた)。
> 転送先のURLは最初のリクエスト1回ぶんを焼き付けた使い捨てのもので、`action` を
> 付け替えても常に同じ応答しか返さないため、管理画面の `action=list` が通らない。
> 正しいURLは Apps Script の「デプロイを管理」に表示されているもの。

同じカードに**参加者用URLとQRコード**が出るので、それを会場で見せる。

参加者用URLは `…/prototypes/event-comments/?role=post&gas=<デプロイID>` の形
(`role=post` は2ページに分ける前に配ったQRと同じURLにするため残している。参加者ページは `role` を見ない)。
GASのURLをまるごと `?api=` に入れる古い形も動くが、URLエンコードで長くなりQRが密になって
読み取りづらいので、いまは短い `gas=` を出している。QRが出ないときは理由がその場に表示される
(ライブラリが読めていない等)。**URLをコピーして配るだけでも運用できる。**

### 6. 実機につなぐ

`playlist.json` の `commentsUrl` に **`?action=text` を付けたURL** を書いて push する
(仕様: [`playlist-spec.md`](playlist-spec.md))。**ファームウェアの書き込みは不要。**

```json
{ "commentsUrl": "https://script.google.com/macros/s/..../exec?action=text" }
```

- 実機がこれを読むのは次の playlist 取得のとき。**約1〜2分**で繋がる
- 以降、管理画面で「掲示板に出す」を押すと **15秒以内に実機に出る**
- **要 v0.10以上** (実機の版は電源を挿し直すと右下に数秒出る)。
  v0.9以前は取得先が `config.h` 固定で、かつGASの302リダイレクトを追わなかった
- 受け付けるURLは `https://script.google.com/macros/` で始まるものだけ。
  別の置き場を使うときは `config.h` の `COMMENTS_URL` に書く (ファームウェア担当の作業)

**イベントが終わったら `commentsUrl` を `""` に戻す。** 取得が止まり、流れていたコメントも消える。

## APIの仕様

| | |
|---|---|
| `GET ?action=text` | 承認済みを1行1件のプレーンテキストで返す。**実機がこれを見る** |
| `GET ?action=public` | 承認済みを JSON で返す（参加者画面用・鍵不要） |
| `GET ?action=list&key=…` | 全件を JSON で返す（管理画面用）。**鍵が要る** |
| `POST {"action":"add","text":"..."}` | 1件足す。状態は `pending` |
| `POST {"action":"state","id":1,"state":"approved","key":"..."}` | 状態を変える。`key` 必須 |
| `GET ?action=settings` | 参加者ページの文言と色 `{ok, settings:{title, lead, color, placeholder}}`。鍵不要。空の項目は既定値で埋めて返す |
| `POST {"action":"settings","key":"...","title":"...","lead":"...","color":"...","placeholder":"..."}` | 文言と色を保存。`key` 必須。送らなかった項目はそのまま、空文字は既定に戻す。`color` は `FF9C00` / `#ff9c00` どちらでもよい |
| `GET ?action=events&key=…` | 保存済みイベントの一覧 `{ok, events:[{name, title, lead, color, placeholder, savedAt}]}`。`key` 必須 |
| `POST {"action":"event_save","key":"...","name":"..."}` | いまの文言と色をその名前で `events` シートに保存。同名は上書き |
| `POST {"action":"event_load","key":"...","name":"..."}` | その名前の文言と色をいまの設定にする。無ければ `not found` |
| `POST {"action":"event_start","key":"...","name":"..."}` | 新しいイベントを始める。`comments` を `archive_<yyyyMMdd>_<name>` シートに写してから空にし、その名前の設定があれば読み込む（無ければ設定はそのまま）。応答 `{ok, archived, moved, loaded, settings}` |

- シートは `comments`（id / ts / text / state）、`settings`（key / value）、
  `events`（name / title / lead / color / placeholder / savedAt）、退避先の `archive_…`
- `event_start` のあとは `comments` が空になるので、実機の `action=text` も空になる
  (event モードの実機は、コメントが0件のあいだ常設メッセージを流す)。id は 1 から振り直し

- **`Code.gs` を貼り替えたら、必ずデプロイし直すこと**（「デプロイを管理」→ 既存のデプロイを鉛筆で編集 →
  バージョンを「新しいバージョン」にして更新）。貼っただけでは公開中のURLは古いコードのまま。
  古いままだと参加者画面が「未確認」「見送り」まで読めてしまう（管理画面が赤帯で知らせる）
  - **アクティブなデプロイが複数あると、貼り替えても古いURLが古いコードを配り続ける。**
    2026-09-08 に実際にこれで一度失敗した（稼働中が3つあった）。「デプロイを管理」で
    **使っていないデプロイはアーカイブして1つに絞る**。そのうえでバージョンを上げる
  - 反映できたかは叩けば分かる。`<URL>?action=public` が `{"ok":true,…}` を返せば新しい、
    `unknown action` なら古いまま。`<URL>?action=list`（鍵なし）が `forbidden` なら塞がっている
- `state` は `pending` / `approved` / `rejected` のいずれか
- POST の `Content-Type` は `text/plain` にする（GASはプリフライトを返せないため）
- 1件の上限は100文字、実機に渡すのは新しい20件（`MAX_COMMENTS` に合わせている）

## 実機の見た目を管理画面から変える (v19〜)

`commentsUrl` の応答を JSON にすると、イベント中だけ実機の見た目を GAS 側から変えられる。
`playlist.json` を push しなくてよい。**要 v19以上** (v18以前は JSON を1行1件のテキストとして読んでしまう)。

```json
{ "settings": { "colorScroll": "FF9C00", "speed": 60, "scrollScale": 2, "scrollBold": true, "eventLines": 1 },
  "comments": [ { "id": 12, "text": "こんにちは" } ] }
```

| キー | 範囲 | 意味 |
|---|---|---|
| `colorScroll` | RGB 16進6桁 / `"rainbow"` | 流れる文字の色 |
| `speed` | 5〜200 (丸める) | スクロール速度 dot/秒 |
| `scrollScale` | 1 / 2 (丸める) | 拡大率。`2` は `eventLines:1` のときだけ効く |
| `scrollBold` | true / false | 太字 |
| `eventLines` | 1 / 2 (丸める、既定 2) | `1` = 上段を出さず1段で流す / `2` = 従来どおり2段 |
| `eventFlow` | 文字列 | 実機は保持するだけ (v20 で使う予定) |

- **優先順位: GAS の `settings` > `playlist.json`。** 指定のあるキーだけ上書きし、無いキーは `playlist.json` のまま
- 効くのは `mode:"event"` のあいだだけ。「通常に戻して」で `mode` が戻れば自動で外れる
- `settings` の無い応答では前回の設定を保持する。`commentsUrl` を `""` にすると設定も破棄
- 詳しい規則は [`playlist-spec.md`](playlist-spec.md) の「イベント中の表示設定」

## 当日の運びかた

1. **イベントを始める。** 管理者用ページをPCで開いてGASにつなぎ、「イベント」カードで
   名前を選んで（または入れて）「このイベントを始める」。前回のコメントは `archive_…` シートに退避されて空になり、
   保存してあればそのイベントの文言と色が参加者ページに入る。初めてのイベントなら
   「参加者ページの文言と色」を書いて保存 → 「いまの設定をこの名前で保存」しておくと次から使い回せる
2. **実機をイベントモードにする。** Discordで「イベントモードにして」と頼む
   (`mode:"event"` と `commentsUrl` が入る。約1〜2分で反映)
3. **QRを出す。** 管理者用ページの「つなぎ先（GAS）」に出る参加者用QRを会場に見せる
4. 開始前に**自分のスマホから1件投稿して、実機に出るところまで通す**
   （承認から実機まで15秒。ここで出なければ `commentsUrl` の設定か実機の版を疑う）
5. 「未確認」タブを見ながら、出すものを押していく
6. 終わったら **CSVで保存**（振り返り用。元のログはスプレッドシートにも、次に「始める」と `archive_…` にも残る）
7. **通常に戻す。** Discordで「通常に戻して」と頼む (`mode:"dual"`、`commentsUrl:""` に戻る)

## 注意

- 公共の場に出る文言なので、迷ったら出さない。**見送りは何度でも取り消せる**
- 参加者の名前は取っていない（本文だけ）。名前を出したい場合は仕様から相談する
- 管理キーが漏れると誰でも承認できる。イベントごとに変えるのが安全
