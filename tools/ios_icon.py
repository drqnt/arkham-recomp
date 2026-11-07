#!/usr/bin/env python3
"""The phone app's icon, from the game's own: tools/ios_icon.py

Takes the Steam version's icon out of the game's Mac app on this Mac (GameIcon_STM.icns, or GameIcon.icns), cuts away its transparent edge, fades its
grey Mac frame into black while keeping the logo's colours that reach into it, lays it on black, since an iPhone icon has no transparency and iOS
rounds the corners itself, and writes it as the asset catalog the Xcode project compiles: build/ios/Assets.xcassets. It comes from the game, so it
stays in build/, out of the repository. ios/app/run.sh runs this before each build; without the game on this Mac the icon is plain black."""
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_APP = os.path.expanduser("~/Library/Application Support/Steam/steamapps/common/Batman Arkham Asylum GOTY/Batman Arkham Asylum GOTY.app")
OUT = os.path.join(ROOT, "build", "ios", "Assets.xcassets")
CROP = 8   # pixels at each side of the 1024 one that are transparent
BAND = 88  # and then the frame: its greys fade to black over this many


def read_png(path):
    """Width, height and rows of RGBA bytes of an 8-bit RGBA PNG, as iconutil writes them."""
    data = open(path, "rb").read()
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, color = struct.unpack(">IIBB", body[:10])
            if (depth, color) != (8, 6):
                raise ValueError(f"{path}: not 8-bit RGBA")
        elif kind == b"IDAT":
            idat += body
    raw, stride, rows, prev, i = zlib.decompress(idat), w * 4, [], bytearray(w * 4), 0
    for _ in range(h):
        kind, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, rows


def write_png(path, w, h, rows):
    """An 8-bit RGB PNG of rows of RGB bytes."""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main() -> int:
    icon_set = os.path.join(OUT, "AppIcon.appiconset")
    os.makedirs(icon_set, exist_ok=True)
    png = os.path.join(icon_set, "icon.png")
    sources = [os.path.join(GAME_APP, "Contents", "Resources", n) for n in ("GameIcon_STM.icns", "GameIcon.icns")]
    src = next((p for p in sources if os.path.exists(p)), None)
    newest = max([os.path.getmtime(__file__)] + ([os.path.getmtime(src)] if src else []))
    if not (os.path.exists(png) and os.path.getmtime(png) >= newest):
        with tempfile.TemporaryDirectory() as tmp:
            flat = os.path.join(tmp, "flat.png")
            if src:
                iconset = os.path.join(tmp, "icon.iconset")
                subprocess.run(["iconutil", "-c", "iconset", "-o", iconset, src], check=True)
                w, h, rows = read_png(os.path.join(iconset, "icon_512x512@2x.png"))
                out = []
                for y in range(CROP, h - CROP):
                    r, line = rows[y], bytearray()
                    for x in range(CROP, w - CROP):
                        a = r[4 * x + 3]
                        px = [r[4 * x + i] * a // 255 for i in range(3)]
                        edge = min(x - CROP, y - CROP, w - CROP - 1 - x, h - CROP - 1 - y)
                        if edge < BAND and max(px) - min(px) < 60:  # grey, near the edge: the frame or the dark ground, not the logo
                            k = (edge / BAND) ** 2
                            px = [int(v * k) for v in px]
                        line += bytes(px)
                    out.append(line)
                write_png(flat, w - 2 * CROP, h - 2 * CROP, out)
                print(f"the app's icon from {os.path.basename(src)}")
            else:
                write_png(flat, 1024, 1024, [bytearray(1024 * 3)] * 1024)
                print("no game on this Mac: the app's icon is plain black")
            subprocess.run(["sips", "-z", "1024", "1024", flat, "--out", png], check=True, capture_output=True)
    info = {"author": "xcode", "version": 1}
    json.dump({"info": info}, open(os.path.join(OUT, "Contents.json"), "w"), indent=2)
    json.dump({"images": [{"filename": "icon.png", "idiom": "universal", "platform": "ios", "size": "1024x1024"}], "info": info},
              open(os.path.join(icon_set, "Contents.json"), "w"), indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
