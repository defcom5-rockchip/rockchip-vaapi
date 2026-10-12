#!/usr/bin/env python3
# list OBUs per IVF frame (or per raw OBU stream): type, size, and for frame headers the first bits
import sys, struct
OBU={1:"SEQ",2:"TD",3:"FRAME_HDR",4:"TILE_GRP",5:"META",6:"FRAME",7:"REDUNDANT_FH",8:"TILE_LIST",15:"PAD"}
def leb(b,i):
    v=0; s=0
    while True:
        c=b[i]; i+=1; v|=(c&0x7f)<<s; s+=7
        if not c&0x80: return v,i
def bits(b):  # MSB-first bit reader
    for byte in b:
        for k in range(7,-1,-1): yield (byte>>k)&1
def fh_summary(p, reduced_still=False):
    g=bits(p); nb=lambda n: sum(next(g)<<(n-1-i) for i in range(n))
    show_existing=nb(1)
    if show_existing: return f"show_existing slot={nb(3)}"
    ft=nb(2); show=nb(1); s=f"type={['KEY','INTER','INTRA_ONLY','SWITCH'][ft]} show={show}"
    if show: s+=f" (error_res={nb(1) if ft!=0 else 1})"
    else: s+=f" showable={nb(1)}"
    return s
def walk(payload, tag):
    i=0; out=[]
    while i < len(payload):
        h=payload[i]; t=(h>>3)&0xf; ext=(h>>2)&1; has=(h>>1)&1; i+=1+ext
        n,i=leb(payload,i) if has else (len(payload)-i,i)
        body=payload[i:i+n]; i+=n
        d=OBU.get(t,str(t))
        if t in (3,6,7): d+=f"[{fh_summary(body)}]"
        out.append(f"{d}:{n}")
    print(f"{tag}: "+" ".join(out))
data=open(sys.argv[1],'rb').read()
if data[:4]==b'DKIF':
    hl=struct.unpack('<H',data[6:8])[0]; i=hl; n=0
    while i+12<=len(data):
        sz,pts=struct.unpack('<IQ',data[i:i+12]); i+=12; walk(data[i:i+sz],f"TU{n}"); i+=sz; n+=1
else:
    walk(data,"stream")
