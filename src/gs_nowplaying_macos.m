// macOS: the MediaPlayer framework's remote command centre, which hands the media keys, the
// Control Centre's buttons and AirPods' taps to the app playing now, and its now-playing
// information, which the Control Centre shows. Its handlers run on the main thread.
#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>

#include "gs_nowplaying.h"

static Uint32 event;
static NSMutableArray *targets;  // (what each command was given, to take back at the end)

static void hear(MPRemoteCommand *command, int code) {
    [command setEnabled:YES];
    id target = [command addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *e) {
        SDL_Event pushed = { .user = { .type = event, .code = code } };
        SDL_PushEvent(&pushed);
        return MPRemoteCommandHandlerStatusSuccess;
    }];
    [targets addObject:@[ command, target ]];
}

Uint32 gs_nowplaying_init(SDL_Window *win, const char *name, const char *id) {
    if (event) return event;
    event = SDL_RegisterEvents(1);
    if (!event) return 0;
    targets = [NSMutableArray array];
    MPRemoteCommandCenter *c = [MPRemoteCommandCenter sharedCommandCenter];
    hear(c.playCommand, GS_NOWPLAYING_PLAY), hear(c.pauseCommand, GS_NOWPLAYING_PAUSE), hear(c.togglePlayPauseCommand, GS_NOWPLAYING_TOGGLE);
    hear(c.nextTrackCommand, GS_NOWPLAYING_NEXT), hear(c.previousTrackCommand, GS_NOWPLAYING_PREVIOUS), hear(c.stopCommand, GS_NOWPLAYING_STOP);
    return event;
}

void gs_nowplaying_set(const char *title, const char *artist, int state) {
    if (!event) return;
    MPNowPlayingInfoCenter *now = [MPNowPlayingInfoCenter defaultCenter];
    if (state == GS_NOWPLAYING_STOPPED) {
        now.nowPlayingInfo = nil, now.playbackState = MPNowPlayingPlaybackStateStopped;
        return;
    }
    NSMutableDictionary *info = [NSMutableDictionary dictionary];
    if (title) info[MPMediaItemPropertyTitle] = [NSString stringWithUTF8String:title];
    if (artist) info[MPMediaItemPropertyArtist] = [NSString stringWithUTF8String:artist];
    now.nowPlayingInfo = info;
    now.playbackState = state == GS_NOWPLAYING_PLAYING ? MPNowPlayingPlaybackStatePlaying : MPNowPlayingPlaybackStatePaused;
}

void gs_nowplaying_quit(void) {
    if (!event) return;
    for (NSArray *t in targets) [(MPRemoteCommand *)t[0] removeTarget:t[1]];
    [targets removeAllObjects];
    [MPNowPlayingInfoCenter defaultCenter].nowPlayingInfo = nil;
    event = 0;
}
