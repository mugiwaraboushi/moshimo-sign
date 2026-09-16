#!/usr/bin/env python3
"""data/events.json を見て、当日のイベント告知を playlist.json の下段に1行だけ出し入れする。

  python3 scripts/update-event-message.py            変化があれば playlist.json を書き換える
  python3 scripts/update-event-message.py --clear    告知の行を消すだけ
  python3 scripts/update-event-message.py --dry-run  書き換えずに、どうなるかだけ出す
  python3 scripts/update-event-message.py --now 2026-09-22T15:30   その時刻だと仮定して動かす (確認用)

GitHub Actions (.github/workflows/events.yml) から定期実行される。判定はすべて日本時間。

大事な約束: 自分が入れた行は .event-line.json に控えておき、それと一致する行だけを消す。
人が手で足した文言や、雨の行 (scripts/update-rain-message.py) を巻き込まないための作り。
Actions は毎回まっさらな作業コピーで動くので、この控えは playlist.json と一緒に commit する。
"""
import argparse, datetime, json, pathlib, sys, zoneinfo

ROOT = pathlib.Path(__file__).resolve().parent.parent
EVENTS = ROOT / "data" / "events.json"
PLAYLIST = ROOT / "playlist.json"
STATE = ROOT / ".event-line.json"
MAX_MESSAGES = 10                      # playlist.json の仕様上の上限
DEFAULT_LEAD_MIN = 120                 # from を書かなければ開始の2時間前から出す


def to_min(hhmm, where):
    """\"15:00\" を 0時からの分数にする。"""
    try:
        h, m = str(hhmm).strip().split(":")
        h, m = int(h), int(m)
    except ValueError:
        raise SystemExit(f'{where}: 時刻の書き方が不正です: "{hhmm}"')
    if not (0 <= h <= 23 and 0 <= m <= 59):
        raise SystemExit(f'{where}: 時刻の範囲が不正です: "{hhmm}"')
    return h * 60 + m


def desired_line(events, date_key, now_min):
    """いま出すべき告知の1行。出す必要がなければ None。"""
    today = next((e for e in events.get("events", []) if e.get("date") == date_key), None)
    if not today:
        return None

    start = to_min(today["start"], date_key)
    end = to_min(today["end"], date_key)
    if end <= start:
        raise SystemExit(f"{date_key}: end が start より後になっていません")

    frm = to_min(today["from"], date_key) if today.get("from") else max(0, start - DEFAULT_LEAD_MIN)
    to = to_min(today["to"], date_key) if today.get("to") else end
    if not (frm <= now_min < to):
        return None

    # 開始前は before、始まってからは during (無ければ before のまま)
    line = today.get("before") if now_min < start else (today.get("during") or today.get("before"))
    return line or None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clear", action="store_true", help="告知の行を消すだけ")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--now", help="確認用。日本時間で \"YYYY-MM-DDTHH:MM\" と仮定して動かす")
    a = ap.parse_args()

    events = json.loads(EVENTS.read_text(encoding="utf-8"))
    tz = zoneinfo.ZoneInfo(events.get("tz", "Asia/Tokyo"))
    now = (datetime.datetime.fromisoformat(a.now) if a.now
           else datetime.datetime.now(tz).replace(tzinfo=None))
    date_key = now.strftime("%Y-%m-%d")
    now_min = now.hour * 60 + now.minute

    line = None if a.clear else desired_line(events, date_key, now_min)

    previous = None
    if STATE.exists():
        try:
            previous = json.loads(STATE.read_text(encoding="utf-8")).get("line")
        except (ValueError, OSError):
            previous = None

    if previous == line:
        print(f"{date_key} {now:%H:%M} JST — 変化なし: " + (f'"{line}"' if line else "告知の行なし"))
        return 0

    playlist = json.loads(PLAYLIST.read_text(encoding="utf-8"))
    messages = [m for m in playlist["messages"] if m != previous]   # 自分が入れた行だけ外す
    if line:
        messages.insert(0, line)
    if len(messages) > MAX_MESSAGES:
        print(f"中止: messages が {len(messages)} 件で上限 {MAX_MESSAGES} を超えます", file=sys.stderr)
        return 1

    print(f'{previous or "(なし)"}  ->  {line or "(なし)"}')
    if a.dry_run:
        print("--dry-run なので書き換えません")
        return 0

    playlist["messages"] = messages
    out = json.dumps(playlist, ensure_ascii=False, indent=2) + "\n"
    json.loads(out)                                    # 書く前に壊れていないか確かめる
    PLAYLIST.write_text(out, encoding="utf-8")
    STATE.write_text(json.dumps({"line": line, "date": date_key}, ensure_ascii=False) + "\n",
                     encoding="utf-8")
    print("playlist.json を更新しました")
    return 0


if __name__ == "__main__":
    sys.exit(main())
