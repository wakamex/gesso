#!/bin/sh
# Makes the sounds gs_sound's tests play (run from this folder; needs ffmpeg with libmp3lame and
# libopus): two seconds each, a 440 Hz tone for the first second and an 880 Hz tone for the second,
# so a test can tell where in the sound it is, in each format a voice note arrives in:
#   tone.m4a   AAC in a plain MP4 (mono, 44.1 kHz), as phones record
#   tone.aac   AAC as ADTS frames (stereo, 48 kHz)
#   tone.mp3   MP3 (mono, 44.1 kHz), with an ID3 tag in front
#   tone.ogg   Ogg Opus (mono, 48 kHz)
set -eu
q="-hide_banner -loglevel error -y"
src="-f lavfi -i sine=frequency=440:duration=1 -f lavfi -i sine=frequency=880:duration=1 -filter_complex [0][1]concat=n=2:v=0:a=1"
ffmpeg $q $src -ar 44100 -ac 1 -c:a aac -b:a 48k -movflags -faststart tone.m4a
ffmpeg $q $src -ar 48000 -ac 2 -c:a aac -b:a 64k -f adts tone.aac
ffmpeg $q $src -ar 44100 -ac 1 -c:a libmp3lame -b:a 48k -id3v2_version 3 -metadata title=tone tone.mp3
ffmpeg $q $src -ar 48000 -ac 1 -c:a libopus -b:a 24k tone.ogg
