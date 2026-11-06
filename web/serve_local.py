#!/usr/bin/env python3
"""Serves web/ for local testing, telling the browser not to reuse old files."""
import functools
import http.server
import sys
from pathlib import Path


class NoCache(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


port = int(sys.argv[1]) if len(sys.argv) > 1 else 8771
handler = functools.partial(NoCache, directory=str(Path(__file__).resolve().parent))
http.server.ThreadingHTTPServer(("127.0.0.1", port), handler).serve_forever()
