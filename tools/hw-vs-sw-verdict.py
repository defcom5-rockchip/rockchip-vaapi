import glob, struct, zlib, sys
def png_rgb(path):
    d=open(path,'rb').read(); pos=8; w=h=0; idat=b''; bpp=3
    while pos<len(d):
        ln=struct.unpack('>I',d[pos:pos+4])[0]; t=d[pos+4:pos+8]; body=d[pos+8:pos+8+ln]; pos+=12+ln
        if t==b'IHDR': w,h,bd,ct=struct.unpack('>IIBB',body[:10]); bpp={2:3,6:4}.get(ct,3)
        elif t==b'IDAT': idat+=body
        elif t==b'IEND': break
    raw=zlib.decompress(idat); stride=w*bpp; out=bytearray(); prev=bytearray(stride); i=0
    for y in range(h):
        f=raw[i]; line=bytearray(raw[i+1:i+1+stride]); i+=1+stride
        if f==0: pass
        elif f==2:
            for x in range(stride): line[x]=(line[x]+prev[x])&255
        else:
            for x in range(stride):
                a=line[x-bpp] if x>=bpp else 0; b=prev[x]; c=prev[x-bpp] if x>=bpp else 0
                if f==1: line[x]=(line[x]+a)&255
                elif f==3: line[x]=(line[x]+(a+b)//2)&255
                else:
                    p=a+b-c; pa,pb,pc=abs(p-a),abs(p-b),abs(p-c); pr=a if pa<=pb and pa<=pc else (b if pb<=pc else c); line[x]=(line[x]+pr)&255
        out+=line; prev=line
    return w,h,bpp,bytes(out)
def stats(px,bpp,step=7):
    n=0; s=[0,0,0]; ss=[0,0,0]
    for i in range(0,len(px)-3,bpp*step):
        for c in range(3): v=px[i+c]; s[c]+=v; ss[c]+=v*v
        n+=1
    mean=[s[c]/n for c in range(3)]; sd=[max(0,ss[c]/n-mean[c]**2)**0.5 for c in range(3)]; return mean,sd
hwd,swd=sys.argv[1],sys.argv[2]
hw=sorted(glob.glob(hwd+'/*.png')); sw=sorted(glob.glob(swd+'/*.png'))
if not hw: print("  NO HW FRAMES -> decoder produced nothing"); sys.exit(2)
flat=True; match=True
for k in sorted(set([0,len(hw)//2,len(hw)-1])):
    w,h,bpp,px=png_rgb(hw[k]); m,sd=stats(px,bpp)
    line=f"  HW frame {k:2d}: {w}x{h} meanRGB=({m[0]:.0f},{m[1]:.0f},{m[2]:.0f}) sd=({sd[0]:.1f},{sd[1]:.1f},{sd[2]:.1f})"
    if max(sd)>=3: flat=False
    if k<len(sw):
        _,_,bpp2,px2=png_rgb(sw[k]); n=0; ad=0
        for i in range(0,min(len(px),len(px2))-3,bpp*11):
            ad+=abs(px[i]-px2[i])+abs(px[i+1]-px2[i+1])+abs(px[i+2]-px2[i+2]); n+=3
        d=ad/n; line+=f"  | vs SW |diff|={d:.2f}/255 {'MATCH' if d<6 else 'DIFFERENT'}"
        if d>=6: match=False
    print(line)
print("  =>", "FLAT FRAME (green wall) — NOT decoding" if flat else ("REAL PICTURE, MATCHES SOFTWARE REFERENCE ✓" if match else "structured picture but DIFFERS from software (corruption?)"))
sys.exit(0 if (not flat and match) else 1)
