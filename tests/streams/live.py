# Serves a folder of 2 s transport stream segments as a live HLS stream that never ends, adding a
# segment every PERIOD seconds to a six-segment window and marking a discontinuity each time the
# segments start over. A PERIOD above 2 publishes slower than real time, as a sound card running
# fast would see it, so a player reaches the live edge and stalls:
#   python3 live.py a/ts 18770 2.04      then play http://127.0.0.1:18770/live.m3u8
import http.server
import os
import sys
import time

root, port, period = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
segments = sorted((f for f in os.listdir(root) if f.endswith(".ts")), key=lambda f: (len(f), f))
start = time.time()
WINDOW = 6


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=root, **k)

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.split("?")[0] != "/live.m3u8":
            return super().do_GET()
        newest = int((time.time() - start) / period) + WINDOW
        lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:2", f"#EXT-X-MEDIA-SEQUENCE:{newest - WINDOW}"]
        for n in range(newest - WINDOW, newest):
            if n and n % len(segments) == 0:
                lines.append("#EXT-X-DISCONTINUITY")
            lines += ["#EXTINF:2.000,", segments[n % len(segments)]]
        body = ("\n".join(lines) + "\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/vnd.apple.mpegurl")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
