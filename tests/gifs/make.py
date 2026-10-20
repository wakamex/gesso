#!/usr/bin/env -S uv run --no-project --with pillow python3
"""Writes the GIFs tests/gif.c reads, and what each frame of anim.gif should look like.

anim.gif is 8 by 8, four frames with their own delays and colour tables: red all over; a blue square
top left, then disposed to the background; a green square bottom right with a transparent pixel,
then restored to the frame before it; a white square bottom left. Pillow composes the frames, with
the disposal and transparency the file gives, and anim.rgba holds them, 4 bytes a pixel, frame
after frame, for the test to compare with. still.gif is one frame. noise.gif is three interlaced
frames of 160 by 120 random pixels (interlaced by ImageMagick) in 256 colours, which fill the decoder's dictionary and make it
start again; noise.rgba holds them as Pillow reads them. moving.gif is five frames from ffmpeg, whose
encoder ends a frame's data inside a sub-block and leaves unchanged pixels transparent;
moving.rgba holds them as Pillow reads them."""
import os, random, subprocess
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
def frame(fill, box, color, hole=None):  # a palette frame: `fill` everywhere, `color` in box, one pixel left transparent
    im = Image.new("P", (8, 8), 0)
    im.putpalette(list(fill) + list(color) + [0, 0, 0] * 254)
    if box: im.paste(1, box)
    if hole: im.putpixel(hole, 2)
    return im
frames = [frame((255, 0, 0), None, (0, 0, 0)), frame((255, 0, 0), (0, 0, 4, 4), (0, 0, 255)),
          frame((255, 0, 0), (4, 4, 8, 8), (0, 200, 0), hole=(5, 5)), frame((255, 0, 0), (0, 4, 2, 6), (255, 255, 255))]
path = os.path.join(here, "anim.gif")
frames[0].save(path, save_all=True, append_images=frames[1:], duration=[50, 30, 40, 60], disposal=[1, 2, 3, 1], transparency=2, loop=0, optimize=False)
gif = Image.open(path)
with open(os.path.join(here, "anim.rgba"), "wb") as f:
    for i in range(4):
        gif.seek(i)
        f.write(gif.convert("RGBA").tobytes())
Image.new("RGB", (5, 3), (10, 20, 30)).save(os.path.join(here, "still.gif"))
random.seed(89)
noise = []
for i in range(3):
    im = Image.new("P", (160, 120))
    im.putpalette([random.randrange(256) for _ in range(768)])
    im.putdata([random.randrange(256) if random.random() < 0.7 else (x // 7) % 256 for y in range(120) for x in range(160)])
    noise.append(im)
path = os.path.join(here, "noise.gif")
noise[0].save(path, save_all=True, append_images=noise[1:], duration=[70, 80, 90], loop=0, optimize=False)
subprocess.run(["magick", path, "-interlace", "GIF", path], check=True)  # (Pillow writes no interlaced frames; ImageMagick rewrites them so)
gif = Image.open(path)
with open(os.path.join(here, "noise.rgba"), "wb") as f:
    for i in range(3):
        gif.seek(i)
        f.write(gif.convert("RGBA").tobytes())
path = os.path.join(here, "moving.gif")
subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=64x48:rate=10:duration=0.5",
                "-vf", "split[a][b];[a]palettegen[p];[b][p]paletteuse", path], check=True)
gif = Image.open(path)
with open(os.path.join(here, "moving.rgba"), "wb") as f:
    for i in range(gif.n_frames):
        gif.seek(i)
        f.write(gif.convert("RGBA").tobytes())
print(gif.n_frames, "frames in moving.gif")
