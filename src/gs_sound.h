// A whole sound held in memory, played through gs_mix: a voice note, a ringtone, an attachment. It is
// decoded as it plays, a packet at a time on the mixer's thread, so minutes of audio cost no more
// memory than the file itself.
//
// It plays:
// - MP3 (MPEG audio layers 1 to 3), with minimp3;
// - Ogg Opus, with libopus;
// - AAC, both in a plain MP4 or M4A file (one with its sample table in a moov box, as phones record
//   it; a fragmented one is gs_mp4's) and as a stream of ADTS frames, with libavcodec, which gesso
//   has only when built with -Dvideo. Without it gs_sound_open returns NULL for these two.
//
// Add gs_sound_render to the mix with the sound as its user pointer (gs_mix_add) and it plays while
// it is set playing. Every call here may be made from any thread while the mixer runs.
//
// An app that draws only when something happens has nothing to wake it while a sound plays: ask for
// frames while gs_sound_playing is true (gs_ui_wake(ui, 50), say), draw the position, and notice
// gs_sound_ended in one of those frames.
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct gs_sound gs_sound;

// Opens a sound from its file's bytes, to be played at the mix's rate (gs_mix_rate()). The bytes
// are read as it plays and must stay as they are until gs_sound_free. NULL when the bytes are not
// one of the formats above. It is opened paused, at its start.
gs_sound *gs_sound_open(const void *data, size_t len, int rate);
void gs_sound_free(gs_sound *s);  // remove it from the mix first

double gs_sound_length(const gs_sound *s);    // seconds
void gs_sound_play(gs_sound *s, bool playing);  // played again after it ended, it starts over
bool gs_sound_playing(gs_sound *s);
void gs_sound_seek(gs_sound *s, double seconds);
double gs_sound_position(gs_sound *s);  // seconds, of what has been handed to the mixer
bool gs_sound_ended(gs_sound *s);       // it played to its end, and stopped there

void gs_sound_render(void *sound, float *lr, int frames);  // a gs_mix_fn: adds
