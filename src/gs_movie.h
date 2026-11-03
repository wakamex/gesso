// A video file held in memory, played in place: an attachment in a chat, a clip in a list. It opens
// as a still of its first frame holding little memory, and decodes only once it is played, on a
// thread of its own, with its picture following its sound. Needs gesso built with -Dvideo.
//
// It plays MP4 files, plain (the sample table in a moov box, as phones and messengers make them) or
// fragmented (an empty moov, then moof and mdat pairs, as web downloads and screen recorders write
// them), with H.264 video and, if there is sound, AAC. Other video (H.265, as
// iPhones can record) is not something gesso decodes, and gs_movie_open returns NULL for it.
//
// Call these on the thread that renders, except gs_movie_render, which the mixer calls. For hardware
// decoding the app calls gs_video_prepare before creating its window, as for gs_video; without that,
// or on a renderer it did not name, movies decode in software, which any renderer shows.
//
// An app that draws only when something happens has nothing to wake it while a movie plays, or while
// a paused one is still reaching the frame it is to show: ask for frames while gs_movie_playing or
// gs_movie_waiting is true (gs_ui_wake(ui, 16), say), and notice gs_movie_ended in one of them.
#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

#include "gs_video.h"

typedef struct gs_movie gs_movie;

// Opens a movie from its file's bytes, to be drawn with `r` and heard at the mix's rate
// (gs_mix_rate()). The bytes are read as it plays and must stay as they are until gs_movie_free.
// NULL when they are not a file it plays. It opens paused at its start, with nothing decoded.
gs_movie *gs_movie_open(const void *data, size_t len, SDL_Renderer *r, int rate);
void gs_movie_free(gs_movie *m);  // remove it from the mix first

// Its picture's size as it is to be seen (a phone's upright recording is stored on its side and
// turned), and its length in seconds: known from opening, for laying it out.
void gs_movie_size(const gs_movie *m, int *width, int *height);
double gs_movie_length(const gs_movie *m);
bool gs_movie_has_sound(const gs_movie *m);

// Played again after it ended, it starts over. Started, and after a seek while it plays, it holds
// where it is, its sound too, until the decoder shows the frame for that place, and then runs from
// there: the picture never trails its clock, at the cost of a moment's wait on a slow machine.
void gs_movie_play(gs_movie *m, bool playing);
bool gs_movie_playing(const gs_movie *m);
// True while what gs_movie_draw shows is not yet the frame for the movie's place: before its first
// frame is decoded, and after a seek, when the decoder works forward from the key frame before the
// place and each frame drawn shows how far it has come (a file with one key frame takes longest).
// Keep drawing until it is false, playing or not.
bool gs_movie_waiting(const gs_movie *m);
void gs_movie_seek(gs_movie *m, double seconds);
double gs_movie_position(const gs_movie *m);  // seconds
bool gs_movie_ended(const gs_movie *m);       // it played to its end, and went back to its first frame, paused
// Back to its first frame, paused, with its decoder let go: what a movie no longer in view, or put
// aside for another, should be told. Pausing alone keeps the decoder, to go on at once.
void gs_movie_rest(gs_movie *m);
void gs_movie_set_loop(gs_movie *m, bool loop);  // at its end it starts again (a clip sent as a GIF)
void gs_movie_set_muted(gs_movie *m, bool muted);

// Draws the frame for this moment into `dst` (pixels), turned the way the recording says: the first
// frame while it rests, the frame its clock has reached while it plays or is paused. False while
// there is nothing to draw yet, which is the first call or two after opening: the first frame is
// decoded when first asked for.
bool gs_movie_draw(gs_movie *m, SDL_FRect dst);
// The same frame for an app that draws it itself: the texture, the part of it holding the picture,
// and the quarter turns clockwise it is to be shown with. NULL as above.
SDL_Texture *gs_movie_frame(gs_movie *m, SDL_FRect *src, int *turns);

// How it is being decoded: gs_video's figures for the decoder it has, as of the last frame drawn. The
// path says whether the system's hardware decodes it ("d3d11va", "vaapi", "videotoolbox") or
// libavcodec ("software"). At rest they are those of the last decoder it had, and the path is "none"
// before it has had one.
gs_video_info gs_movie_get_info(const gs_movie *m);

// Its sound, a gs_mix_fn that adds: add it to the mix with the movie as its user pointer
// (gs_mix_add) if gs_movie_has_sound. A movie with sound that is not in the mix, or is muted or
// looping, keeps time by the clock instead.
void gs_movie_render(void *movie, float *lr, int frames);
