#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 defcom5-rockchip
# insert a temporal delimiter OBU before every SEQ/FRAME/FRAME_HEADER OBU of a raw low-overhead OBU stream (makes ffmpeg's obu demuxer accept it)
import sys
def leb(b,i):
    v=0; s=0
    while True:
        c=b[i]; i+=1; v|=(c&0x7f)<<s; s+=7
        if not c&0x80: return v,i
d=open(sys.argv[1],'rb').read(); out=bytearray(); i=0
while i < len(d):
    j=i; h=d[i]; t=(h>>3)&0xf; ext=(h>>2)&1; i+=1+ext; n,i=leb(d,i); i+=n
    if t in (1,3,6): out+=b'\x12\x00'
    out+=d[j:i]
open(sys.argv[2],'wb').write(out)
