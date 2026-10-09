# Serves segments as a live HLS stream that never ends, adding a segment every PERIOD seconds to a
# six-segment window and marking a discontinuity each time the segments start over.
#   python3 live.py DIR PORT PERIOD [--rate KBPS[@SECONDS],...]
# DIR holds either 2 s transport stream segments (served at /live.m3u8) or one folder of them per
# rendition, each with `bandwidth` and `resolution` files, as ladder.sh makes (served at
# /master.m3u8 and /NAME/live.m3u8). A PERIOD above 2 publishes slower than real time, as a sound
# card running fast would see it. --rate caps the whole server's throughput like one network link,
# in kbit/s, changing at the given seconds after start: --rate 4000,800@40,4000@100.
import http.server
import os
import sys
import threading
import time

root, port, period = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
schedule = []  # (from second, bytes a second)
if len(sys.argv) > 5 and sys.argv[4] == "--rate":
    for step in sys.argv[5].split(","):
        kbps, _, at = step.partition("@")
        schedule.append((float(at or 0), float(kbps) * 1000 / 8))
start = time.time()
WINDOW = 6


def segments(folder):
    return sorted((f for f in os.listdir(folder) if f.endswith(".ts")), key=lambda f: (len(f), f))


renditions = {}  # name: (segments, bandwidth, resolution)
for name in sorted(os.listdir(root)):
    folder = os.path.join(root, name)
    if os.path.isfile(os.path.join(folder, "bandwidth")):
        read = lambda f: open(os.path.join(folder, f)).read().strip()
        renditions[name] = (segments(folder), int(read("bandwidth")), read("resolution"))
single = segments(root) if not renditions else []

link = threading.Lock()
link_free = 0.0  # when the simulated link has sent everything handed to it


def send(out, data):
    global link_free
    now = time.time()
    rate = next((r for at, r in reversed(schedule) if now - start >= at), None)
    if rate is None:
        out.write(data)
        return
    for i in range(0, len(data), 16384):
        chunk = data[i:i + 16384]
        with link:
            link_free = max(link_free, time.time()) + len(chunk) / rate
            due = link_free
        time.sleep(max(0.0, due - time.time()))
        out.write(chunk)


def window(names, prefix=""):
    newest = int((time.time() - start) / period) + WINDOW
    lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2", f"#EXT-X-MEDIA-SEQUENCE:{newest - WINDOW}"]
    for n in range(newest - WINDOW, newest):
        if n and n % len(names) == 0:
            lines.append("#EXT-X-DISCONTINUITY")
        lines += ["#EXTINF:2.000,", prefix + names[n % len(names)]]
    return lines


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=root, **k)

    def log_message(self, *a):
        pass

    def reply(self, body, kind):
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        send(self.wfile, body)

    def do_GET(self):
        path = self.path.split("?")[0].lstrip("/")
        playlist = "application/vnd.apple.mpegurl"
        if path == "live.m3u8" and single:
            return self.reply(("\n".join(window(single)) + "\n").encode(), playlist)
        if path == "master.m3u8" and renditions:
            lines = ["#EXTM3U"]
            for name, (_, bandwidth, resolution) in renditions.items():
                lines += [f'#EXT-X-STREAM-INF:BANDWIDTH={bandwidth},RESOLUTION={resolution},CODECS="avc1.4D401F,mp4a.40.2"', f"{name}/live.m3u8"]
            return self.reply(("\n".join(lines) + "\n").encode(), playlist)
        name, _, rest = path.partition("/")
        if rest == "live.m3u8" and name in renditions:
            return self.reply(("\n".join(window(renditions[name][0])) + "\n").encode(), playlist)
        try:
            with open(os.path.join(root, path), "rb") as f:
                data = f.read()
        except OSError:
            return self.send_error(404)
        self.reply(data, "video/mp2t")


http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
