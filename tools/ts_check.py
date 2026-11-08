#!/usr/bin/env python3
"""TS sanity check: sync bytes, TEI, PIDs, continuity errors (usage: ts_check.py file.ts)"""
import sys
d = open(sys.argv[1], 'rb').read()
n = len(d) // 188
sync = tei = cc_err = 0
pids, last = {}, {}
for i in range(n):
    p = d[i * 188: i * 188 + 188]
    if p[0] != 0x47:
        continue
    sync += 1
    if p[1] & 0x80:
        tei += 1
        continue
    pid = ((p[1] & 0x1f) << 8) | p[2]
    pids[pid] = pids.get(pid, 0) + 1
    if pid != 0x1FFF and p[3] & 0x10:
        cc = p[3] & 15
        if pid in last and cc != (last[pid] + 1) & 15 and cc != last[pid]:
            cc_err += 1
        last[pid] = cc
print(f"{n} packets, sync {sync}, TEI {tei}, continuity errors {cc_err}, PIDs " +
      ", ".join(f"0x{k:x}:{v}" for k, v in sorted(pids.items())))
