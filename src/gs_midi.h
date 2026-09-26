// Standard MIDI Files: read into one time-ordered event list, and play one on a gs_synth with
// sample-accurate timing, optionally looping. gs_player_render is a gs_mix_fn.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gs_synth.h"

typedef struct {
    double time;  // seconds from the start
    uint8_t status, a, b;
} gs_midi_event;

typedef struct {
    gs_midi_event *ev;
    int n;
    double length;  // seconds, to the last end-of-track
} gs_song;

bool gs_song_load(gs_song *song, const void *data, size_t len);
void gs_song_free(gs_song *song);

typedef struct {
    const gs_song *song;
    gs_synth *synth;
    int rate;
    bool loop, playing;
    double t;  // song time of the next frame
    int next;  // next event to play
} gs_player;

void gs_player_init(gs_player *p, const gs_song *song, gs_synth *synth, int rate, bool loop);
void gs_player_render(void *player, float *lr, int frames);
