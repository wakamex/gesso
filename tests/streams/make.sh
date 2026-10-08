#!/bin/sh
# Makes the synthetic HLS streams gesso's tests replay (run from this folder; needs ffmpeg):
#   a/ and b/: 6 s each of a 64x36 test pattern at 30 fps with a 440 Hz tone, in 2 s segments,
#     a/ at 48 kHz, b/ at 96x54 and 44.1 kHz with timestamps starting elsewhere;
#   ts.m3u8 and mp4.m3u8 play a/ then b/ across an EXT-X-DISCONTINUITY, as transport stream and as
#     fragmented MP4 with its initialization segments;
#   wrap.ts: 4 s whose 33-bit timestamps wrap past 2^33 halfway;
#   beep.ts: a white frame and a click together every second, for measuring audio/video drift.
set -eu
q="-hide_banner -loglevel error -y"
gen() {  # out-dir size rate offset type
    mkdir -p "$1"
    ffmpeg $q -f lavfi -i "testsrc2=size=$2:rate=30" -f lavfi -i "sine=frequency=440:sample_rate=$3" -t 6 \
        -c:v libx264 -profile:v main -g 30 -b:v 40k -c:a aac -b:a 32k -ac 2 -output_ts_offset "$4" \
        -f hls -hls_time 2 -hls_segment_type "$5" -hls_list_size 0 -hls_flags independent_segments \
        -hls_fmp4_init_filename init.mp4 -hls_segment_filename "$1/seg%d.$6" "$1/index.m3u8"
}
gen a/ts 64x36 48000 10 mpegts ts
gen b/ts 96x54 44100 5000 mpegts ts
gen a/mp4 64x36 48000 10 fmp4 m4s
gen b/mp4 96x54 44100 5000 fmp4 m4s
# The two halves joined across a discontinuity.
join() {  # name dir
    {
        echo "#EXTM3U"; echo "#EXT-X-VERSION:7"; echo "#EXT-X-TARGETDURATION:2"; echo "#EXT-X-MEDIA-SEQUENCE:100"
        grep -E '^#EXT-X-MAP|^#EXTINF|^seg' a/$2/index.m3u8 | sed "s|^seg|a/$2/seg|; s|URI=\"init|URI=\"a/$2/init|"
        echo "#EXT-X-DISCONTINUITY"
        grep -E '^#EXT-X-MAP|^#EXTINF|^seg' b/$2/index.m3u8 | sed "s|^seg|b/$2/seg|; s|URI=\"init|URI=\"b/$2/init|"
        echo "#EXT-X-ENDLIST"
    } > "$1"
}
join ts.m3u8 ts
join mp4.m3u8 mp4
ffmpeg $q -f lavfi -i "testsrc2=size=64x36:rate=30" -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 4 \
    -c:v libx264 -profile:v main -g 30 -b:v 40k -c:a aac -b:a 32k -ac 2 -output_ts_offset 95441.7 -f mpegts wrap.ts
# A click and a white frame at each whole second: the drift between them is the player's error.
ffmpeg $q -f lavfi -i "color=black:size=64x36:rate=30,drawbox=c=white:t=fill:enable='lt(mod(t,1),0.034)'" \
    -f lavfi -i "aevalsrc='if(lt(mod(t,1),0.01),sin(2*PI*1000*t),0)':s=48000" -t 10 \
    -c:v libx264 -profile:v main -g 30 -b:v 30k -c:a aac -b:a 32k -ac 2 -output_ts_offset 10 -f mpegts beep.ts
