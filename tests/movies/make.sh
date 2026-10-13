#!/bin/sh
# Makes the movies gs_movie's tests play (run from this folder; needs ffmpeg with libx264): two
# seconds each at 30 frames a second, red for the first second and blue for the second, so a test
# can tell where in the movie it is.
#   clip.mp4     H.264 with B-frames and AAC (a 440 Hz tone, then 880 Hz), the sample table at the end
#   turned.mp4   the same picture without sound, marked to be shown a quarter turn round, as a phone
#                held upright records
set -eu
q="-hide_banner -loglevel error -y"
video="-f lavfi -i color=red:size=320x240:rate=30:duration=1 -f lavfi -i color=blue:size=320x240:rate=30:duration=1"
audio="-f lavfi -i sine=frequency=440:duration=1 -f lavfi -i sine=frequency=880:duration=1"
ffmpeg $q $video $audio -filter_complex "[0][1]concat=n=2:v=1:a=0[v];[2][3]concat=n=2:v=0:a=1[a]" -map "[v]" -map "[a]" \
    -c:v libx264 -preset veryfast -bf 2 -g 15 -pix_fmt yuv420p -c:a aac -b:a 48k -ar 44100 -ac 1 clip.mp4
ffmpeg $q $video -filter_complex "[0][1]concat=n=2:v=1:a=0[v]" -map "[v]" -c:v libx264 -preset veryfast -bf 0 -g 15 -pix_fmt yuv420p plain.mp4
ffmpeg $q -display_rotation 270 -i plain.mp4 -c copy turned.mp4
rm plain.mp4
