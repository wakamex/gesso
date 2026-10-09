#!/bin/sh
# Makes a stream in three renditions for testing adaptive play (needs ffmpeg), in OUT_DIR:
#   540p (about 2.5 Mbit/s), 360p (1 Mbit/s) and 180p (300 kbit/s), 30 fps, 60 s each, in 2 s
#   transport stream segments with a keyframe at each start and the same timestamps and tone, as
#   Twitch's renditions have. Each folder holds seg0.ts... and `bandwidth` and `resolution` files for live.py.
#   sh ladder.sh OUT_DIR      then   python3 live.py OUT_DIR 18770 2 --rate 4000,800@40
set -eu
out=${1:?output directory}
q="-hide_banner -loglevel error -y"
for r in "540p 960x540 2500k" "360p 640x360 1000k" "180p 320x180 300k"; do
    set -- $r
    mkdir -p "$out/$1"
    ffmpeg $q -f lavfi -i "testsrc2=size=$2:rate=30" -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 60 \
        -c:v libx264 -profile:v main -g 60 -keyint_min 60 -sc_threshold 0 -b:v "$3" -maxrate "$3" -bufsize "$3" \
        -c:a aac -b:a 64k -ac 2 -output_ts_offset 10 \
        -f hls -hls_time 2 -hls_list_size 0 -hls_segment_filename "$out/$1/seg%d.ts" "$out/$1/index.m3u8"
    echo "${3%k}000" | awk '{print $1 + 64000}' > "$out/$1/bandwidth"
    echo "$2" > "$out/$1/resolution"
done
