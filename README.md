# gesso

Gesso is the chalk ground a painter lays on a panel before painting: the layer every picture is built on. This is a small C library on SDL3 for lean native apps and games that build for Windows, Linux, the Steam Deck and the web from one codebase, with installs measured in megabytes.

## Modules

| Module | What it does |
|---|---|
| `gs_pix` | Indexed-colour frames in the VGA manner: Floyd-Steinberg dithering onto a palette, palette expansion to RGBA, colour-cycling ramps, highlight lookup tables |
| `gs_text` | Fonts through stb_truetype and a glyph atlas per SDL renderer; draws glyphs with an angle, a colour and a left-to-right reveal, or plain kerned UTF-8 lines |
| `gs_mix` | Audio output: one SDL stream summing any number of sources, each a function that adds stereo frames; the same mix renders offline for tests and exports |
| `gs_synth` | SoundFont 2 / SF3 synthesizer: zones, generators and modulators, envelopes, vibrato, low-pass filter, pan, loops, reverb |
| `gs_midi` | Standard MIDI Files and a sample-accurate, looping player that is a `gs_mix` source |
| `gs_rand.h` | Seeded generator, position hash and value noise, so every random-looking thing reproduces from a seed |

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

## Licence

MIT, see `LICENSE`. The stb libraries in `vendor/stb` are public domain (or MIT, at your choice); SDL3 is zlib.
