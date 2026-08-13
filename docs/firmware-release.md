# ファームウェアのリリース手順 (自己アップデート v0.7)

実機は `firmware/manifest.json` を定期的に見て、自分より新しいバージョンがあれば
`.bin` を取りに行って自分で書き込む。**この手順を踏めば、設置後もUSB接続・現地作業なしで
ファームウェアを更新できる。**

- 定期確認: 起動60秒後に初回、以後6時間ごと (`SELFUPDATE_INTERVAL_MS`)
- 即時反映: `playlist.json` の `fwPing` に新バージョン番号を書くと、周期を待たずその場で更新
- ビルド手順そのものは [`firmware-build.md`](firmware-build.md) を参照

---

## いちばん大事な決まり

> **`.bin` は `FW_VERSION` を上げてビルドしたものを公開する。**
> `manifest.json` の `version` だけ上げて `.bin` が古いままだと、実機は
> 「更新した → 再起動 → まだ古い → また更新」を延々繰り返す。

この事故は実際に起こりやすいので、ファームウェア側にも歯止めを入れてある
(同じバージョンへの更新を2回連続で試みたら中断してシリアルに警告を出す)。
ただし**歯止めは電源を切ると忘れる**ので、頼りにせず手順で防ぐこと。

もう一点:

> **ロールバックは無い。** 起動しない `.bin` を公開すると、実機はUSB接続でしか復旧できない。
> 設置後は特に、`.bin` を公開する前に必ず手元の実機で `espota` による書き込みテストを済ませること。

---

## 手順

### ① ファームウェア担当レビュー済みの main をビルド

```bash
git checkout main && git pull
# firmware/moshimo_sign/moshimo_sign.ino の FW_VERSION を +1 する
arduino-cli compile --fqbn "esp32:esp32:esp32:PartitionScheme=min_spiffs" \
  --output-dir build/moshimo_sign firmware/moshimo_sign
```

`FW_VERSION` を上げ忘れていないか、ここで必ず確認する。

### ② (実機が手元にある間は) espota で直接テスト

公開する前に、その `.bin` で実機がちゃんと起動して動くことを確かめる。

```bash
espota.exe -i moshimo-sign.local -p 3232 -a "<OTA_PASSWORD>" \
  -f build/moshimo_sign/moshimo_sign.ino.bin -r
```

`espota.exe` は `arduino-cli config get directories.data` 配下の
`packages/esp32/hardware/esp32/<ver>/tools/` にある。
パスワードは `config.h` の `OTA_PASSWORD` (リポジトリには入れない)。

**実機が手元を離れたあとはこの段階が踏めない。** その場合は「戻せない」前提で、
③以降を実施するかどうかをファームウェア担当と相談すること。

### ③ `.bin` と `manifest.json` をコミットして push

```bash
cp build/moshimo_sign/moshimo_sign.ino.bin firmware/moshimo_sign.bin
# md5 と size を求める
md5sum firmware/moshimo_sign.bin        # または certutil -hashfile ... MD5
stat -c%s firmware/moshimo_sign.bin
```

`firmware/manifest.json` を更新する:

```json
{
  "version": 8,
  "url": "https://mugiwaraboushi.github.io/moshimo-sign/firmware/moshimo_sign.bin",
  "md5": "<md5sum の出力>",
  "size": 1426395,
  "quietHours": { "from": 2, "to": 5 }
}
```

| キー | 意味 |
|---|---|
| `version` | 新しいバージョン番号。**`.bin` の `FW_VERSION` と必ず一致させる** |
| `url` | `.bin` の公開URL |
| `md5` | `.bin` のMD5。書き込み後に実機が照合し、合わなければ中断して現行のまま残る |
| `size` | `.bin` のバイト数。サーバの `Content-Length` と食い違えば中断する |
| `quietHours` | この時間帯 (JST) のときだけ更新する。`{"from":2,"to":5}` なら2時〜5時 |

`quietHours` を省くと**見つけ次第すぐ更新する**。テスト時以外は付けること
(表示中に数十秒の停止と再起動が入るため)。NTP未同期で時刻が信用できないときは、
`quietHours` がある限り更新を見送る。

### ④ `playlist.json` の `fwPing` を新バージョン番号にして push

```json
{ "fwPing": 8 }
```

`fwPing` はチャットから即時更新をかけるための口。実機は次の `playlist.json` 取得
(約60秒周期) でこれを見つけ、**`quietHours` を無視して**その場で更新する。
急がないなら `fwPing` は書かなくてよい (6時間周期で自然に上がる)。

`playlist.json` を編集したら、JSONの検証を忘れないこと (`CLAUDE.md` の手順2)。

### ⑤ Discordに告知

> ファームv8を公開しました。実機はまもなく自動更新されます。

---

## 実機側の動き (シリアルログ)

`[selfupdate]` で始まる行が出る。

| ログ | 意味 |
|---|---|
| `up to date (manifest v7 / running v7)` | 同じバージョン。**何もしないのが正常** |
| `postpone: quietHours 2-5時の外` | 時間帯待ち。次の周期に持ち越す |
| `postpone: NTP未同期で時刻が信用できない` | 時刻が取れるまで見送る |
| `start: v7 -> v8 (1426395 bytes)` | ダウンロードと書き込みを開始 |
| `ok: v8 を書き込んだ。再起動する` | 成功。この直後に再起動する |
| `verify failed: ...` | MD5不一致など。**現行のまま動き続ける**ので次の周期で再試行 |
| `abort: v8 を書き込み済みのはずが FW_VERSION=7 のまま` | `.bin` と `manifest` の不一致。①をやり直す |

更新は `version > FW_VERSION` のときだけ実行する (ダウングレードはしない)。
書き込みに失敗しても現行のファームウェアはそのまま残る。

---

## 触ってはいけないもの

`firmware/manifest.json` と `firmware/*.bin` は**この手順以外で変更しない**。
とくに `manifest.json` の `version` を単独で書き換えると、実機が無限に再書き込みを試みる。
