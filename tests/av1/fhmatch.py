#!/usr/bin/env python3
# fhmatch.py A.fh B.fh order_hint refresh : diff the first frame blocks (after the 250th) in A and B whose header has that order_hint and refresh_frame_flags and show_frame=1
import sys
def blocks(p):
    out=[]
    for b in open(p).read().split("\nframe ")[1:]:
        lines=b.split("\n"); f={}
        for l in lines[1:]:
            l=l.strip()
            if "=" in l and not l.startswith(("q:","seg:","lf:","cdef:","lr:","tiles:","gm:","fg:")) : k,v=l.split("=",1); f[k]=v
        out.append((f,lines))
    return out
A,B=blocks(sys.argv[1]),blocks(sys.argv[2]); oh,rf=sys.argv[3],sys.argv[4]
def find(bl):
    for i,(f,l) in enumerate(bl):
        if i>250 and f.get("order_hint")==oh and f.get("refresh_frame_flags")==rf and f.get("show_frame")=="1": return i,l
    return None,None
ia,la=find(A); ib,lb=find(B); print(f"A block {ia}, B block {ib}")
if la and lb:
    import difflib
    for d in difflib.unified_diff(la,lb,lineterm="",n=0):
        if not d.startswith(("---","+++","@@")): print("  "+d)
