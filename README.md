# gesso

Gesso is the chalk ground a painter lays on a panel before painting: the layer every picture is built on. This is a small C library on SDL3 for lean native apps and games that build for Windows, macOS, Linux (the Steam Deck included) and the web from one codebase, with installs measured in megabytes.

## Principles

Every module works on all three desktop systems: Windows, macOS and Linux. Code that differs by platform, such as system font paths, process memory and CPU figures, or file mapping, has a branch for each of the three, and a module is finished only when it builds and runs on all of them. A dependency that does not reach one of the three is not taken. Where a platform genuinely lacks a feature, this README says so and names what is missing. The web is a target as well, and a module that cannot run there says why.

## Modules

| Module | What it does |
|---|---|
| `gs_pix` | Indexed-colour frames in the VGA manner: Floyd-Steinberg dithering onto a palette, palette expansion to RGBA, colour-cycling ramps, highlight lookup tables |
| `gs_text` | Fonts through stb_truetype and a glyph atlas per SDL renderer, with text shaped by kb_text_shape: Arabic joins, Indic scripts reorder, right-to-left runs are placed in visual order, and lines drawn each frame are shaped once and kept. The system's interface font comes at a chosen weight and as a monospace face, with variable fonts (San Francisco, Noto) drawn at exactly that weight from their glyph variations. Text wraps into lines at the breaks Unicode allows, at newlines and inside words too wide for a line, optionally held to a number of lines with an ellipsis, and an app can ask whether a glyph was left out of a frame for want of its upload. Font chains fall back across the system's fonts per grapheme, emoji draw in colour from COLRv0, CBDT and sbix fonts, and glyphs can be drawn with an angle and a left-to-right reveal |
| `gs_stream` | A streaming source for `gs_mix`: a decoder thread writes, the mixer plays, with prebuffering, pause and back-pressure |
| `gs_webm` | A streaming WebM (Matroska) reader for audio: bytes in as they arrive, codec setup and frames out |
| `gs_opus` | Opus decoding (libopus) for streams |
| `gs_mix` | Audio output: one SDL stream summing any number of sources, each a function that adds stereo frames; the same mix renders offline for tests and exports, and the most recent output, as heard, is available for scopes and visualizers |
| `gs_synth` | SoundFont 2 / SF3 synthesizer (Vorbis samples via stb_vorbis, Opus samples via libopus): zones, generators and modulators, envelopes, vibrato, low-pass filter, pan, loops, a reverb whose room length and level crossfade, per-channel mute, tagged notes (release exactly one note's voices); banks can also be built in code, with several renderings ("takes") per sample swapped in while playing |
| `gs_seq` | A timeline of timed events played sample-accurately: notes, MIDI and calls into your code at exact times, a feeder that keeps it filled ahead (a generative score), and other sources rendered in step; a `gs_mix` source |
| `gs_dsp` | Building blocks for synthesized sound: FFT, Web Audio's biquads, resonators, an extended Karplus-Strong plucked string and a bowed string (after the Synthesis ToolKit) |
| `gs_jobs` | A small worker-thread pool (runs jobs inline where threads are unavailable) |
| `gs_midi` | Standard MIDI Files and a sample-accurate, looping player that is a `gs_mix` source |
| `gs_stats` | A performance overlay in SDL's debug font: frame rate, frame time, process memory and CPU (Windows, Linux, macOS), audio load, the program file's size, renderer; the web shows the wasm heap and no CPU or file size |
| `gs_pace` | Frame pacing: vsync (adaptive where supported, every nth refresh for caps that divide the refresh rate), a limiter for any cap, for vsync that is off or unsupported, and for vsync the driver reports but ignores, and 10 fps while the window is minimised or hidden; on the web the browser paces |
| `gs_sha256` | SHA-256, for checking downloads against published checksums |
| `gs_json` | JSON read and written: a read-only tree with NULL-safe lookups and dotted paths for reading, and values that are built, changed, merged over defaults, and written compact or indented, to text or to a file replaced whole |
| `gs_http` | HTTPS requests from worker threads: WinHTTP on Windows, the system's libcurl (loaded at run time) on Linux and macOS, and the curl program where neither is usable; whole bodies, files or streamed ranges, with a progress hook that can give a request up |
| `gs_secret` | A small secret such as a sign-in token kept for the current user: encrypted with the Data Protection API on Windows, an owner-only file on Linux and macOS |
| `gs_oauth` | Signing in with OAuth 2.0's device authorization grant: a short code the user enters in their own browser, then polling for tokens; no embedded browser |
| `gs_hls` | HTTP Live Streaming playlists: a master playlist's renditions and a media playlist's segments, with unknown tags passed to the app |
| `gs_ts`, `gs_mp4` | Demuxers for HLS segments, MPEG transport stream and fragmented MP4: H.264 access units in Annex B form and AAC frames out, with presentation times (`gs_media.h`) |
| `gs_aac` | AAC decoding (libavcodec) to the mix's rate, shaped like `gs_opus`; with `-Dvideo` |
| `gs_video` | H.264 decoding shown on an SDL texture by a clock, one new frame a refresh: VAAPI through EGL on Linux, Direct3D 11 on Windows, VideoToolbox on macOS, without a copy through the CPU, and libavcodec in software elsewhere; with `-Dvideo` |
| `gs_live` | A live HLS player: segments fetched as they appear, demuxed, decoded and played through `gs_mix` with video following the heard audio, on one continuous timeline across discontinuities; it starts behind the live edge, skips ahead, asks the app for a fresh playlist URL and rebuffers on stalls; given a master playlist it moves between renditions by measured throughput (`gs_abr`); with `-Dvideo` |
| `gs_abr` | Choosing a stream's rendition by measured throughput (adaptive bitrate): a fast and a slow average, steps down at once and up with hysteresis, and when to give up a download that will arrive too late; the policy alone, with no network |
| `gs_ui` | An immediate-mode interface in points: one per window for apps with several windows, rectangle cutting, buttons, links, toggles, switches, radio buttons, sliders with and without stops, single-line and multi-line text fields, masked and number fields, wrapped text, dropdowns, menus, tooltips that wrap, a scrolling page of controls with a scroll position the app reads and sets, modal questions, a field that captures a hotkey as an accelerator, scrolling lists with keyboard focus and scrolling lists of rows with differing heights that follow the end of a chat, one floating layer, a Tab order with a visible focus mark and every control worked from the keyboard, controls of an app's own design in that order, disabled controls, text in several font sets (weights, monospace) and held to a number of lines, rounded corners, opacity, a progress bar, values eased over time, and the window's position and size between runs |
| `gs_draw` | Smooth shapes on an SDL renderer: rounded rectangles filled and outlined, discs, strokes with round ends and triangles, each with a rim that fades over a pixel, and an easing curve for values that move |
| `gs_desk` | An app's dealings with the desktop around it: global hotkeys from Electron-style accelerators (RegisterHotKey, Carbon, X11 grabs), a paste keystroke into the app that has the keyboard (SendInput, posted key events with macOS's Accessibility asked for, the X test extension, or wtype and ydotool under Wayland), the clipboard's whole contents put aside and put back, an overlay window that floats over other apps on every workspace without taking the keyboard, and one running instance that later ones forward their arguments to |
| `gs_image` | Thumbnails fetched over HTTPS on workers, cached compressed under a byte limit, and decoded to textures only while drawn |
| `gs_rand.h` | Seeded generators (SplitMix64, and Mulberry32 bit for bit as in JavaScript, so ported generative code plays identically), a position hash and value noise |

Apps use SDL3 directly; gesso does not wrap it, because SDL already is the platform layer. The stb libraries (public domain) are compiled once, in `gs_stb.c`, and their headers are available to apps.

## Using it

Needs Zig 0.17.0. SDL3 comes from the Zig package manager ([castholm/SDL](https://github.com/castholm/SDL)), so there is nothing else to install.

In the app's `build.zig.zon`, depend on gesso by path or by URL:

```zig
.dependencies = .{
    .gesso = .{ .path = "../gesso" },
},
```

In its `build.zig`, link the `gesso` artifact. It brings the gesso, stb and SDL3 headers and links SDL3 with it:

```zig
const gesso = b.dependency("gesso", .{ .target = target, .optimize = optimize });
app_mod.linkLibrary(gesso.artifact("gesso"));
```

Then build for Windows from any host, and for macOS from any host that has a copy of a macOS SDK (below):

```sh
zig build -Doptimize=ReleaseSmall
zig build -Doptimize=ReleaseSmall -Dtarget=x86_64-windows-gnu
```

A macOS build on another system takes `-Dmacos_sdk=PATH`, the `MacOSX.sdk` folder of an Xcode installation (copy it from a Mac, for example `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk`; about 775 MB). Apple's license allows its SDK only on Apple-branded hardware, so keep the copy out of repositories and CI caches:

```sh
zig build -Dtarget=aarch64-macos -Dmacos_sdk=$HOME/.local/share/macos-sdk/MacOSX.sdk
```

Tests built this way run on a Mac, not on the host. An app that depends on gesso passes the same option to gesso and adds the SDK's framework, include and library paths to its own modules.

`-Dvideo=true` adds the video modules (`gs_aac`, `gs_video`, `gs_live`) and builds [FFmpeg](https://ffmpeg.org/)'s libavcodec and libavutil into gesso, trimmed to H.264, AAC and each system's hardware decoding, from FFmpeg's release tarball and the configure output stored in `ffmpeg/` (made by `ffmpeg/tools/import.sh`). It is configured for x86-64 Linux and Windows and aarch64 macOS.

For the web, compile the files in `src/` together with the app with Emscripten and `-sUSE_SDL=3`, and use SDL's main callbacks (`SDL_MAIN_USE_CALLBACKS`) so the browser can run the loop.

## Deferred

Accessibility: gesso apps draw their own interface, so screen readers and other assistive technology see nothing in them. That is acceptable for games and personal tools but not for an app offered to the public. The likely route is [AccessKit](https://github.com/AccessKit/accesskit), which implements each platform's accessibility API and has C bindings: an app describes its interface as a tree of nodes, and AccessKit answers the screen reader. Revisit before a gesso app is offered as a general-purpose app, or as soon as someone who uses a screen reader wants to use one.

Web sign-in: an app that signs in to a web service through the service's own page needs a small browser window that watches for the page to finish and hands over its cookies. [gtube](https://github.com/wakamex/gtube) has one in `src/signin.c`: WebView2 on Windows, WebKit on macOS (`src/signin_mac.m`), and on Linux WebKitGTK for GTK 3 or GTK 4 in a process of its own, loaded at run time so nothing is needed to build, writing a Netscape cookie file. It becomes a gesso module when a second gesso app needs to sign in, with the start URL, the URLs that mean done, the cookie domains, the cookie that proves the sign-in and the window title as parameters, and the Google and YouTube specifics left in gtube. The web build has no use for it, since a page there signs in with the browser itself.

## Licence

MIT, see `LICENSE`. The stb libraries in `vendor/stb` are public domain (or MIT, at your choice); kb_text_shape in `vendor/kb` is zlib; the libva headers in `vendor/va` are MIT; SDL3 is zlib; libopus (fetched by the build) is BSD. With `-Dvideo`, FFmpeg (fetched by the build) is linked statically under the GNU Lesser General Public License 2.1 or later: an app that ships it ships FFmpeg's source with its binaries, or a way to relink them.
