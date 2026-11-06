#!/usr/bin/env python3
"""Sample the running game and print each thread's guest stack, innermost last: tools/stacks.py [keep_frames]"""
import os
import re
import subprocess
import sys
import tempfile

keep = int(sys.argv[1]) if len(sys.argv) > 1 else 8
pid = subprocess.run(["pgrep", "-x", "arkham"], capture_output=True, text=True).stdout.split()
if not pid:
    print("not running")
    sys.exit(1)
pid = pid[0]
print(subprocess.run(["ps", "-o", "rss=,%cpu=", "-p", pid], capture_output=True, text=True).stdout.strip())
with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as tf:
    path = tf.name
subprocess.run(["sample", pid, "1", "-file", path], capture_output=True)
text = open(path, errors="replace").read()
os.unlink(path)
part = text[text.index("Call graph:"):text.index("Total number in stack")]
groups = {}
for th in re.split(r"\n\s+\d+ Thread_", part)[1:]:
    head, *lines = th.splitlines()
    frames = []
    for line in lines:
        depth = len(line) - len(line.lstrip(" +!:|"))
        m = re.match(r"^[ +!:|]*(\d+) (.*?)\s+\(in ([^)]*)\)", line)
        if not m:
            continue
        count, name, lib = int(m.group(1)), m.group(2), m.group(3)
        frames.append((depth, count, name, lib))
    # Follow the heaviest path through the call tree.
    path_frames = []
    cur_depth = -1
    i = 0
    while i < len(frames):
        d, n, name, lib = frames[i]
        if d > cur_depth:
            path_frames.append((name, lib))
            cur_depth = d
        i += 1
        # Stop once the tree branches back above the current path.
        if i < len(frames) and frames[i][0] <= d:
            break
    guest = [f"{n}" if lib == "arkham" else f"{n}@{lib.split('.')[0]}" for n, lib in path_frames if lib == "arkham" or n.startswith("__psynch") or n.startswith("mach_")]
    key = " ".join(guest[-keep:])
    if guest:
        groups[key] = groups.get(key, 0) + 1
for k, v in groups.items():
    print(f"x{v}  {k}"[:400])
