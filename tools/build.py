#!/usr/bin/env python3
"""Incremental parallel build of generated shards and the runtime into build/Arkham.app, with build/arkham pointing at its executable."""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import glob
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = dict(os.environ, DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer")
COMMON = ["-std=gnu11", "-fno-strict-aliasing", "-fwrapv", "-w", "-fno-stack-protector", "-fno-exceptions", "-funwind-tables",
          "-fno-math-errno", "-ffp-contract=off", "-mcpu=apple-m1", "-arch", "arm64", "-mmacosx-version-min=14.0",
          f"-I{ROOT}/runtime"]
FRAMEWORKS = ["CoreFoundation", "CoreServices", "Foundation", "AppKit", "OpenGL", "ApplicationServices", "Carbon", "ImageIO", "CoreGraphics", "CoreText", "AudioToolbox", "AudioUnit", "CoreAudio", "Metal", "GameController", "CoreHaptics", "QuartzCore", "MetalFX"]


EXTRA: list[str] = []
IOS_LEAVES_OUT = {"gl_queue.c", "pipelines.c", "shims_gl.c"}


def stale(src: str, obj: str, deps: list[str]) -> bool:
    if not os.path.exists(obj):
        return True
    t = os.path.getmtime(obj)
    return os.path.getmtime(src) > t or any(os.path.getmtime(d) > t for d in deps)


def compile_one(job):
    src, obj, opt = job
    t0 = time.time()
    r = subprocess.run(["xcrun", "clang", "-c", opt, *COMMON, *EXTRA, src, "-o", obj], env=ENV, capture_output=True, text=True)
    return src, r.returncode, r.stderr, time.time() - t0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-O", dest="opt", default="2")
    ap.add_argument("-j", dest="jobs", type=int, default=os.cpu_count())
    ap.add_argument("--only", default="", help="compile only shards matching this glob and stop")
    ap.add_argument("--serial", action="store_true", help="for ARKHAM_SERIAL=1 debugging: objects and binary in build/serial")
    ap.add_argument("--define", action="append", default=[], help="an experiment's macro, as -D: its objects go in a folder of their own, so the usual build isn't redone")
    ap.add_argument("--ios", action="store_true", help="for the iPhone: everything into build/ios/libarkham.a, which the app in ios/app links")
    args = ap.parse_args()
    if args.ios:
        sdk = subprocess.run(["xcrun", "--sdk", "iphoneos", "--show-sdk-path"], env=ENV, capture_output=True, text=True).stdout.strip()
        COMMON[:] = [f for f in COMMON if f not in ("-mcpu=apple-m1", "-arch", "arm64", "-mmacosx-version-min=14.0")]
        COMMON.extend(["-target", "arm64-apple-ios18.0", "-isysroot", sdk, "-mcpu=apple-a14"])
    opt = f"-O{args.opt}"
    objdir = os.path.join(ROOT, "build", "serial" if args.serial else ("ios-obj" if args.ios else "obj") + args.opt + "".join("-" + d.split("=")[0].lower() for d in args.define))
    EXTRA.extend(f"-D{d}" for d in args.define)
    if args.serial:
        EXTRA.append("-DARKHAM_POLL")
    os.makedirs(objdir, exist_ok=True)
    headers = glob.glob(f"{ROOT}/runtime/*.h")
    gen_headers = [f"{ROOT}/runtime/rt.h", f"{ROOT}/runtime/rt_ops.h"]  # shards depend on these alone
    jobs, objs = [], []
    gen = sorted(glob.glob(f"{ROOT}/gen/{args.only or '*'}.c"))
    for src in gen:
        obj = os.path.join(objdir, os.path.basename(src)[:-2] + ".o")
        objs.append(obj)
        if stale(src, obj, gen_headers):
            jobs.append((src, obj, opt))
    if not args.only:
        for src in sorted(glob.glob(f"{ROOT}/runtime/*.c") + glob.glob(f"{ROOT}/runtime/*.m")):
            name = os.path.basename(src)
            # Feral's OpenGL path has no place on the phone: ios_gl.c stands in for these three there, and only there.
            if args.ios and name in IOS_LEAVES_OUT or not args.ios and name.startswith("ios_"):
                continue
            obj = os.path.join(objdir, "rt_" + os.path.basename(src)[:-2] + ".o")
            objs.append(obj)
            if stale(src, obj, headers):
                jobs.append((src, obj, "-O2"))
    t0 = time.time()
    failed = 0
    slowest = (0.0, "")
    if jobs:
        with cf.ThreadPoolExecutor(args.jobs) as pool:
            for k, (src, rc, err, dt) in enumerate(pool.map(compile_one, jobs), 1):
                slowest = max(slowest, (dt, os.path.basename(src)))
                if rc:
                    failed += 1
                    if failed <= 3:
                        print(f"FAILED {src}\n{err[:3000]}", file=sys.stderr)
                if k % 40 == 0:
                    print(f"  {k}/{len(jobs)} {time.time() - t0:.0f}s", flush=True)
    print(f"compiled {len(jobs)} files in {time.time() - t0:.0f}s, failed={failed}, slowest={slowest[1]} {slowest[0]:.0f}s", flush=True)
    if failed:
        return 1
    if args.only:
        return 0
    if args.ios:
        lib = os.path.join(ROOT, "build", "ios", "libarkham.a")
        os.makedirs(os.path.dirname(lib), exist_ok=True)
        t1 = time.time()
        rsp = os.path.join(ROOT, "build", "ios", "objs.rsp")
        with open(rsp, "w") as fh:
            fh.write("\n".join(objs))
        if os.path.exists(lib):
            os.remove(lib)
        r = subprocess.run(["xcrun", "libtool", "-static", "-no_warning_for_no_symbols", "-o", lib, "-filelist", rsp], env=ENV, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[:4000], file=sys.stderr)
            return 1
        print(f"archived {lib} {os.path.getsize(lib) / 1e6:.0f} MB in {time.time() - t1:.0f}s")
        return 0
    t1 = time.time()
    rsp = os.path.join(ROOT, "build", "link.rsp")
    with open(rsp, "w") as fh:
        fh.write("\n".join(objs))
    # The game lives in an app bundle, build/Arkham.app, because macOS only turns Game Mode on for an app marked as a game. build/arkham points at it.
    # Metal's shader cache depends on the executable's path, so the shaders are built ahead for this one: tools/prebuild_shaders.sh.
    app = os.path.join(ROOT, "build", "Arkham.app", "Contents")
    os.makedirs(os.path.join(app, "MacOS"), exist_ok=True)
    out = os.path.join(ROOT, "build", "serial", "arkham") if args.serial else os.path.join(app, "MacOS", "arkham")
    # With a bundle identifier Metal keeps its shader cache in a folder of its own, instead of the one most of the system shares.
    bundle = ["-Wl,-sectcreate,__TEXT,__info_plist," + os.path.join(ROOT, "runtime", "Info.plist")]
    cmd = ["xcrun", "clang", "-arch", "arm64", "-mmacosx-version-min=14.0", f"@{rsp}", "-o", out, "-Wl,-w", "-lz",
           "-Wl,-stack_size,0x10000000", *bundle]
    for fw in FRAMEWORKS:
        cmd += ["-framework", fw]
    r = subprocess.run(cmd, env=ENV, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:4000], file=sys.stderr)
        return 1
    print(f"linked {out} {os.path.getsize(out) / 1e6:.0f} MB in {time.time() - t1:.0f}s")
    if not args.serial:
        import plistlib
        with open(os.path.join(ROOT, "runtime", "Info.plist"), "rb") as fh:
            info = plistlib.load(fh)
        info.update({"CFBundleExecutable": "arkham", "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "1.0", "CFBundleVersion": "1",
                     "LSMinimumSystemVersion": "14.0", "NSHighResolutionCapable": True})
        with open(os.path.join(app, "Info.plist"), "wb") as fh:
            plistlib.dump(info, fh)
        link = os.path.join(ROOT, "build", "arkham")
        if os.path.islink(link) or not os.path.exists(link) or os.path.isfile(link):
            if os.path.lexists(link):
                os.remove(link)
            os.symlink(os.path.join("Arkham.app", "Contents", "MacOS", "arkham"), link)
    tool, tool_src = os.path.join(ROOT, "build", "warm_shaders"), os.path.join(ROOT, "tools", "warm_shaders.c")
    if not args.serial and stale(tool_src, tool, []):
        r = subprocess.run(["xcrun", "clang", "-O2", "-arch", "arm64", "-mcpu=apple-m1", "-mmacosx-version-min=14.0", "-Wno-deprecated-declarations", tool_src, "-framework", "OpenGL", *bundle, "-o", tool],
                           env=ENV, capture_output=True, text=True)
        if r.returncode:
            print(r.stderr[:2000], file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
