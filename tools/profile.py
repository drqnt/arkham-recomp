#!/usr/bin/env python3
"""Sample the running game and print where each busy thread spends its time: tools/profile.py [seconds] [top]"""
import collections
import re
import subprocess
import sys
import tempfile

secs = sys.argv[1] if len(sys.argv) > 1 else "5"
top = int(sys.argv[2]) if len(sys.argv) > 2 else 14
pid = subprocess.run(["pgrep", "-x", "arkham"], capture_output=True, text=True).stdout.split()
if not pid:
    sys.exit("not running")
with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as tf:
    path = tf.name
subprocess.run(["sample", pid[0], secs, "5", "-file", path], capture_output=True)
text = open(path, errors="replace").read()
part = text[text.index("Call graph:"):text.index("Total number in stack")]
threads = re.split(r"\n\s+(?=\d+ Thread_)", part)[1:]
line_re = re.compile(r"^([ +!:|]*)(\d+) (.*?)\s+\(in ([^)]*)\)")
idle_re = re.compile(r"cvwait|__ulock_wait|semaphore_.*wait.*trap")


def head_key(h):
    return h.strip()[:50]


results = []
wait_chains = {}
for th in threads:
    lines = th.splitlines()
    head = lines[0]
    total = int(head.split()[0])
    # Self time of a node is its count minus its children's.
    stack, self_time = [], collections.Counter()
    incl = collections.Counter()
    first = None
    for ln in lines[1:]:
        m = line_re.match(ln)
        if not m:
            continue
        depth, count, name, lib = len(m.group(1)), int(m.group(2)), m.group(3), m.group(4)
        name = re.sub(r" \+ .*$", "", name)
        if first is None and lib == "arkham" and name.startswith("F_"):
            first = name
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if name.startswith("shim_") and not any(f[1].startswith("shim_") for f in stack):
            incl[name] += count
        if stack:
            self_time[stack[-1][1]] -= count
        self_time[name + ("" if lib == "arkham" else " @" + lib.split(".")[0])] += count
        stack.append((depth, name + ("" if lib == "arkham" else " @" + lib.split(".")[0])))
        # Remember the heaviest chain ending in a wait, to show what the thread waits for.
        if idle_re.search(name):
            key = head_key(lines[0])
            if key not in wait_chains or count > wait_chains[key][0]:
                wait_chains[key] = (count, [f[1] for f in stack][-9:])
    results.append((total, head[:60], first, self_time, incl))
idle = re.compile(r"cvwait|mach_msg|__semwait|semaphore_.*wait.*trap|kevent|__select|__workq|nanosleep|__ulock_wait|start_wqthread")
print("Threads by samples spent running or waiting for the gate:")
for total, head, first, st, incl in sorted(results, key=lambda r: -sum(v for k, v in r[3].items() if not idle.search(k))):
    busy = sum(v for k, v in st.items() if not idle.search(k))
    if busy < 3:
        continue
    gate = sum(v for k, v in st.items() if "psynch_cvwait" in k)
    other = sum(v for k, v in st.items() if idle.search(k)) - gate
    print(f"\n{head.strip()[:50]} first={first} total={total} on-cpu={busy} gate/cond-wait={gate} other-wait={other}")
    for k, v in st.most_common(top):
        if v > 0:
            print(f"   {v:6d}  {k[:70]}")
    by_lib = collections.Counter()
    for k, v in st.items():
        by_lib["wait" if idle.search(k) else (k.split(" @")[1] if " @" in k else "translated+runtime")] += v
    print("   by lib: " + ", ".join(f"{k}={v}" for k, v in by_lib.most_common(9)))
    if incl:
        print("   shims incl: " + ", ".join(f"{k[5:]}={v}" for k, v in incl.most_common(10)))
    w = wait_chains.get(head_key(head))
    if w:
        print(f"   heaviest wait ({w[0]}): " + " > ".join(x.split(" @")[0][:26] for x in w[1]))
