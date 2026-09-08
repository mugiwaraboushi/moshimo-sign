#!/usr/bin/env python3
"""もしも付近で雨が降り出したかを、気象庁の高解像度降水ナウキャストから調べる。

  python3 tools/rain-watch/rain.py            いまの状況と1時間先までの見通しを出す
  python3 tools/rain-watch/rain.py --state F  前回と比べて「降り出した/やんだ」の変化だけ報告する

使っているのは気象庁のナウキャストのタイル画像 (5分ごと・1kmメッシュ)。
観測は targetTimes_N1.json、予測 (最大1時間先) は targetTimes_N2.json にある。
外部ライブラリは使わない (標準の zlib だけでPNGを展開している)。

基準点は大岡山駅。**掲示板の設置場所ではなく、公開されている目印を使っている。**
別の地点を見たいときは --lat / --lon で渡す。
"""
import argparse, json, math, os, struct, sys, urllib.request, zlib
from datetime import datetime, timedelta, timezone

BASE = "https://www.jma.go.jp/bosai/jmatile/data/nowc"
JST = timezone(timedelta(hours=9))
OOKAYAMA = (35.6069, 139.6857)   # 大岡山駅 (公開されている目印)

# ナウキャストのタイルは 4bit パレットPNG。パレット番号がそのまま強度の段階になる。
# 0,1 = 降水なし (透明)、2以降が凡例の各段。値は mm/h の下限。
LEVELS = [0, 0, 0.5, 1, 5, 10, 20, 30, 50, 80]
LABELS = {0: "降っていない", 0.5: "ごく弱い雨", 1: "弱い雨", 5: "雨", 10: "やや強い雨",
          20: "強い雨", 30: "激しい雨", 50: "非常に激しい雨", 80: "猛烈な雨"}


def fetch(url, binary=False):
    req = urllib.request.Request(url, headers={"User-Agent": "moshimo-sign/rain-watch"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read() if binary else json.loads(r.read())


def tile_xy(lat, lon, z):
    """緯度経度 → タイル番号と、そのタイル内の画素位置 (Webメルカトル)。"""
    n = 2 ** z
    x = (lon + 180.0) / 360.0 * n
    y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n
    return int(x), int(y), int((x % 1) * 256), int((y % 1) * 256)


def palette_index(png, px, py):
    """パレットPNGの1画素のパレット番号を返す。PILが無い環境で動かすため手で展開する。"""
    pos, idat, ihdr = 8, b"", None
    while pos < len(png):
        ln = struct.unpack(">I", png[pos:pos + 4])[0]
        typ, body = png[pos + 4:pos + 8], png[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    w, depth, ctype = ihdr[0], ihdr[2], ihdr[3]
    if ctype != 3:
        raise RuntimeError(f"パレットPNGではない (color type {ctype})")
    raw = zlib.decompress(idat)
    stride = (w * depth + 7) // 8
    bpp = max(1, depth // 8)
    prev, line, off = bytearray(stride), bytearray(stride), 0
    for _ in range(py + 1):                      # 目的の行までほどく (上の行が必要なため)
        ft = raw[off]; off += 1
        line = bytearray(raw[off:off + stride]); off += stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ft == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        prev = line
    if depth == 8:
        return line[px]
    per = 8 // depth
    return (line[px // per] >> ((per - 1 - (px % per)) * depth)) & ((1 << depth) - 1)


def intensity_at(basetime, validtime, lat, lon, z):
    """その時刻・その地点の降水強度 (mm/h)。周囲1画素も見て取りこぼしを減らす。"""
    x, y, px, py = tile_xy(lat, lon, z)
    png = fetch(f"{BASE}/{basetime}/none/{validtime}/surf/hrpns/{z}/{x}/{y}.png", binary=True)
    best = 0.0
    for dx in (-1, 0, 1):                        # 3x3画素 ≒ 450m四方の最大値を採る
        for dy in (-1, 0, 1):
            qx, qy = px + dx, py + dy
            if 0 <= qx < 256 and 0 <= qy < 256:
                idx = palette_index(png, qx, qy)
                if idx < len(LEVELS):
                    best = max(best, LEVELS[idx])
    return best


def to_jst(stamp):
    return datetime.strptime(stamp, "%Y%m%d%H%M%S").replace(tzinfo=timezone.utc).astimezone(JST)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lat", type=float, default=OOKAYAMA[0])
    ap.add_argument("--lon", type=float, default=OOKAYAMA[1])
    ap.add_argument("--zoom", type=int, default=10)
    ap.add_argument("--state", help="前回の状態を覚えておくファイル。変化があったときだけ報告する")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    obs = [t for t in fetch(f"{BASE}/targetTimes_N1.json") if t["validtime"] == t["basetime"]]
    latest = max(obs, key=lambda t: t["basetime"])
    now_mmh = intensity_at(latest["basetime"], latest["validtime"], a.lat, a.lon, a.zoom)
    obs_at = to_jst(latest["basetime"])

    # 1時間先までの予測から、最初に降り出す時刻を探す
    soon = None
    if now_mmh == 0:
        fc = sorted(fetch(f"{BASE}/targetTimes_N2.json"), key=lambda t: t["validtime"])
        for t in fc:
            if intensity_at(t["basetime"], t["validtime"], a.lat, a.lon, a.zoom) > 0:
                at = to_jst(t["validtime"])
                soon = {"at": at.strftime("%H:%M"),
                        "in_min": round((at - obs_at).total_seconds() / 60)}
                break

    result = {"observed_at": obs_at.strftime("%Y-%m-%d %H:%M"), "mmh": now_mmh,
              "raining": now_mmh > 0, "label": LABELS.get(now_mmh, f"{now_mmh}mm/h"),
              "starts_soon": soon}

    changed = True
    if a.state:
        was = None
        if os.path.exists(a.state):
            try:
                was = json.load(open(a.state)).get("raining")
            except (ValueError, OSError):
                was = None
        changed = (was is None) or (was != result["raining"])
        result["was_raining"] = was
        with open(a.state, "w") as f:
            json.dump({"raining": result["raining"], "observed_at": result["observed_at"]}, f)
    result["changed"] = changed

    if a.json:
        print(json.dumps(result, ensure_ascii=False))
        return 0
    print(f"{result['observed_at']} JST 時点 — {result['label']}"
          + (f" ({now_mmh}mm/h)" if now_mmh else ""))
    if soon:
        print(f"およそ{soon['in_min']}分後 ({soon['at']}) に降り出す予測です")
    if a.state:
        print("前回から変化あり" if changed else "前回から変化なし")
    return 0


if __name__ == "__main__":
    sys.exit(main())
