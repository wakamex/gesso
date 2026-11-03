// gs_video and gs_aac where gesso has no decoder for the system (Android, for now): the video modules
// link, gs_video_library says NULL, and gs_movie_open and gs_live_start return NULL.
#include "gs_aac.h"
#include "gs_video.h"

const char *gs_video_prepare(bool hardware) { return NULL; }
gs_video *gs_video_new(SDL_Renderer *r, int queue, int threads) { return NULL; }
void gs_video_free(gs_video *v) {}
bool gs_video_decode(gs_video *v, const uint8_t *data, size_t len, double pts) { return false; }
void gs_video_flush(gs_video *v) {}
void gs_video_drain(gs_video *v) {}
void gs_video_stop(gs_video *v) {}
SDL_Texture *gs_video_frame(gs_video *v, double clock, SDL_FRect *src) { return NULL; }
gs_video_info gs_video_get_info(gs_video *v) { return (gs_video_info){ .path = "none", .next_pts = -1, .shown_pts = -1 }; }
const char *gs_video_library(void) { return NULL; }

gs_aac *gs_aac_new(const gs_aac_config *c, int out_rate) { return NULL; }
void gs_aac_free(gs_aac *a) {}
int gs_aac_decode(gs_aac *a, const uint8_t *frame, size_t len, float *lr, int max_frames) { return -1; }
void gs_aac_flush(gs_aac *a) {}
