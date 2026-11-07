#!/usr/bin/env python3
"""Builds the game's shaders for the phone ahead of play: tools/build_ios_shaders.py

Without these the phone compiles each shader from source the first time a place needs it, about 45 ms apiece: the first room after a cutscene
stalled for 14 seconds. This translates every shader in the game's own shader caches with the runtime's translator (tools/check_shaders.py
translate), compiles each with the iOS Metal compiler under a name made from the hash of its text, and links them into 16 libraries in
build/ios/shaders. tools/ios_install.sh puts those in the app's Documents/Shaders, and the runtime takes a shader from them when one carries its
text's name (mtl_device.m): a translator that has changed since gives other texts, so those are compiled on the phone as before, never taken stale.
Everything written comes from the game, so it stays in build/ and logs/, out of the repository. Running it again only compiles what is new."""
import concurrent.futures as cf
import glob
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = dict(os.environ, DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer")
MSL = os.path.join(ROOT, "logs", "msl")
OUT = os.path.join(ROOT, "build", "ios", "shaders")
AIR = os.path.join(OUT, "air")
SHARDS = 16


def text_name(data: bytes) -> str:
    """FNV-1a over the text, as text_hash() in runtime/mtl_device.m."""
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return "s_%016x" % (h or 1)


def compile_one(job):
    src, name, entry = job
    out = os.path.join(AIR, name + ".air")
    if os.path.exists(out):
        return name, 0, ""
    tmp = out + ".part"
    # The runtime compiles with preserveInvariance, so these are compiled the same way.
    r = subprocess.run(["xcrun", "-sdk", "iphoneos", "metal", "-c", "-fpreserve-invariance", f"-D{entry}={name}", src, "-o", tmp],
                       env=ENV, capture_output=True, text=True)
    if r.returncode:
        return name, r.returncode, r.stderr[:500]
    os.replace(tmp, out)
    return name, 0, ""


def main() -> int:
    sources = [os.path.join(ROOT, "runtime", "d3d_shader.c"), os.path.join(ROOT, "tools", "d3d2msl.c")]
    newest_source = max(os.path.getmtime(p) for p in sources)
    listing = os.path.join(MSL, "files.txt")
    if not os.path.exists(listing) or os.path.getmtime(listing) < newest_source:
        print("translating the game's shaders with the current translator")
        subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "check_shaders.py"), "translate"], check=True)
    os.makedirs(AIR, exist_ok=True)
    jobs, names = [], {}
    for path in sorted(glob.glob(os.path.join(MSL, "*.metal"))):
        data = open(path, "rb").read()
        name = text_name(data)
        if name in names:
            continue  # the same text under another shader's name: one compile does for both
        entry = "ps_main" if os.path.basename(path).startswith("ps_") else "vs_main"
        names[name] = path
        jobs.append((path, name, entry))
    print(f"{len(jobs)} different shader texts")
    t0 = time.time()
    failed = 0
    with cf.ThreadPoolExecutor(os.cpu_count()) as pool:
        for k, (name, rc, err) in enumerate(pool.map(compile_one, jobs), 1):
            if rc:
                failed += 1
                if failed <= 3:
                    print(f"FAILED {names[name]}\n{err}", file=sys.stderr)
            if k % 2000 == 0:
                print(f"  {k}/{len(jobs)} {time.time() - t0:.0f}s", flush=True)
    print(f"compiled in {time.time() - t0:.0f}s, {failed} failed")
    shards = {i: [] for i in range(SHARDS)}
    for name in names:
        air = os.path.join(AIR, name + ".air")
        if os.path.exists(air):
            shards[int(name[2], 16) % SHARDS].append(air)
    t1 = time.time()
    for i, airs in shards.items():
        lib = os.path.join(OUT, f"arkham-{i:02d}.metallib")
        rsp = os.path.join(OUT, f"arkham-{i:02d}.rsp")
        open(rsp, "w").write("\n".join(airs) + "\n")
        r = subprocess.run(["xcrun", "-sdk", "iphoneos", "metallib", "-o", lib, f"@{rsp}"], env=ENV, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[:2000], file=sys.stderr)
            return 1
    total = sum(os.path.getsize(p) for p in glob.glob(os.path.join(OUT, "*.metallib")))
    print(f"linked {SHARDS} libraries, {total / 1e6:.0f} MB, in {time.time() - t1:.0f}s: {OUT}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
