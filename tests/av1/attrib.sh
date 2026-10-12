#!/bin/bash
# Attribute each failed Fluster vector: driver (VA-API re-serialiser) vs raw MPP (original stream, av1mpp --mode tu), both against dav1d.
# usage: FLUSTER=~/fluster LIBVA_DRIVERS_PATH=<dir with rockchip_drv_video.so> TOOLS=<dir with av1mpp, nv15unpack> \
#        CSVS="fluster-*.csv" ./attrib.sh > attrib.out      (needs ffmpeg with libdav1d + vaapi, ffprobe)
# Columns: frames from dav1d / the driver / raw MPP, then frames differing from dav1d for the driver and for raw MPP.
FLUSTER=${FLUSTER:-$HOME/fluster}; TOOLS=${TOOLS:-$(cd "$(dirname "$0")" && pwd)}; CSVS=${CSVS:-$PWD/fluster-*.csv}
: "${LIBVA_DRIVERS_PATH:?set LIBVA_DRIVERS_PATH to the directory holding rockchip_drv_video.so}"
export LIBVA_DRIVERS_PATH LIBVA_DRIVER_NAME=rockchip
W=${W:-$PWD/attrib}; mkdir -p "$W"; CSVLIST=$(ls $CSVS)
cd "$FLUSTER/resources" || { echo "no Fluster resources at $FLUSTER/resources"; exit 1; }
cmpf(){ paste <(grep "^0," $1 | cut -d, -f6) <(grep "^0," $2 | cut -d, -f6) | awk '{if ($1 != $2) n++} END {print (n ? n : 0)}'; }
nfr(){ grep -c "^0," $1; }
printf "%-34s %5s %5s %5s | %8s %8s\n" vector dav1d drv raw "drv!=ref" "raw!=ref"
for csv in $CSVLIST; do
  grep -E ",(Fail|Error)," $csv | cut -d, -f1 | sed 's/\.ivf$//' | while read v; do
    f=$(find . -type f -name "$v.ivf" | head -1); [ -z "$f" ] && f=$(find . -type f -path "*/$v.ivf/*.ivf" | head -1); [ -z "$f" ] && { echo "$v: not found"; continue; }
    read w h pf < <(ffprobe -v error -show_entries stream=width,height,pix_fmt -of csv=p=0 "$f" | tr ',' ' ')
    case $pf in yuv420p) hwf=nv12; swf=yuv420p;; yuv420p10le) hwf=p010; swf=yuv420p10le;; *) printf "%-34s %s (skipped: %s)\n" "$v" "-" "$pf"; continue;; esac
    fg=""; case $v in *film_grain*|*filmgrain*) fg="-filmgrain 0";; esac
    ffmpeg -hide_banner -loglevel error -y -c:v libdav1d $fg -i "$f" -pix_fmt $swf -f framemd5 $W/$v.ref </dev/null 2>/dev/null
    timeout 60 ffmpeg -hide_banner -loglevel error -y -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -hwaccel_output_format vaapi -i "$f" -vf hwdownload,format=$hwf -pix_fmt $swf -f framemd5 $W/$v.drv </dev/null 2>$W/$v.drv.err
    info=$(timeout 60 "$TOOLS/av1mpp" --mode tu --out $W/$v.raw "$f" </dev/null 2>$W/$v.raw.err | tail -1)
    st=$(echo "$info" | grep -o "stride=[0-9]*x[0-9]*" | cut -d= -f2); hs=${st%x*}; sz=$(echo "$info" | grep -o "size=[0-9]*x[0-9]*" | cut -d= -f2)
    if [ "$hwf" = nv12 ]; then ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt nv12 -s ${sz:-${w}x${h}} -i $W/$v.raw -pix_fmt yuv420p -f framemd5 $W/$v.rawmd5 </dev/null 2>/dev/null
    else "$TOOLS/nv15unpack" $W/$v.raw $W/$v.y10 ${sz%x*} ${sz#*x} $hs 2>/dev/null; ffmpeg -hide_banner -loglevel error -y -f rawvideo -pix_fmt yuv420p10le -s $sz -i $W/$v.y10 -f framemd5 $W/$v.rawmd5 </dev/null 2>/dev/null; rm -f $W/$v.y10; fi
    printf "%-34s %5s %5s %5s | %8s %8s  %s\n" "$v" "$(nfr $W/$v.ref)" "$(nfr $W/$v.drv 2>/dev/null)" "$(nfr $W/$v.rawmd5 2>/dev/null)" "$(cmpf $W/$v.drv $W/$v.ref)" "$(cmpf $W/$v.rawmd5 $W/$v.ref)" "$(echo "$info" | grep -o "size=[^ ]*") $(grep -m1 -oE "info change" $W/$v.raw.err)"
    rm -f $W/$v.raw
  done
done
