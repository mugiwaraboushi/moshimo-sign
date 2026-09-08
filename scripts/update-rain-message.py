#!/usr/bin/env python3
"""雨の見通しを playlist.json の下段に1行だけ出し入れする。

  python3 scripts/update-rain-message.py            変化があれば playlist.json を書き換える
  python3 scripts/update-rain-message.py --clear    雨の行を消すだけ (見張りをやめるとき)
  python3 scripts/update-rain-message.py --dry-run  書き換えずに、どうなるかだけ出す

出す文言は3通り:
  降っている        「雨が降っています 傘をお忘れなく」
  1時間以内に降る    「13:45ごろ 雨が降り出しそうです 傘をどうぞ」
  どちらでもない     行を出さない

**この行は入れっぱなしにしない。** 予測は数分で変わるので、見張りを止めるときは
必ず --clear で消すこと。消し忘れると古い時刻が掲示板に残り続ける。

自分が入れた行は state ファイルに控えておき、それと一致する行だけを消す。
人が手で足した文言を巻き込まないための作りにしている。
"""
import argparse, importlib.util, json, os, pathlib, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PLAYLIST = ROOT / "playlist.json"
MAX_MESSAGES = 10                      # playlist.json の仕様上の上限

spec = importlib.util.spec_from_file_location("rain", ROOT / "tools" / "rain-watch" / "rain.py")
rain = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rain)


def desired_line(lat, lon, zoom, lead_min):
    """いま出すべき雨の1行。出す必要がなければ None。"""
    obs = [t for t in rain.fetch(f"{rain.BASE}/targetTimes_N1.json")
           if t["validtime"] == t["basetime"]]
    latest = max(obs, key=lambda t: t["basetime"])
    if rain.intensity_at(latest["basetime"], latest["validtime"], lat, lon, zoom) > 0:
        return "雨が降っています 傘をお忘れなく", "raining"

    obs_at = rain.to_jst(latest["basetime"])
    for t in sorted(rain.fetch(f"{rain.BASE}/targetTimes_N2.json"), key=lambda t: t["validtime"]):
        at = rain.to_jst(t["validtime"])
        if (at - obs_at).total_seconds() / 60 > lead_min:
            break
        if rain.intensity_at(t["basetime"], t["validtime"], lat, lon, zoom) > 0:
            return f"{at:%H:%M}ごろ 雨が降り出しそうです 傘をどうぞ", "soon"
    return None, "dry"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lat", type=float, default=rain.OOKAYAMA[0])
    ap.add_argument("--lon", type=float, default=rain.OOKAYAMA[1])
    ap.add_argument("--zoom", type=int, default=10)
    ap.add_argument("--lead-min", type=int, default=60, help="何分先までの予測を出すか")
    ap.add_argument("--state", default=str(ROOT / ".rain-line.json"),
                    help="自分が入れた行を控えておくファイル")
    ap.add_argument("--clear", action="store_true", help="雨の行を消すだけ")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    line, state = (None, "cleared") if a.clear else desired_line(a.lat, a.lon, a.zoom, a.lead_min)

    previous = None
    if os.path.exists(a.state):
        try:
            previous = json.load(open(a.state)).get("line")
        except (ValueError, OSError):
            previous = None

    if previous == line:
        print(f"変化なし ({state}): " + (f'"{line}"' if line else "雨の行なし"))
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
    with open(a.state, "w") as f:
        json.dump({"line": line, "state": state}, f, ensure_ascii=False)
    print("playlist.json を更新しました")
    return 0


if __name__ == "__main__":
    sys.exit(main())
