#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Open CYMA-1 from this Mac: serve the page (instrument/dist/web) at http://localhost:8137 and open it in Chrome
(docs/CYMA-1-sounds-spec.md §9.1). There the page can use Web MIDI, so it can send sounds to an FM-1 running CYMA-1
and get them back; the copy hosted on claude.ai runs in a protected frame with no MIDI access.

    python3 instrument/tools/cyma_local.py [--port 8137] [--no-open] [--dir DIR] [LINK]

- localhost is a secure context, so the AudioWorklet and Web MIDI work. Only this computer can reach the page: it
  listens on the loopback address alone, serves files only (no folder listings, nothing outside the folder), and sends
  nothing anywhere.
- My Sounds is kept per address: the page at http://localhost:8137 has its own, separate from claude.ai's and from any
  other port. Files and codes move sounds between them (Save all my sounds as a file, then Import it here).
- Share links made here use the build's SHARE_BASE when it has one; otherwise they open this computer's copy only.
- If CYMA-1 is already being served on the port, this just opens it.
- LINK: a share link or code someone sent you (#cy1.... or #cy2...., or a whole message holding one). It opens here,
  in the local page, which plays it (nothing is saved until Keep it). This is how a long #cy1. link opens: claude.ai
  passes a link's anchor into its page only when it is at most 128 characters, and a cy1. code is 126-140.

Close the window (or press Ctrl+C) to stop. Exit 0 when stopped, 2 when the page is not built or the port is taken.
"""
import argparse
import functools
import http.server
import re
import shutil
import socket
import subprocess
import sys
import threading
import urllib.request
import webbrowser
from pathlib import Path

HERE = Path(__file__).resolve().parent
WEB = HERE.parent / "dist" / "web"
TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
         ".mjs": "text/javascript; charset=utf-8", ".wasm": "application/wasm", ".json": "application/json",
         ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml", ".png": "image/png", ".jpg": "image/jpeg",
         ".gif": "image/gif", ".mp4": "video/mp4", ".webm": "video/webm", ".mp3": "audio/mpeg", ".wav": "audio/wav",
         ".txt": "text/plain; charset=utf-8", ".ico": "image/x-icon", "": "application/octet-stream"}


class Handler(http.server.SimpleHTTPRequestHandler):
    """files from one folder, GET and HEAD only, never cached (a rebuilt page is picked up on reload)"""
    extensions_map = TYPES
    quiet = True

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        super().end_headers()

    def list_directory(self, path):
        self.send_error(404, "Not found")
        return None

    def log_message(self, fmt, *args):
        if not self.quiet:
            super().log_message(fmt, *args)


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True                   # (a quick restart: TIME_WAIT; macOS still refuses a second listener)


class Server6(Server):
    address_family = socket.AF_INET6


def already_cyma(port):
    """is CYMA-1 already being served on this port?"""
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/", timeout=1.5) as r:
            return b"CYMA-1" in r.read(65536)
    except Exception:
        return False


def say(msg, err=False):
    """print, with plain dashes where the terminal can't show the em dash"""
    f = sys.stderr if err else sys.stdout
    try:
        print(msg, file=f, flush=True)
    except UnicodeEncodeError:
        print(msg.replace("\u2014", "-"), file=f, flush=True)


# the anchor the page reads (its HASH_RE: cy1. with 12-240 token characters, cy2. with 10-240); a chat app's line breaks
# and invisible characters are removed first. The page decodes and checks it; this only passes it on.
LINK_RE = re.compile(r"(cy1\.[A-Za-z0-9_-]{12,240}|cy2\.[A-Za-z0-9_-]{10,240})(?![A-Za-z0-9_-])")


def link_anchor(text):
    """a link, a code or a message holding one -> "cy1...." / "cy2....", or None"""
    if not isinstance(text, str) or len(text) > 4096:
        return None
    s = "".join(c for c in text if not c.isspace() and c not in "\u00ad\u200b\u200c\u200d\u2060\ufeff")
    m = LINK_RE.search(s)
    return m.group(1) if m else None


def open_chrome(url):
    if sys.platform == "darwin" and shutil.which("open"):
        if subprocess.run(["open", "-a", "Google Chrome", url], capture_output=True).returncode == 0:
            return
    webbrowser.open(url)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="cyma_local.py", description="Serve CYMA-1's page at http://localhost:8137 "
                                 "and open it in Chrome, where it can talk to an FM-1 over USB (Web MIDI).")
    ap.add_argument("--port", type=int, default=8137, help="the port (default 8137; My Sounds is kept per port)")
    ap.add_argument("--no-open", action="store_true", help="don't open Chrome")
    ap.add_argument("--dir", default=str(WEB), help="the folder to serve (default instrument/dist/web)")
    ap.add_argument("--verbose", action="store_true", help="log every request")
    ap.add_argument("link", nargs="?", help="a share link or code (#cy1.... or #cy2....) to open in the local page")
    a = ap.parse_args(argv)
    root = Path(a.dir).expanduser().resolve()
    url = f"http://localhost:{a.port}"
    page = url
    if a.link is not None:
        anchor = link_anchor(a.link)
        if not anchor:
            print("That isn't a CYMA-1 link or code (they hold cy1. or cy2. and a run of letters and digits).",
                  file=sys.stderr)
            return 2
        page = f"{url}/#{anchor}"
    if not (root / "index.html").is_file():
        print(f"The page isn't built yet ({root / 'index.html'} is missing). Build it first:\n"
              f"  instrument/tools/run_all.sh --quick    (or: python3 instrument/tools/package_web.py)",
              file=sys.stderr)
        return 2
    if not 1 <= a.port <= 65535:
        print(f"--port {a.port}: 1..65535", file=sys.stderr)
        return 2
    Handler.quiet = not a.verbose
    handler = functools.partial(Handler, directory=str(root))
    try:
        httpd = Server(("127.0.0.1", a.port), handler)
    except OSError:
        if already_cyma(a.port):
            say(f"CYMA-1 is already running at {url} \u2014 opening it.")
            if a.link is not None:
                say(f"The shared sound: {page}")
            if not a.no_open:
                open_chrome(page)
            return 0
        print(f"Port {a.port} is in use by another program. Try --port {a.port + 1} (My Sounds is kept per port, so "
              f"keep using the same one).", file=sys.stderr)
        return 2
    servers = [httpd]
    try:                                         # (Chrome tries ::1 for localhost too; IPv4 alone also works)
        servers.append(Server6(("::1", a.port), handler))
    except OSError:
        pass
    for s in servers[1:]:
        threading.Thread(target=s.serve_forever, daemon=True).start()
    say(f"CYMA-1 is at {url} \u2014 open it in Chrome. Close this window to stop.")
    if a.link is not None:
        say(f"The shared sound: {page}")
    if not a.no_open:
        open_chrome(page)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        for s in servers:
            s.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
