#!/usr/bin/env python3
# obu2ivf.py in.obu out.ivf W H : wrap a raw low-overhead OBU stream into IVF, one IVF frame per FRAME/FRAME_HEADER OBU
# (a sequence header OBU rides with the frame that follows it)
import sys, struct
def leb(b,i):
    v=0; s=0
    while True:
        c=b[i]; i+=1; v|=(c&0x7f)<<s; s+=7
        if not c&0x80: return v,i
d=open(sys.argv[1],'rb').read(); W,H=int(sys.argv[3]),int(sys.argv[4]); frames=[]; pend=b''; i=0
while i<len(d):
    j=i; h=d[i]; t=(h>>3)&0xf; ext=(h>>2)&1; i+=1+ext; n,i=leb(d,i); i+=n; pend+=d[j:i]
    if t in (3,6): frames.append(pend); pend=b''
o=open(sys.argv[2],'wb'); o.write(b'DKIF'+struct.pack('<HHIHHIIII',0,32,0x31305641,W,H,30,1,len(frames),0))
for k,f in enumerate(frames): o.write(struct.pack('<IQ',len(f),k)+f)
print(len(frames),"ivf frames")
