#!/usr/bin/env python3
"""Print the longest [hitch] reports from a run log: tools/hitches.py [log] [count] [skip_first_fps_lines]"""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "logs/run.err"
count = int(sys.argv[2]) if len(sys.argv) > 2 else 6
skip = int(sys.argv[3]) if len(sys.argv) > 3 else 0
L = open(path, errors="replace").read().split("\n")
fps = [i for i, x in enumerate(L) if x.startswith("[fps]")]
start = fps[skip] if len(fps) > skip else 0
idx = [i for i, x in enumerate(L) if x.startswith("[hitch] frame took") and i > start]
idle = ("F_03f9490 < F_03f9880", "F_03264a0", "F_27a3530 <", "semaphore_wait_signal", "F_e100db80", "IOThread")
print(len(idx), "reports, longest:", sorted(int(L[i].split()[3]) for i in idx)[-16:])
for i in sorted(sorted(idx, key=lambda i: -int(L[i].split()[3]))[:count]):
    print(f"{L[i][8:60]}, second {sum(1 for f in fps if f < i)}")
    j = i + 1
    block = []
    while j < len(L) and L[j].startswith("[hitch]  "):
        block.append(L[j][8:])
        j += 1
    k = 0
    while k < len(block):
        t = block[k]
        if t.lstrip().startswith("thread "):
            lines = []
            k += 1
            while k < len(block) and not block[k].lstrip().startswith(("thread ", "slow ")):
                lines.append(block[k])
                k += 1
            if lines and not all(any(p in x for p in idle) or x.strip().endswith("?") for x in lines):
                print("  ", t.strip()[:80])
                for x in lines:
                    print("    ", x.strip()[:150])
        else:
            print("  ", t.strip()[:120])
            k += 1
