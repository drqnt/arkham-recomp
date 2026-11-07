#!/usr/bin/env python3
"""Translates every shader in the game's caches and compiles the results with the system's Metal compiler: tools/check_shaders.py [how many, 0 for all]
Those compiles go to a cache of the checker's own, so they don't help the game. tools/prebuild_shaders.sh does that. "translate" as the argument stops before compiling."""
import glob
import os
import random
import shutil
import subprocess
import sys
import collections as C

sys.path.insert(0, os.path.dirname(__file__))
from game_path import game_root

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = dict(os.environ, DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer")
translate_only = sys.argv[1:] == ["translate"]
limit = 0 if translate_only else int(sys.argv[1]) if len(sys.argv) > 1 else 300
out = os.path.join(ROOT, "logs", "msl")
tool = os.path.join(ROOT, "build", "d3d2msl")
subprocess.run(["xcrun", "clang", "-O1", "-std=gnu11", os.path.join(ROOT, "tools", "d3d2msl.c"), os.path.join(ROOT, "runtime", "d3d_shader.c"), "-o", tool], env=ENV, check=True)
shutil.rmtree(out, ignore_errors=True)
base = os.path.join(game_root(), "BMAAData/BmGame/CookedPC")
r = subprocess.run([tool, out, os.path.join(base, "GlobalShaderCache-PC-D3D-SM3.bin"), os.path.join(base, "RefShaderCache-PC-D3D-SM3.upk")], env=ENV, capture_output=True, text=True)
print(r.stdout.strip())
print(C.Counter(l for l in r.stderr.splitlines() if l.startswith("error")).most_common(12))
files = sorted(glob.glob(out + "/*.metal"))
random.seed(1)
if limit and limit < len(files):
    files = random.sample(files, limit)
listing = os.path.join(out, "files.txt")
open(listing, "w").write("\n".join(files) + "\n")
if translate_only:
    sys.exit(0)

checker = os.path.join(ROOT, "build", "check_msl")
subprocess.run(["xcrun", "clang", "-O1", "-fobjc-arc", "-fmodules", os.path.join(ROOT, "tools", "check_msl.m"), "-framework", "Foundation", "-framework", "Metal", "-o", checker], env=ENV, check=True)
r = subprocess.run([checker, listing], env=ENV, capture_output=True, text=True)
print(r.stdout[-6000:])
