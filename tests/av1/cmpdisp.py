#!/usr/bin/env python3
# cmpdisp.py hw.framemd5 out.idx exp.txt sw.framemd5 — compare MPP outputs (by packet pts) with dav1d's display order
import sys
hw=[l.split(',')[5].strip() for l in open(sys.argv[1]) if l.startswith('0,')]
idx=[int(l) for l in open(sys.argv[2]) if l.strip()]
exp=[int(l) for l in open(sys.argv[3]) if l.strip()]
sw=[l.split(',')[5].strip() for l in open(sys.argv[4]) if l.startswith('0,')]
assert len(hw)==len(idx), f"outputs {len(hw)} vs idx {len(idx)}"
by={}
for p,m in zip(idx,hw):
    if p in by and by[p]!=m: print(f"pts {p}: two different outputs"); 
    by.setdefault(p,m)
missing=[p for p in exp if p not in by]
got=[by.get(p) for p in exp]
ok=sum(1 for a,b in zip(got,sw) if a==b)
print(f"outputs={len(hw)} distinct_pts={len(by)} expected_display={len(exp)} dav1d={len(sw)} matching={ok}/{len(sw)} missing_pts={missing[:8]}")
bad=[(i,p) for i,(p,a,b) in enumerate(zip(exp,got,sw)) if a!=b and a is not None]
if bad: print("mismatches (display#, pts):", bad[:8])
