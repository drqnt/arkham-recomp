#!/usr/bin/env python3
"""Print the Bink library's disassembly from an address: tools/showbink.py ADDR [count]"""
import re
import sys

a = int(sys.argv[1], 16)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
for line in open("build/dis_bink.txt", errors="replace"):
    m = re.match(r"\s*([0-9a-f]+):\s*(.*)", line)
    if m and int(m.group(1), 16) >= a:
        print(m.group(1), re.sub(r"\s*<[^>]*>", "", m.group(2)).strip()[:70])
        n -= 1
        if n <= 0:
            break
