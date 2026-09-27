# gesso

Gesso is the chalk ground a painter lays on a panel before painting: the layer every picture is built on. This is a small C library on SDL3 for lean native apps and games that build for Windows, Linux, the Steam Deck and the web from one codebase, with installs measured in megabytes.

## Modules

| Module | What it does |
|---|---|
| `gs_pix` | Indexed-colour frames in the VGA manner: Floyd-Steinberg dithering onto a palette, palette expansion to RGBA, colour-cycling ramps, highlight lookup tables |
| `gs_text` | Fonts through stb_truetype and a glyph atlas per SDL renderer; draws glyphs with an angle, a colour and a left-to-right reveal, or plain kerned UTF-8 lines; font chains fall back across the system's fonts for other scripts (loaded on first need; no shaping, so Arabic and Indic scripts show unjoined) |
| `gs_stream` | A streaming source for `gs_mix`: a decoder thread writes, the mixer plays, with prebuffering, pause and back-pressure |
| `gs_webm` | A streaming WebM (Matroska) reader for audio: bytes in as they arrive, codec setup and frames out |
| `gs_opus` | Opus decoding (libopus) for streams |
| `gs_mix` | Audio output: one SDL stream summing any number of sources, each a function that adds stereo frames; the same mix renders offline for tests and exports |
| `gs_synth` | SoundFont 2 / SF3 synthesizer (Vorbis samples via stb_vorbis, Opus samples via libopus): zones, generators and modulators, envelopes, vibrato, low-pass filter, pan, loops, a reverb whose room length and level crossfade, per-channel mute, tagged notes (release exactly one note's voices); banks can also be built in code, with several renderings ("takes") per sample swapped in while playing |
| `gs_seq` | A timeline of timed events played sample-accurately: notes, MIDI and calls into your code at exact times, a feeder that keeps it filled ahead (a generative score), and other sources rendered in step; a `gs_mix` source |
| `gs_dsp` | Building blocks for synthesized sound: FFT, Web Audio's biquads, resonators, an extended Karplus-Strong plucked string and a bowed string (after the Synthesis ToolKit) |
| `gs_jobs` | A small worker-thread pool (runs jobs inline where threads are unavailable) |
| `gs_midi` | Standard MIDI Files and a sample-accurate, looping player that is a `gs_mix` source |
| `gs_stats` | A performance overlay in SDL's debug font: frame rate, frame time, process memory and CPU (Windows, Linux, macOS), audio load, renderer; the web shows the wasm heap and no CPU |
| `gs_pace` | Frame pacing: vsync (adaptive where supported, every nth refresh for caps that divide the refresh rate), a limiter for any cap, for vsync that is off or unsupported, and for vsync the driver reports but ignores, and 10 fps while the window is minimised or hidden; on the web the browser paces |
| `gs_sha256` | SHA-256, for checking downloads against published checksums |
| `gs_json` | A small JSON reader: a read-only tree with NULL-safe lookups and dotted paths |
| `gs_rand.h` | Seeded generators (SplitMix64, and Mulberry32 bit for bit as in JavaScript, so ported generative code plays identically), a position hash and value noise |

Apps use SDL3 directly; gesso does not wrap it, because SDL already is the platform layer. The stb libraries (public domain) are compiled once, in `gs_stb.c`, and their headers are available to apps.

## Using it

Needs Zig 0.16.0. SDL3 comes from the Zig package manager ([castholm/SDL](https://github.com/castholm/SDL)), so there is nothing else to install.

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

Then build for any target from any host (Mac builds need a Mac with Xcode):

```sh
zig build -Doptimize=ReleaseSmall
zig build -Doptimize=ReleaseSmall -Dtarget=x86_64-windows-gnu
```

For the web, compile the files in `src/` together with the app with Emscripten and `-sUSE_SDL=3`, and use SDL's main callbacks (`SDL_MAIN_USE_CALLBACKS`) so the browser can run the loop.

## Deferred

Accessibility: gesso apps draw their own interface, so screen readers and other assistive technology see nothing in them. That is acceptable for games and personal tools but not for an app offered to the public. The likely route is [AccessKit](https://github.com/AccessKit/accesskit), which implements each platform's accessibility API and has C bindings: an app describes its interface as a tree of nodes, and AccessKit answers the screen reader. Revisit before a gesso app is offered as a general-purpose app, or as soon as someone who uses a screen reader wants to use one.

## Licence

MIT, see `LICENSE`. The stb libraries in `vendor/stb` are public domain (or MIT, at your choice); SDL3 is zlib; libopus (fetched by the build) is BSD.
