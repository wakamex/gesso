#!/usr/bin/env -S uv run --no-project --with playwright python3
"""Checks gesso's web behaviour in real browsers, on the page tests/web/build.sh makes:
the browser's own keys (F5, Ctrl+R, F11, F12, Ctrl and +) reach the browser instead of being
cancelled by SDL, a key the page uses (Ctrl+A) is still the page's, copying a field's text with
Ctrl+A and Ctrl+C puts it on the browser's clipboard (Chromium, where the clipboard can be read),
and the README's redraw on resize (an SDL event watch) leaves no cleared canvas for the browser to
show: a pixel read inside the browser's resize event, after SDL's resize, is the page's colour,
and without the watch (?watch=0) it is the cleared canvas.

    tests/web/check.py OUT_DIR        (OUT_DIR as given to build.sh)"""
import functools, http.server, sys, threading
from playwright.sync_api import sync_playwright

out = sys.argv[1]
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Quiet, directory=out))
threading.Thread(target=server.serve_forever, daemon=True).start()
url = f"http://127.0.0.1:{server.server_address[1]}/index.html"

# A key dispatched on the canvas, as the browser would send it: true when nothing cancelled it.
DISPATCH = """([key, code, ctrl]) => document.getElementById('canvas').dispatchEvent(
    new KeyboardEvent('keydown', { key, code, ctrlKey: ctrl, bubbles: true, cancelable: true }))"""
keys = [("F5", "F5", False), ("r", "KeyR", True), ("F11", "F11", False), ("F12", "F12", False), ("=", "Equal", True), ("-", "Minus", True)]
failures = 0
with sync_playwright() as p:
    for name in ("chromium", "firefox"):
        browser = getattr(p, name).launch()
        context = browser.new_context()
        if name == "chromium": context.grant_permissions(["clipboard-read", "clipboard-write"])
        page = context.new_page()
        page.goto(url)
        page.wait_for_function("document.title === 'ready'", timeout=30000)
        page.wait_for_timeout(300)
        for key, code, ctrl in keys:
            passed = page.evaluate(DISPATCH, [key, code, ctrl])
            label = ("Ctrl+" if ctrl else "") + key
            print(f"{name}: {label} reaches the browser: {passed}")
            failures += not passed
        kept = not page.evaluate(DISPATCH, ["a", "KeyA", True])
        print(f"{name}: Ctrl+A is still the page's: {kept}")
        failures += not kept
        if name == "chromium":
            page.mouse.click(100, 35)
            page.keyboard.type("hello web")
            page.wait_for_timeout(100)
            page.keyboard.press("Control+a")
            page.keyboard.press("Control+c")
            page.wait_for_timeout(300)
            got = page.evaluate("navigator.clipboard.readText()")
            print(f"{name}: the clipboard holds {got!r}")
            failures += got != "hello web"
        # The pixel at (5, 5) as it stands inside the resize event, after SDL has resized the canvas.
        for watch in (True, False):
            page.goto(url + ("" if watch else "?watch=0"))
            page.wait_for_function("document.title === 'ready'", timeout=30000)
            page.wait_for_timeout(300)
            page.evaluate("""() => window.addEventListener('resize', () => {
                const c = document.getElementById('canvas');
                window.seen = Array.from(c.getContext('2d').getImageData(5, 5, 1, 1).data);
            })""")
            size = page.viewport_size
            page.set_viewport_size({"width": size["width"] - 37, "height": size["height"] - 23})
            page.wait_for_function("window.seen !== undefined", timeout=5000)
            seen = page.evaluate("window.seen")
            drawn = seen[:3] == [20, 40, 60] and seen[3] == 255
            print(f"{name}: inside the resize, {'with' if watch else 'without'} the watch, the canvas shows {seen}")
            failures += drawn != watch
            page.set_viewport_size(size)
        browser.close()
server.shutdown()
print("ok" if not failures else f"{failures} failed")
sys.exit(1 if failures else 0)
